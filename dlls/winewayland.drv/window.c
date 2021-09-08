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

static BOOL can_be_effective_parent(HWND hwnd, HWND parent_hwnd)
{
    struct wayland_surface *parent_surface;

    if (parent_hwnd == 0)
        return FALSE;

    if (parent_hwnd == hwnd)
    {
        TRACE("hwnd=%p can't use parent=%p since it's itself\n",
              hwnd, parent_hwnd);
        return FALSE;
    }

    if (!(parent_surface = wayland_surface_for_hwnd_lock(parent_hwnd)))
    {
        TRACE("hwnd=%p can't use parent=%p since we are not tracking it\n",
              hwnd, parent_hwnd);
        return FALSE;
    }
    wayland_surface_for_hwnd_unlock(parent_surface);

    if (!(GetWindowLongW(parent_hwnd, GWL_STYLE) & WS_VISIBLE))
    {
        TRACE("hwnd=%p can't use parent=%p since it's not visible\n",
              hwnd, parent_hwnd);
        return FALSE;
    }

    return TRUE;
}

static HWND guess_popup_parent(struct wayland *wayland, HWND hwnd)
{
    HWND pointer_hwnd;
    HWND cursor_hwnd;
    HWND keyboard_hwnd;
    HWND focus_hwnd;
    HWND popup_hwnd;
    POINT cursor;

    pointer_hwnd = wayland->pointer.focused_surface ?
                   wayland->pointer.focused_surface->hwnd : NULL;
    if (pointer_hwnd)
        pointer_hwnd = GetAncestor(pointer_hwnd, GA_ROOT);

    GetCursorPos(&cursor);
    cursor_hwnd = WindowFromPoint(cursor);
    if (cursor_hwnd)
        cursor_hwnd = GetAncestor(cursor_hwnd, GA_ROOT);

    keyboard_hwnd = wayland->keyboard.focused_surface ?
                    wayland->keyboard.focused_surface->hwnd : NULL;
    if (keyboard_hwnd)
        keyboard_hwnd = GetAncestor(keyboard_hwnd, GA_ROOT);

    focus_hwnd = GetFocus();
    if (focus_hwnd)
        focus_hwnd = GetAncestor(focus_hwnd, GA_ROOT);

    TRACE("pointer_hwnd=%p cursor_hwnd=%p keyboard_hwnd=%p focus_hwnd=%p "
          "last_event_type=%d\n",
          pointer_hwnd, cursor_hwnd, keyboard_hwnd, focus_hwnd,
          wayland->last_event_type);

    /* If we have a recent mouse event, the popup parent is likely the window
     * under the cursor, so prefer it. Otherwise prefer the window with
     * the keyboard focus. */
    if (wayland->last_event_type == INPUT_MOUSE)
    {
        if (can_be_effective_parent(hwnd, pointer_hwnd))
            popup_hwnd = pointer_hwnd;
        else if (can_be_effective_parent(hwnd, cursor_hwnd))
            popup_hwnd = cursor_hwnd;
        else if (can_be_effective_parent(hwnd, keyboard_hwnd))
            popup_hwnd = keyboard_hwnd;
        else if (can_be_effective_parent(hwnd, focus_hwnd))
            popup_hwnd = focus_hwnd;
        else
            popup_hwnd = 0;
    }
    else
    {
        if (can_be_effective_parent(hwnd, keyboard_hwnd))
            popup_hwnd = keyboard_hwnd;
        else if (can_be_effective_parent(hwnd, focus_hwnd))
            popup_hwnd = focus_hwnd;
        else if (can_be_effective_parent(hwnd, pointer_hwnd))
            popup_hwnd = pointer_hwnd;
        else if (can_be_effective_parent(hwnd, cursor_hwnd))
            popup_hwnd = cursor_hwnd;
        else
            popup_hwnd = 0;
    }

    TRACE("=> popup_hwnd=%p\n", popup_hwnd);

    return popup_hwnd;
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

    /* Minimized windows can't be popups. */
    if (style & WS_MINIMIZE)
    {
        TRACE("hwnd=%p is minimized => FALSE\n", data->hwnd);
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
    HWND effective_parent_hwnd;

    if (!can_be_effective_parent(data->hwnd, parent_hwnd))
        parent_hwnd = 0;

    /* Many applications use top level, unowned (or owned by the desktop)
     * popup windows for menus and tooltips and depend on screen
     * coordinates for correct positioning. Since wayland can't deal with
     * screen coordinates, try to guess the effective parent window of such
     * popups and manage them as wayland subsurfaces. */
    if (!parent_hwnd && wayland_win_data_can_be_popup(data))
        effective_parent_hwnd = guess_popup_parent(wayland, data->hwnd);
    else
        effective_parent_hwnd = parent_hwnd;

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

static struct wayland_surface *update_surface_for_role(struct wayland_win_data *data,
                                                       enum wayland_surface_role role,
                                                       struct wayland *wayland,
                                                       struct wayland_surface *parent_surface)
{
    struct wayland_surface *surface = data->wayland_surface;

    if (!surface ||
        (role != WAYLAND_SURFACE_ROLE_NONE &&
         surface->role != WAYLAND_SURFACE_ROLE_NONE &&
         surface->role != role))
    {
        surface = wayland_surface_create_plain(wayland);
        if (surface) EnterCriticalSection(&surface->crit);
    }
    else
    {
        /* Lock the wayland surface to avoid other threads interacting with it
         * while we are updating. */
        EnterCriticalSection(&surface->crit);
        wayland_surface_clear_role(surface);
        /* Clear the associated HWND, to allow a potential invocation of
         * wayland_surface_make_toplevel below, to properly handle the
         * initial configure event. */
        surface->hwnd = 0;
    }

    if (role == WAYLAND_SURFACE_ROLE_TOPLEVEL)
        wayland_surface_make_toplevel(surface, parent_surface);
    else if (role == WAYLAND_SURFACE_ROLE_SUBSURFACE)
        wayland_surface_make_subsurface(surface, parent_surface);

    surface->hwnd = data->hwnd;

    LeaveCriticalSection(&surface->crit);

    return surface;
}

static void wayland_win_data_update_wayland_surface(struct wayland_win_data *data)
{
    struct wayland *wayland;
    HWND effective_parent_hwnd;
    struct wayland_surface *surface;
    struct wayland_surface *parent_surface;

    TRACE("hwnd=%p\n", data->hwnd);

    data->wayland_surface_needs_update = FALSE;

    wayland = thread_init_wayland();

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
    {
        surface = update_surface_for_role(data, WAYLAND_SURFACE_ROLE_SUBSURFACE,
                                          wayland, parent_surface);
    }
    else if (data->visible)
    {
        surface = update_surface_for_role(data, WAYLAND_SURFACE_ROLE_TOPLEVEL,
                                          wayland, parent_surface);
    }
    else
    {
        surface = update_surface_for_role(data, WAYLAND_SURFACE_ROLE_NONE,
                                          wayland, parent_surface);
    }

    if (data->wayland_surface != surface)
    {
        if (data->wayland_surface)
            wayland_surface_unref(data->wayland_surface);
        data->wayland_surface = surface;
    }
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
    *surface = wayland_window_surface_create(data->hwnd, &surface_rect);

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

    update_wayland_state(data);

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

    wayland_surface_coords_to_wine(wsurface, width, height,
                                   &wine_width, &wine_height);

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
    origin_x = 0;
    origin_y = 0;
    needs_move_to_origin = data->window_rect.top != origin_x ||
                           data->window_rect.left != origin_y;
    TRACE("current=%d,%d origin=%d,%d\n",
          data->window_rect.left, data->window_rect.top,
          origin_x, origin_y);

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
    default:
        FIXME("got window msg %x hwnd %p wp %lx lp %lx\n", msg, hwnd, wp, lp);
    }

    return 0;
}
