/*
 * Window related functions
 *
 * Copyright 1993, 1994, 1995, 1996, 2001, 2013-2017 Alexandre Julliard
 * Copyright 1993 David Metcalfe
 * Copyright 1995, 1996 Alex Korobka
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include "waylanddrv.h"

#include "wingdi.h"
#include "winuser.h"

#include "wine/debug.h"
#include "wine/heap.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

/* private window data */
struct wayland_win_data
{
    /* hwnd that this private data belongs to */
    HWND           hwnd;
    /* parent hwnd for child windows */
    HWND           parent;
    /* effective parent hwnd (what the driver considers to
     * be the parent for relative positioning) */
    HWND           effective_parent;
    /* USER window rectangle relative to parent */
    RECT           window_rect;
    /* client area relative to parent */
    RECT           client_rect;
    /* wayland surface (if any) representing this window on the wayland side */
    struct wayland_surface *wayland_surface;
    /* wine window_surface backing this window */
    struct window_surface *window_surface;
    /* pending wine window_surface for this window */
    struct window_surface *pending_window_surface;
    /* whether the pending_window_surface value is valid */
    BOOL           has_pending_window_surface;
    /* whether this window is currently being resized */
    BOOL           resizing;
    /* the window_rect this window should be restored to after unmaximizing */
    RECT           restore_rect;
    /* whether the window is currently fullscreen */
    BOOL           fullscreen;
    /* whether the window is currently maximized */
    BOOL           maximized;
    /* whether we are currently handling a wayland configure event */
    BOOL           handling_wayland_configure_event;
    /* the configure flags for the configure event we are handling */
    enum wayland_configure_flags wayland_configure_event_flags;
    /* whether this window is visible */
    BOOL           visible;
    /* Save previous state to be able to decide when to recreate wayland surface */
    HWND           old_parent;
    RECT           old_window_rect;
    /* whether a wayland surface update is needed */
    BOOL           wayland_surface_needs_update;
    /* Whether we have a pending/unprocessed WM_WAYLAND_STATE_UPDATE message */
    BOOL           pending_state_update_message;
};

static CRITICAL_SECTION win_data_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &win_data_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": win_data_section") }
};
static CRITICAL_SECTION win_data_section = {&critsect_debug, -1, 0, 0, 0, 0};

static struct wayland_win_data *win_data_context[32768];

static inline int context_idx(HWND hwnd)
{
    return LOWORD(hwnd) >> 1;
}

/***********************************************************************
 *           wayland_win_data_destroy
 */
static void wayland_win_data_destroy(struct wayland_win_data *data)
{
    TRACE("hwnd=%p\n", data->hwnd);
    win_data_context[context_idx(data->hwnd)] = NULL;

    if (data->has_pending_window_surface && data->pending_window_surface)
    {
        wayland_window_surface_update_wayland_surface(data->pending_window_surface, NULL);
        window_surface_release(data->pending_window_surface);
    }
    if (data->window_surface)
    {
        wayland_window_surface_update_wayland_surface(data->window_surface, NULL);
        window_surface_release(data->window_surface);
    }
    if (data->wayland_surface) wayland_surface_unref(data->wayland_surface);
    heap_free(data);

    LeaveCriticalSection(&win_data_section);
}

/***********************************************************************
 *           wayland_win_data_get
 *
 * Lock and return the data structure associated with a window.
 */
static struct wayland_win_data *wayland_win_data_get(HWND hwnd)
{
    struct wayland_win_data *data;

    if (!hwnd) return NULL;

    EnterCriticalSection(&win_data_section);
    if ((data = win_data_context[context_idx(hwnd)]) && data->hwnd == hwnd)
        return data;
    LeaveCriticalSection(&win_data_section);

    return NULL;
}

/***********************************************************************
 *           wayland_win_data_release
 *
 * Release the data returned by wayland_win_data_get.
 */
static void wayland_win_data_release(struct wayland_win_data *data)
{
    if (data) LeaveCriticalSection(&win_data_section);
}

/***********************************************************************
 *           wayland_win_data_create
 *
 * Create a data window structure for an existing window.
 */
static struct wayland_win_data *wayland_win_data_create(HWND hwnd)
{
    struct wayland_win_data *data;
    HWND parent;

    /* Don't create win data for desktop or HWND_MESSAGE windows. */
    if (!(parent = GetAncestor(hwnd, GA_PARENT))) return NULL;
    if (parent != GetDesktopWindow() && !GetAncestor(parent, GA_PARENT)) return NULL;

    if (!(data = heap_alloc_zero(sizeof(*data))))
        return NULL;

    data->hwnd = hwnd;
    data->wayland_surface_needs_update = TRUE;

    EnterCriticalSection(&win_data_section);
    win_data_context[context_idx(hwnd)] = data;

    TRACE("hwnd=%p\n", data->hwnd);

    return data;
}

/***********************************************************************
 *           wayland_surface_for_hwnd_lock
 *
 *  Gets the wayland surface for HWND while locking the private window data.
 */
static struct wayland_surface *wayland_surface_for_hwnd_lock(HWND hwnd)
{
    struct wayland_win_data *data = wayland_win_data_get(hwnd);

    if (data && data->wayland_surface)
        return data->wayland_surface;

    wayland_win_data_release(data);

    return NULL;
}

/***********************************************************************
 *           wayland_surface_for_hwnd_unlock
 */
static void wayland_surface_for_hwnd_unlock(struct wayland_surface *surface)
{
    if (surface) LeaveCriticalSection(&win_data_section);
}

static HWND guess_popup_parent(struct wayland *wayland)
{
    HWND cursor_hwnd;
    HWND keyboard_hwnd;
    POINT cursor;

    GetCursorPos(&cursor);
    cursor_hwnd = wayland->pointer.focused_surface ?
                  wayland->pointer.focused_surface->hwnd :
                  WindowFromPoint(cursor);
    cursor_hwnd = GetAncestor(cursor_hwnd, GA_ROOT);
    keyboard_hwnd = GetFocus();

    TRACE("cursor_hwnd=%p keyboard_hwnd=%p\n", cursor_hwnd, keyboard_hwnd);

    /* If we have a recent mouse event, the popup parent is likely the window
     * under the cursor, so prefer it. Otherwise prefer the window with
     * the keyboard focus. */
    if (wayland->last_event_type == INPUT_MOUSE)
        return cursor_hwnd ? cursor_hwnd : keyboard_hwnd;
    else
        return keyboard_hwnd ? keyboard_hwnd : cursor_hwnd;
}

/* Whether we consider this window to be a transient popup, so we can
 * display it as a Wayland subsurface with relative positioning. */
static BOOL wayland_win_data_can_be_popup(struct wayland_win_data *data)
{
    DWORD style;
    HMONITOR hmonitor;
    MONITORINFO mi;
    double monitor_width;
    double monitor_height;
    int window_width;
    int window_height;

    style = GetWindowLongW(data->hwnd, GWL_STYLE);

    /* Child windows can't be popups, unless they are children of the desktop
     * (thus effectively top-level). */
    if ((style & WS_CHILD) && GetWindowLongPtrW(data->hwnd, GWLP_HWNDPARENT))
    {
        TRACE("hwnd=%p is child => FALSE\n", data->hwnd);
        return FALSE;
    }

    /* If the window has top bar elements, don't consider it a popup candidate. */
    if ((style & WS_CAPTION) == WS_CAPTION ||
        (style & (WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)))
    {
        TRACE("hwnd=%p style=0x%08x => FALSE\n", data->hwnd, style);
        return FALSE;
    }

    mi.cbSize = sizeof(mi);
    if (!(hmonitor = MonitorFromRect(&data->window_rect, MONITOR_DEFAULTTOPRIMARY)) ||
        !GetMonitorInfoW(hmonitor, &mi))
    {
        SetRectEmpty(&mi.rcMonitor);
    }

    monitor_width = mi.rcMonitor.right - mi.rcMonitor.left;
    monitor_height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    window_width = data->window_rect.right - data->window_rect.left;
    window_height = data->window_rect.bottom - data->window_rect.top;

    /* If the window has an unreasonably small size or is too large, don't consider
     * it a popup candidate. */
    if (window_width <= 1 || window_height <= 1 ||
        window_width * window_height > 0.5 * monitor_width * monitor_height)
    {
        TRACE("hwnd=%p window=%s monitor=%s => FALSE\n",
              data->hwnd, wine_dbgstr_rect(&data->window_rect),
              wine_dbgstr_rect(&mi.rcMonitor));
        return FALSE;
    }

    TRACE("hwnd=%p style=0x%08x window=%s monitor=%s => TRUE\n",
          data->hwnd, style, wine_dbgstr_rect(&data->window_rect),
          wine_dbgstr_rect(&mi.rcMonitor));

    return TRUE;
}

static HWND wayland_win_data_get_effective_parent(struct wayland_win_data *data)
{
    struct wayland *wayland = thread_init_wayland();
    /* GWLP_HWNDPARENT gets the owner for any kind of toplevel windows,
     * and the parent for child windows. */
    HWND parent_hwnd = (HWND)GetWindowLongPtrW(data->hwnd, GWLP_HWNDPARENT);
    struct wayland_surface *parent_surface = NULL;
    HWND effective_parent_hwnd;

    /* If we don't track the parent Wayland surface, we can't use the parent
     * window as the effective parent. */
    if (parent_hwnd && !(parent_surface = wayland_surface_for_hwnd_lock(parent_hwnd)))
        parent_hwnd = 0;
    wayland_surface_for_hwnd_unlock(parent_surface);

    /* Many applications use top level, unowned (or owned by the desktop)
     * popup windows for menus and tooltips and depend on screen
     * coordinates for correct positioning. Since wayland can't deal with
     * screen coordinates, try to guess the effective parent window of such
     * popups and manage them as wayland subsurfaces. */
    if (!parent_hwnd && wayland_win_data_can_be_popup(data))
    {
        effective_parent_hwnd = guess_popup_parent(wayland);
        if (effective_parent_hwnd == data->hwnd ||
            effective_parent_hwnd == GetDesktopWindow())
        {
            effective_parent_hwnd = 0;
        }
    }
    else
    {
        effective_parent_hwnd = parent_hwnd;
    }

    TRACE("hwnd=%p parent=%p effective_parent=%p\n",
          data->hwnd, parent_hwnd, effective_parent_hwnd);

    return effective_parent_hwnd;
}

static BOOL wayland_win_data_wayland_surface_needs_update(struct wayland_win_data *data)
{
    if (data->wayland_surface_needs_update)
        return TRUE;

    /* Change of parentage (either actual or effective) requires recreating the
     * whole win_data to ensure we have a properly owned wayland surface. We
     * check for change of effective parent only if the window changed in any
     * way, to avoid spuriously reassigning parent windows when new windows
     * are created. */
    if ((!EqualRect(&data->window_rect, &data->old_window_rect) &&
         data->effective_parent != wayland_win_data_get_effective_parent(data)) ||
        data->parent != data->old_parent)
    {
        return TRUE;
    }

    /* If this is currently or potentially a toplevel surface, and its
     * visibility state has changed, recreate win_data so that we only have
     * xdg_toplevels for visible windows. */
    if (data->wayland_surface && !data->wayland_surface->wl_subsurface)
    {
        BOOL visible = data->wayland_surface->xdg_toplevel != NULL;
        if (data->visible != visible)
            return TRUE;
    }

    return FALSE;
}

static void wayland_win_data_update_wayland_surface(struct wayland_win_data *data)
{
    struct wayland *wayland;
    HWND effective_parent_hwnd;
    struct wayland_surface *parent_surface;

    TRACE("hwnd=%p\n", data->hwnd);

    data->wayland_surface_needs_update = FALSE;

    wayland = thread_init_wayland();

    if (data->wayland_surface)
    {
        struct wayland_surface *child;

        /* Dependent Wayland surfaces (either subsurfaces or xdg children)
         * require an update, so that they point to the updated surface.
         * TODO: Ensure that an update is eventually triggered. */
        EnterCriticalSection(&data->wayland_surface->crit);
        wl_list_for_each(child, &data->wayland_surface->child_list, link)
        {
            struct wayland_win_data *child_data;
            if ((child_data = wayland_win_data_get(child->hwnd)))
            {
                child_data->wayland_surface_needs_update = TRUE;
                wayland_win_data_release(child_data);
            }
        }
        LeaveCriticalSection(&data->wayland_surface->crit);

        wayland_surface_unref(data->wayland_surface);
        data->wayland_surface = NULL;
    }

    effective_parent_hwnd = wayland_win_data_get_effective_parent(data);
    parent_surface = NULL;

    if (effective_parent_hwnd)
    {
        parent_surface = wayland_surface_for_hwnd_lock(effective_parent_hwnd);
        wayland_surface_for_hwnd_unlock(parent_surface);
    }

    data->effective_parent = effective_parent_hwnd;

    /* Reset window state, so that it can be properly applied again. */
    data->maximized = FALSE;
    data->fullscreen = FALSE;

    /* Use wayland subsurfaces for children windows and toplevels that we
     * consider to be popups and have an effective parent. Otherwise, if the
     * window is visible make it wayland toplevel. Finally, if the window is
     * not visible create a plain (without a role) surface to avoid polluting
     * the compositor with empty xdg_toplevels. */
    if (parent_surface && (data->parent || wayland_win_data_can_be_popup(data)))
        data->wayland_surface = wayland_surface_create_subsurface(wayland, parent_surface);
    else if (data->visible)
        data->wayland_surface = wayland_surface_create_toplevel(wayland, parent_surface);
    else
        data->wayland_surface = wayland_surface_create_plain(wayland);

    if (data->wayland_surface)
        data->wayland_surface->hwnd = data->hwnd;
}

static BOOL wayland_win_data_update_wayland_xdg_state(struct wayland_win_data *data)
{
    int wayland_width, wayland_height;
    BOOL compat_with_current = FALSE;
    BOOL compat_with_pending = FALSE;
    int width = data->window_rect.right - data->window_rect.left;
    int height = data->window_rect.bottom - data->window_rect.top;
    struct wayland_surface *wsurface = data->wayland_surface;
    enum wayland_configure_flags conf_flags = 0;
    DWORD style = GetWindowLongW(data->hwnd, GWL_STYLE);
    HMONITOR hmonitor;
    MONITORINFOEXW mi;
    struct wayland_output *output;
    RECT window_in_monitor;

    mi.cbSize = sizeof(mi);
    if ((hmonitor = MonitorFromWindow(data->hwnd, MONITOR_DEFAULTTOPRIMARY)) &&
        GetMonitorInfoW(hmonitor, (MONITORINFO *)&mi))
    {
        output = wayland_output_get_by_wine_name(wsurface->wayland, mi.szDevice);
    }
    else
    {
        output = NULL;
        SetRectEmpty(&mi.rcMonitor);
    }

    TRACE("hwnd=%p window=%dx%d monitor=%dx%d maximized=%d fullscreen=%d handling_event=%d\n",
          data->hwnd, width, height,
          mi.rcMonitor.right - mi.rcMonitor.left,
          mi.rcMonitor.bottom - mi.rcMonitor.top,
          data->maximized, data->fullscreen, data->handling_wayland_configure_event);

    wayland_surface_set_wine_output(data->wayland_surface, output);

    /* If we are currently handling a wayland configure event (i.e., we are
     * being called through handle_wm_wayland_configure() -> SetWindowPos()),
     * use the event configure flags directly. Otherwise try to infer the flags
     * from the window style and rectangle. */
    if (data->handling_wayland_configure_event)
    {
        conf_flags = data->wayland_configure_event_flags;
    }
    else
    {
        /* Set the wayland fullscreen state if the window rect covers the
         * current monitor exactly. Note that we set/maintain the fullscreen
         * wayland state, even if the window style is also maximized. */
        if (!IsRectEmpty(&mi.rcMonitor) &&
            IntersectRect(&window_in_monitor, &data->window_rect, &mi.rcMonitor) &&
            EqualRect(&window_in_monitor, &mi.rcMonitor) &&
            !(style & (WS_MINIMIZE|WS_CAPTION)))
        {
            conf_flags |= WAYLAND_CONFIGURE_FLAG_FULLSCREEN;
        }
        if (style & WS_MAXIMIZE)
        {
            conf_flags |= WAYLAND_CONFIGURE_FLAG_MAXIMIZED;
        }
    }

    /* First do all state unsettings, before setting new state. Some wayland
     * compositors misbehave if the order is reversed. */
    if (data->maximized && !(conf_flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED))
    {
        if (!data->handling_wayland_configure_event)
            xdg_toplevel_unset_maximized(wsurface->xdg_toplevel);
        data->maximized = FALSE;
    }

    if (data->fullscreen && !(conf_flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN))
    {
        if (!data->handling_wayland_configure_event)
            xdg_toplevel_unset_fullscreen(wsurface->xdg_toplevel);
        data->fullscreen = FALSE;
    }

    if (!data->maximized && (conf_flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED))
    {
        if (!data->handling_wayland_configure_event)
            xdg_toplevel_set_maximized(wsurface->xdg_toplevel);
        data->maximized = TRUE;
    }

   /* Set the fullscreen state after the maximized state on the wayland surface
    * to ensure compositors apply the final fullscreen state properly. */
    if (!data->fullscreen && (conf_flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN))
    {
        if (!data->handling_wayland_configure_event)
        {
            xdg_toplevel_set_fullscreen(wsurface->xdg_toplevel,
                                        output ? output->wl_output : NULL);
        }
        data->fullscreen = TRUE;
    }

    if (!(conf_flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN) &&
        !(conf_flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED) &&
        !(style & WS_MINIMIZE))
    {
        data->restore_rect = data->window_rect;
        TRACE("setting hwnd=%p restore_rect=%s\n",
              data->hwnd, wine_dbgstr_rect(&data->restore_rect));
    }

    TRACE("hwnd=%p current state maximized=%d fullscreen=%d\n",
          data->hwnd, data->maximized, data->fullscreen);

    wayland_surface_coords_rounded_from_wine(wsurface, width, height,
                                             &wayland_width, &wayland_height);

    if (wsurface->current.serial &&
        wayland_surface_configure_is_compatible(&wsurface->current,
                                                wayland_width, wayland_height,
                                                conf_flags))
    {
        compat_with_current = TRUE;
    }

    if (wsurface->pending.serial &&
        wayland_surface_configure_is_compatible(&wsurface->pending,
                                                wayland_width, wayland_height,
                                                conf_flags))
    {
        compat_with_pending = TRUE;
    }

    TRACE("current conf serial=%d size=%dx%d flags=%#x\n compat=%d\n",
          wsurface->current.serial, wsurface->current.width,
          wsurface->current.height, wsurface->current.configure_flags,
          compat_with_current);
    TRACE("pending conf serial=%d size=%dx%d flags=%#x compat=%d\n",
          wsurface->pending.serial, wsurface->pending.width,
          wsurface->pending.height, wsurface->pending.configure_flags,
          compat_with_pending);

    /* Only update the wayland surface state to match the window
     * configuration if the surface can accept the new config, in order to
     * avoid transient states that may cause glitches. */
    if (!compat_with_pending && !compat_with_current)
    {
        TRACE("hwnd=%p window state not compatible with current or "
              "pending wayland surface configuration\n", data->hwnd);
        wsurface->drawing_allowed = FALSE;
        return FALSE;
    }

    if (compat_with_pending)
        wayland_surface_ack_pending_configure(wsurface);

    return TRUE;
}

static void wayland_win_data_get_rect_in_monitor(struct wayland_win_data *data,
                                                 enum wayland_configure_flags flags,
                                                 RECT *rect)
{
    HMONITOR hmonitor;
    MONITORINFO mi;
    RECT *area = NULL;

    mi.cbSize = sizeof(mi);
    if ((hmonitor = MonitorFromWindow(data->hwnd, MONITOR_DEFAULTTOPRIMARY)) &&
        GetMonitorInfoW(hmonitor, (MONITORINFO *)&mi))
    {
        if (flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN)
            area = &mi.rcMonitor;
        else if (flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN)
            area = &mi.rcWork;
    }

    if (area)
    {
        IntersectRect(rect, area, &data->window_rect);
        OffsetRect(rect, -data->window_rect.left, -data->window_rect.top);
    }
    else
    {
        SetRectEmpty(rect);
    }
}

static void wayland_win_data_get_compatible_rect(struct wayland_win_data *data,
                                                 RECT *rect)
{
    int width = data->window_rect.right - data->window_rect.left;
    int height = data->window_rect.bottom - data->window_rect.top;
    int wine_conf_width, wine_conf_height;
    enum wayland_configure_flags conf_flags =
        data->wayland_surface->current.configure_flags;

    /* Get the window size corresponding to the Wayland surfaces configuration. */
    wayland_surface_coords_to_wine(data->wayland_surface,
                                   data->wayland_surface->current.width,
                                   data->wayland_surface->current.height,
                                   &wine_conf_width,
                                   &wine_conf_height);

    /* If Wayland requires a surface size smaller than what wine provides,
     * use part of the window contents for the surface. */
    if (((conf_flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED) ||
         (conf_flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN)) &&
        (width > wine_conf_width || height > wine_conf_height))
    {
        wayland_win_data_get_rect_in_monitor(data, conf_flags, rect);
        /* If the window rect in the monitor is smaller than required
         * fall back to an appropriately sized rect at the top-left. */
        if (rect->right - rect->left < wine_conf_width ||
            rect->bottom - rect->top < wine_conf_height)
        {
            SetRect(rect, 0, 0, wine_conf_width, wine_conf_height);
        }
        else
        {
            rect->right = min(rect->right, rect->left + wine_conf_width);
            rect->bottom = min(rect->bottom, rect->top + wine_conf_height);
        }
        TRACE("Window is too large for wayland state, using subarea\n");
    }
    else
    {
        SetRect(rect, 0, 0, width, height);
    }
}

static void wayland_win_data_update_wayland_surface_state(struct wayland_win_data *data)
{
    RECT screen_rect;
    RECT parent_screen_rect;
    int width = data->window_rect.right - data->window_rect.left;
    int height = data->window_rect.bottom - data->window_rect.top;
    struct wayland_surface *wsurface = data->wayland_surface;
    DWORD style = GetWindowLongW(data->hwnd, GWL_STYLE);

    TRACE("hwnd=%p window=%dx%d style=0x%x\n", data->hwnd, width, height, style);

    if (!(style & WS_VISIBLE))
    {
        wayland_surface_unmap(wsurface);
        return;
    }

    /* Lock the wayland surface to avoid commits from other threads while we
     * are setting up the new state. */
    EnterCriticalSection(&wsurface->crit);

    if (wsurface->xdg_toplevel &&
        !wayland_win_data_update_wayland_xdg_state(data))
    {
        LeaveCriticalSection(&wsurface->crit);
        return;
    }

    wayland_surface_reconfigure_size(wsurface, width, height);

    if (wsurface->wl_subsurface)
    {
        /* In addition to children windows, we manage some top level, popup window
         * with subsurfaces (see wayland_win_data_get_effective_parent), which use
         * coordinates relative to their parent surface. */
        if (!GetWindowRect(data->hwnd, &screen_rect))
            SetRectEmpty(&screen_rect);
        if (!GetWindowRect(data->effective_parent, &parent_screen_rect))
            SetRectEmpty(&parent_screen_rect);

        wayland_surface_reconfigure_position(
            wsurface,
            screen_rect.left - parent_screen_rect.left,
            screen_rect.top - parent_screen_rect.top);
    }
    else if (wsurface->xdg_surface)
    {
        RECT compat;
        wayland_win_data_get_compatible_rect(data, &compat);
        wayland_surface_reconfigure_geometry(wsurface, compat.left, compat.top,
                                             compat.right - compat.left,
                                             compat.bottom - compat.top);
    }

    wayland_surface_reconfigure_apply(wsurface);

    if (wsurface->xdg_toplevel || wsurface->wl_subsurface)
        wsurface->drawing_allowed = TRUE;

    LeaveCriticalSection(&wsurface->crit);
}

static void update_wayland_state(struct wayland_win_data *data)
{
    if (data->has_pending_window_surface)
    {
        if (data->window_surface)
        {
            if (data->window_surface != data->pending_window_surface)
                wayland_window_surface_update_wayland_surface(data->window_surface, NULL);
            window_surface_release(data->window_surface);
        }
        data->window_surface = data->pending_window_surface;
        data->has_pending_window_surface = FALSE;
        data->pending_window_surface = NULL;
    }

    if (wayland_win_data_wayland_surface_needs_update(data))
        wayland_win_data_update_wayland_surface(data);

    if (data->wayland_surface)
        wayland_win_data_update_wayland_surface_state(data);

    if (data->window_surface)
    {
        wayland_window_surface_update_wayland_surface(data->window_surface,
                                                      data->wayland_surface);
        if (wayland_window_surface_needs_flush(data->window_surface))
            wayland_window_surface_flush(data->window_surface);
    }
}

/**********************************************************************
 *           WAYLAND_CreateWindow
 */
BOOL CDECL WAYLAND_CreateWindow(HWND hwnd)
{
    TRACE("%p\n", hwnd);

    if (hwnd == GetDesktopWindow())
    {
        /* Initialize wayland so that the desktop process has access
         * to all the wayland related information (e.g., displays). */
        wayland_init_thread_data();
    }

    return TRUE;
}

/***********************************************************************
 *           WAYLAND_DestroyWindow
 */
void CDECL WAYLAND_DestroyWindow(HWND hwnd)
{
    struct wayland_win_data *data;

    TRACE("%p\n", hwnd);

    if (!(data = wayland_win_data_get(hwnd))) return;
    wayland_win_data_destroy(data);
}

/***********************************************************************
 *           WAYLAND_WindowPosChanging
 */
BOOL CDECL WAYLAND_WindowPosChanging(HWND hwnd, HWND insert_after, UINT swp_flags,
                                     const RECT *window_rect, const RECT *client_rect,
                                     RECT *visible_rect, struct window_surface **surface)
{
    struct wayland_win_data *data = wayland_win_data_get(hwnd);
    BOOL exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
    HWND parent = GetAncestor(hwnd, GA_PARENT);
    RECT surface_rect;
    DWORD flags;
    COLORREF color_key;
    BYTE alpha;

    TRACE("win %p window %s client %s visible %s style %08x ex %08x flags %08x after %p\n",
          hwnd, wine_dbgstr_rect(window_rect), wine_dbgstr_rect(client_rect),
          wine_dbgstr_rect(visible_rect), style, exstyle, swp_flags, insert_after);

    if (!data && !(data = wayland_win_data_create(hwnd))) return TRUE;

    data->old_parent = data->parent;
    data->old_window_rect = data->window_rect;
    data->parent = (parent == GetDesktopWindow()) ? 0 : parent;
    data->window_rect = *window_rect;
    data->client_rect = *client_rect;
    data->visible = (style & WS_VISIBLE) == WS_VISIBLE || (swp_flags & SWP_SHOWWINDOW);

    /* Release the dummy surface wine provides for toplevels. */
    if (*surface) window_surface_release(*surface);
    *surface = NULL;

    /* Check if we don't want a dedicated window surface. */
    if (data->parent || (swp_flags & SWP_HIDEWINDOW) || !data->visible) goto done;

    surface_rect = *window_rect;
    OffsetRect(&surface_rect, -surface_rect.left, -surface_rect.top);

    /* Check if we can reuse our current window surface. */
    if (data->window_surface &&
        EqualRect(&data->window_surface->rect, &surface_rect))
    {
        window_surface_add_ref(data->window_surface);
        *surface = data->window_surface;
        TRACE("reusing surface %p\n", *surface);
        goto done;
    }

    /* Create new window surface. */
    color_key = alpha = flags = 0;
    if (!(exstyle & WS_EX_LAYERED) ||
        !GetLayeredWindowAttributes(hwnd, &color_key, &alpha, &flags))
    {
        flags = 0;
    }
    if (!(flags & LWA_COLORKEY)) color_key = CLR_INVALID;
    if (!(flags & LWA_ALPHA)) alpha = 255;

    *surface = wayland_window_surface_create(data->hwnd, &surface_rect, color_key, alpha, FALSE);

done:
    wayland_win_data_release(data);
    return TRUE;
}

/***********************************************************************
 *           WAYLAND_WindowPosChanged
 */
void CDECL WAYLAND_WindowPosChanged(HWND hwnd, HWND insert_after, UINT swp_flags,
                                    const RECT *window_rect, const RECT *client_rect,
                                    const RECT *visible_rect, const RECT *valid_rects,
                                    struct window_surface *surface)
{
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(hwnd))) return;

    TRACE("hwnd %p window %s client %s visible %s style %08x after %p flags %08x\n",
          hwnd, wine_dbgstr_rect(window_rect), wine_dbgstr_rect(client_rect),
          wine_dbgstr_rect(visible_rect), GetWindowLongW(hwnd, GWL_STYLE),
          insert_after, swp_flags);

    if (surface) window_surface_add_ref(surface);
    if (data->has_pending_window_surface && data->pending_window_surface)
        window_surface_release(data->pending_window_surface);
    data->pending_window_surface = surface;
    data->has_pending_window_surface = TRUE;

    /* In some cases, notably when the app calls UpdateLayeredWindow, position
     * and size changes may be emitted from a thread other than the window
     * thread. Since in the current implementation updating the wayland state
     * needs to happen in the context of the window thread to avoid racy
     * interactions, post a message to update the state in the right thread. */
    if (GetCurrentThreadId() == GetWindowThreadProcessId(hwnd, NULL))
    {
        update_wayland_state(data);
    }
    else if (!data->pending_state_update_message)
    {
        PostMessageW(hwnd, WM_WAYLAND_STATE_UPDATE, 0, 0);
        data->pending_state_update_message = TRUE;
    }

    wayland_win_data_release(data);
}

/***********************************************************************
 *           WAYLAND_ShowWindow
 */
UINT CDECL WAYLAND_ShowWindow(HWND hwnd, INT cmd, RECT *rect, UINT swp)
{
    struct wayland_surface *wsurface;

    TRACE("hwnd=%p cmd=%d\n", hwnd, cmd);

    if (IsRectEmpty(rect)) return swp;
    if (!IsIconic(hwnd)) return swp;
    /* always hide icons off-screen */
    if (rect->left != -32000 || rect->top != -32000)
    {
        OffsetRect(rect, -32000 - rect->left, -32000 - rect->top);
        swp &= ~(SWP_NOMOVE | SWP_NOCLIENTMOVE);
    }

    if ((wsurface = wayland_surface_for_hwnd_lock(hwnd)) && wsurface->xdg_toplevel)
        xdg_toplevel_set_minimized(wsurface->xdg_toplevel);

    wayland_surface_for_hwnd_unlock(wsurface);

    return swp;
}

/***********************************************************************
 *           WAYLAND_SetWindowRgn
 */
void CDECL WAYLAND_SetWindowRgn(HWND hwnd, HRGN hrgn, BOOL redraw)
{
    struct wayland_win_data *data;

    TRACE("hwnd=%p\n", hwnd);

    if ((data = wayland_win_data_get(hwnd)))
    {
        if (data->window_surface)
            wayland_window_surface_set_window_region(data->window_surface, hrgn);
        wayland_win_data_release(data);
    }
}

/***********************************************************************
 *           WAYLAND_SetWindowStyle
 */
void CDECL WAYLAND_SetWindowStyle(HWND hwnd, INT offset, STYLESTRUCT *style)
{
    struct wayland_win_data *data;
    DWORD changed = style->styleNew ^ style->styleOld;

    TRACE("hwnd=%p offset=%d changed=%#x\n", hwnd, offset, changed);

    if (hwnd == GetDesktopWindow()) return;
    if (!(data = wayland_win_data_get(hwnd))) return;

    if (offset == GWL_EXSTYLE && (changed & WS_EX_LAYERED))
    {
        TRACE("hwnd=%p changed layered\n", hwnd);
        if (data->window_surface)
            wayland_window_surface_update_layered(data->window_surface, CLR_INVALID, 255, FALSE);
    }

    wayland_win_data_release(data);
}

/***********************************************************************
 *	     WAYLAND_SetLayeredWindowAttributes
 */
void CDECL WAYLAND_SetLayeredWindowAttributes(HWND hwnd, COLORREF key, BYTE alpha, DWORD flags)
{
    struct wayland_win_data *data;

    TRACE("hwnd=%p\n", hwnd);

    if (!(flags & LWA_COLORKEY)) key = CLR_INVALID;
    if (!(flags & LWA_ALPHA)) alpha = 255;

    if ((data = wayland_win_data_get(hwnd)))
    {
        if (data->window_surface)
            wayland_window_surface_update_layered(data->window_surface, key, alpha, FALSE);
        wayland_win_data_release(data);
    }
}

/*****************************************************************************
 *           WAYLAND_UpdateLayeredWindow
 */
BOOL CDECL WAYLAND_UpdateLayeredWindow(HWND hwnd, const UPDATELAYEREDWINDOWINFO *info,
                                       const RECT *window_rect)
{
    struct window_surface *window_surface;
    struct wayland_win_data *data;
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, 0 };
    COLORREF color_key = (info->dwFlags & ULW_COLORKEY) ? info->crKey : CLR_INVALID;
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *bmi = (BITMAPINFO *)buffer;
    void *src_bits, *dst_bits;
    RECT rect, src_rect;
    HDC hdc = 0;
    HBITMAP dib;
    BOOL ret = FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    TRACE("hwnd %p colorkey %x dirty %s flags %x src_alpha %d alpha_format %d\n",
          hwnd, info->crKey, wine_dbgstr_rect(info->prcDirty), info->dwFlags,
          info->pblend->SourceConstantAlpha, info->pblend->AlphaFormat == AC_SRC_ALPHA);

    rect = *window_rect;
    OffsetRect(&rect, -window_rect->left, -window_rect->top);

    window_surface = data->window_surface;
    if (!window_surface || !EqualRect(&window_surface->rect, &rect))
    {
        data->window_surface =
            wayland_window_surface_create(data->hwnd, &rect, 255, color_key, TRUE);
        if (window_surface) window_surface_release(window_surface);
        window_surface = data->window_surface;
        wayland_window_surface_update_wayland_surface(data->window_surface,
                                                      data->wayland_surface);
    }
    else
    {
        wayland_window_surface_update_layered(window_surface, 255, color_key, TRUE);
    }

    if (window_surface) window_surface_add_ref(window_surface);
    wayland_win_data_release(data);

    if (!window_surface) return FALSE;
    if (!info->hdcSrc)
    {
        window_surface_release(window_surface);
        return TRUE;
    }

    dst_bits = window_surface->funcs->get_info(window_surface, bmi);

    if (!(dib = CreateDIBSection(info->hdcDst, bmi, DIB_RGB_COLORS, &src_bits, NULL, 0))) goto done;
    if (!(hdc = CreateCompatibleDC(0))) goto done;

    SelectObject(hdc, dib);

    window_surface->funcs->lock(window_surface);

    if (info->prcDirty)
    {
        IntersectRect(&rect, &rect, info->prcDirty);
        memcpy(src_bits, dst_bits, bmi->bmiHeader.biSizeImage);
        PatBlt(hdc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, BLACKNESS);
    }
    src_rect = rect;
    if (info->pptSrc) OffsetRect(&src_rect, info->pptSrc->x, info->pptSrc->y);
    DPtoLP(info->hdcSrc, (POINT *)&src_rect, 2);

    ret = GdiAlphaBlend(hdc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                        info->hdcSrc, src_rect.left, src_rect.top,
                        src_rect.right - src_rect.left, src_rect.bottom - src_rect.top,
                        (info->dwFlags & ULW_ALPHA) ? *info->pblend : blend);
    if (ret)
    {
        RECT *bounds = window_surface->funcs->get_bounds(window_surface);
        memcpy(dst_bits, src_bits, bmi->bmiHeader.biSizeImage);
        UnionRect(bounds, bounds, &rect);
    }

    window_surface->funcs->unlock(window_surface);
    window_surface->funcs->flush(window_surface);

done:
    window_surface_release(window_surface);
    if (hdc) DeleteDC(hdc);
    if (dib) DeleteObject(dib);
    return ret;
}

static enum xdg_toplevel_resize_edge hittest_to_resize_edge(WPARAM hittest)
{
    switch (hittest) {
    case WMSZ_LEFT:        return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    case WMSZ_RIGHT:       return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    case WMSZ_TOP:         return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    case WMSZ_TOPLEFT:     return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    case WMSZ_TOPRIGHT:    return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    case WMSZ_BOTTOM:      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    case WMSZ_BOTTOMLEFT:  return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    case WMSZ_BOTTOMRIGHT: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    default:               return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    }
}

/***********************************************************************
 *          WAYLAND_SysCommand
 */
LRESULT CDECL WAYLAND_SysCommand(HWND hwnd, WPARAM wparam, LPARAM lparam)
{
    LRESULT ret = -1;
    WPARAM command = wparam & 0xfff0;
    WPARAM hittest = wparam & 0x0f;
    struct wayland_surface *wsurface;

    TRACE("cmd=%lx hwnd=%p, %x, %lx,\n", command, hwnd, (unsigned)wparam, lparam);

    if (!(wsurface = wayland_surface_for_hwnd_lock(hwnd)) || !wsurface->xdg_toplevel)
        goto done;

    if (command == SC_SIZE)
    {
        if (wsurface->wayland->last_button_serial)
        {
            xdg_toplevel_resize(wsurface->xdg_toplevel, wsurface->wayland->wl_seat,
                                wsurface->wayland->last_button_serial,
                                hittest_to_resize_edge(hittest));
        }
        ret = 0;
    }
    else if (command == SC_MOVE)
    {
        if (wsurface->wayland->last_button_serial)
        {
            xdg_toplevel_move(wsurface->xdg_toplevel, wsurface->wayland->wl_seat,
                              wsurface->wayland->last_button_serial);
        }
        ret = 0;
    }

done:
    wayland_surface_for_hwnd_unlock(wsurface);
    return ret;
}

static LRESULT handle_wm_wayland_configure(HWND hwnd)
{
    struct wayland_win_data *data;
    struct wayland_surface *wsurface;
    DWORD flags;
    int width, height, wine_width, wine_height;
    BOOL needs_move_to_origin;
    int origin_x, origin_y;
    UINT swp_flags;
    BOOL needs_enter_size_move = FALSE;
    BOOL needs_exit_size_move = FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return 0;
    if (!data->wayland_surface || !data->wayland_surface->xdg_toplevel)
    {
        TRACE("no suitable wayland surface, returning\n");
        wayland_win_data_release(data);
        return 0;
    }

    wsurface = data->wayland_surface;

    TRACE("serial=%d size=%dx%d flags=%#x restore_rect=%s\n",
          wsurface->pending.serial, wsurface->pending.width,
          wsurface->pending.height, wsurface->pending.configure_flags,
          wine_dbgstr_rect(&data->restore_rect));

    if (wsurface->pending.serial == 0)
    {
        TRACE("pending configure event already handled, returning\n");
        wayland_win_data_release(data);
        return 0;
    }

    wsurface->pending.processed = TRUE;

    data->wayland_configure_event_flags = wsurface->pending.configure_flags;

    width = wsurface->pending.width;
    height = wsurface->pending.height;
    flags = wsurface->pending.configure_flags;

    /* If we are free to set our size, first try the restore size, then
     * the current size. */
    if (width == 0)
    {
        int ignore;
        if (!IsIconic(hwnd))
            width = data->restore_rect.right - data->restore_rect.left;
        if (width == 0)
            width = data->window_rect.right - data->window_rect.left;
        wayland_surface_coords_rounded_from_wine(wsurface, width, 0,
                                                 &width, &ignore);
        wsurface->pending.width = width;
    }
    if (height == 0)
    {
        int ignore;
        if (!IsIconic(hwnd))
            height = data->restore_rect.bottom - data->restore_rect.top;
        if (height == 0)
            height = data->window_rect.bottom - data->window_rect.top;
        wayland_surface_coords_rounded_from_wine(wsurface, 0, height,
                                                 &ignore, &height);
        wsurface->pending.height = height;
    }

    if ((flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN) &&
        !(flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED))
    {
        wayland_surface_find_wine_fullscreen_fit(wsurface, width, height,
                                                 &wine_width, &wine_height);
    }
    else
    {
        wayland_surface_coords_to_wine(wsurface, width, height,
                                       &wine_width, &wine_height);
    }

    TRACE("hwnd=%p effective_size=%dx%d wine_size=%dx%d\n",
          data->hwnd, width, height, wine_width, wine_height);

    if ((flags & WAYLAND_CONFIGURE_FLAG_RESIZING) && !data->resizing)
    {
        data->resizing = TRUE;
        needs_enter_size_move = TRUE;
    }

    if (!(flags & WAYLAND_CONFIGURE_FLAG_RESIZING) && data->resizing)
    {
        data->resizing = FALSE;
        needs_exit_size_move = TRUE;
    }

    /* Parts of the window that are outside the win32 display are not
     * accessible to mouse events, although they may be visible and accessible
     * to the user from a wayland compositor pespective. To mitigate this, we
     * place all top-level windows at 0,0, to maximize the area that can reside
     * within the win32 display. */
    if (data->wayland_surface->main_output)
    {
        origin_x = data->wayland_surface->main_output->x;
        origin_y = data->wayland_surface->main_output->y;
        needs_move_to_origin = data->window_rect.top != origin_x ||
                               data->window_rect.left != origin_y;
        TRACE("current=%d,%d origin=%d,%d\n",
              data->window_rect.left, data->window_rect.top,
              origin_x, origin_y);
    }
    else
    {
        origin_x = 0;
        origin_y = 0;
        needs_move_to_origin = FALSE;
    }

    wayland_win_data_release(data);

    if (needs_enter_size_move)
        SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);

    if (needs_exit_size_move)
        SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);

    if ((data = wayland_win_data_get(hwnd)))
    {
        data->handling_wayland_configure_event = TRUE;
        wayland_win_data_release(data);
    }

    if (flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED)
        SetWindowLongW(hwnd, GWL_STYLE, GetWindowLongW(hwnd, GWL_STYLE) | WS_MAXIMIZE);
    else
        SetWindowLongW(hwnd, GWL_STYLE, GetWindowLongW(hwnd, GWL_STYLE) & ~WS_MAXIMIZE);

    swp_flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER;

    if (!needs_move_to_origin) swp_flags |= SWP_NOMOVE;
    if (wine_width > 0 && wine_height > 0)
        swp_flags |= SWP_FRAMECHANGED;
    else
        swp_flags |= SWP_NOSIZE | SWP_NOREDRAW;
    /* When we are maximized or fullscreen, wayland is particular about the
     * surface size it accepts, so don't allow the app to change it. */
    if (flags & (WAYLAND_CONFIGURE_FLAG_MAXIMIZED|WAYLAND_CONFIGURE_FLAG_FULLSCREEN))
        swp_flags |= SWP_NOSENDCHANGING;

    SetWindowPos(hwnd, 0, origin_x, origin_y, wine_width, wine_height, swp_flags);

    if ((data = wayland_win_data_get(hwnd)))
    {
        data->handling_wayland_configure_event = FALSE;
        wayland_win_data_release(data);
    }

    return 0;
}

static void CALLBACK post_configure(HWND hwnd, UINT msg, UINT_PTR timer_id, DWORD elapsed)
{
    TRACE("hwnd=%p\n", hwnd);
    KillTimer(hwnd, timer_id);
    handle_wm_wayland_configure(hwnd);
}

static void handle_wm_wayland_surface_output_change(HWND hwnd)
{
    struct wayland_surface *wsurface;
    int x, y, w, h;
    UINT swp_flags;

    TRACE("hwnd=%p\n", hwnd);

    wsurface = wayland_surface_for_hwnd_lock(hwnd);
    if (!wsurface || !wsurface->xdg_toplevel)
    {
        TRACE("no suitable wayland surface, returning\n");
        goto out;
    }

    swp_flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                SWP_FRAMECHANGED | SWP_NOSENDCHANGING;

    if (wsurface->main_output)
    {
        x = wsurface->main_output->x;
        y = wsurface->main_output->y;
        TRACE("moving window to %d,%d\n", x, y);
    }
    else
    {
        x = y = 0;
        swp_flags |= SWP_NOMOVE;
    }

    /* If we are fullscreen or maximized we need to provide a particular buffer
     * size to the wayland compositor on the new output (hence swp_flags
     * includes SWP_NOSENDCHANGING). */
    if (wsurface->current.serial &&
        (wsurface->current.configure_flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED))
    {
        wayland_surface_coords_to_wine(wsurface, wsurface->current.width,
                                       wsurface->current.height,
                                       &w, &h);
        TRACE("resizing window to maximized %dx%d\n", w, h);
    }
    else if (wsurface->current.serial &&
             (wsurface->current.configure_flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN))
    {
        wayland_surface_find_wine_fullscreen_fit(wsurface, wsurface->current.width,
                                                 wsurface->current.height,
                                                 &w, &h);
        TRACE("resizing window to fullscreen %dx%d\n", w, h);
    }
    else
    {
        w = h = 0;
        swp_flags |= SWP_NOSIZE;
    }

    SetWindowPos(hwnd, 0, x, y, w, h, swp_flags);

out:
    wayland_surface_for_hwnd_unlock(wsurface);
}

static void handle_wm_wayland_monitor_change(struct wayland *wayland)
{
    struct wayland_surface *surface, *tmp;

    wayland_update_outputs_from_process(wayland);

    /* Update the state of all surfaces tracked by the wayland thread instance,
     * in case any surface was affected by the monitor changes (e.g., gained or
     * lost the fullscreen state). We use the safe iteration variant since a
     * state update may cause the surface to be recreated. */
    wl_list_for_each_safe(surface, tmp, &wayland->toplevel_list, link)
    {
        struct wayland_win_data *data = wayland_win_data_get(surface->hwnd);
        if (data)
        {
            update_wayland_state(data);
            wayland_win_data_release(data);
        }
    }
}

/**********************************************************************
 *           WAYLAND_WindowMessage
 */
LRESULT CDECL WAYLAND_WindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TRACE("msg %x hwnd %p wp %lx lp %lx\n", msg, hwnd, wp, lp);

    switch (msg)
    {
    case WM_WAYLAND_BROADCAST_DISPLAY_CHANGE:
        SendMessageTimeoutW(HWND_BROADCAST, WM_DISPLAYCHANGE, wp, lp,
                            SMTO_ABORTIFHUNG, 2000, NULL);
        break;
    case WM_WAYLAND_SET_CURSOR:
        wayland_pointer_update_cursor_from_win32(&thread_wayland()->pointer,
                                                 (HCURSOR)lp);
        break;
    case WM_WAYLAND_QUERY_SURFACE_MAPPED:
        {
            LRESULT res;
            struct wayland_surface *wayland_surface = wayland_surface_for_hwnd_lock(hwnd);
            res = wayland_surface ? wayland_surface->mapped : 0;
            wayland_surface_for_hwnd_unlock(wayland_surface);
            return res;
        }
        break;
    case WM_WAYLAND_CONFIGURE:
        /* While resizing, configure events can come continuously and due to the
         * amount of other message their handling produces (e.g., paints), have
         * the potential to keep the message loop busy for some time. This may
         * lead Wine core to think that the app never goes idle (see
         * win.c:flush_window_surfaces), and thus start flushing at unfortunate
         * times (e.g., in between partial window paints), causing visual
         * artifacts.
         *
         * To mitigate this we handle the configure message only if the message
         * queue is empty, ensuring that the loop has had time to become idle.
         * If the queue is not currently empty, we schedule a timer message,
         * which due to having the lowest priority is guaranteed to be triggered
         * only on otherwise empty queues.
         */
        if (!GetQueueStatus(QS_ALLINPUT))
        {
            return handle_wm_wayland_configure(hwnd);
        }
        else
        {
            struct wayland_surface *wayland_surface = wayland_surface_for_hwnd_lock(hwnd);
            if (wayland_surface && wayland_surface->xdg_toplevel)
            {
                SetTimer(hwnd, (UINT_PTR)wayland_surface->wl_surface, 10,
                         post_configure);
            }
            wayland_surface_for_hwnd_unlock(wayland_surface);
        }
        break;
    case WM_WAYLAND_STATE_UPDATE:
        {
            struct wayland_win_data *data = wayland_win_data_get(hwnd);
            if (data)
            {
                data->pending_state_update_message = FALSE;
                update_wayland_state(data);
                wayland_win_data_release(data);
            }
        }
        break;
    case WM_WAYLAND_SURFACE_OUTPUT_CHANGE:
        handle_wm_wayland_surface_output_change(hwnd);
        break;
    case WM_WAYLAND_MONITOR_CHANGE:
        handle_wm_wayland_monitor_change(thread_wayland());
        break;
    default:
        FIXME("got window msg %x hwnd %p wp %lx lp %lx\n", msg, hwnd, wp, lp);
    }

    return 0;
}
