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
#include "wine/gdi_driver.h"
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
    /* whether this window is visible */
    BOOL visible;
    /* Save previous state to be able to decide when to recreate wayland surface */
    HWND           old_parent;
    RECT           old_window_rect;
    /* Whether a first wayland surface update is needed */
    BOOL           wayland_surface_needs_first_update;
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
    data->wayland_surface_needs_first_update = TRUE;

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

static HWND wayland_win_data_get_effective_parent(struct wayland_win_data *data)
{
    struct wayland *wayland = thread_init_wayland();
    DWORD style = GetWindowLongW(data->hwnd, GWL_STYLE);
    /* GWLP_HWNDPARENT gets the owner for any kind of toplevel windows,
     * and the parent for child windows. */
    HWND parent_hwnd = (HWND)GetWindowLongPtrW(data->hwnd, GWLP_HWNDPARENT);
    HWND effective_parent_hwnd;
    HMONITOR hmonitor;
    MONITORINFO mi;
    double monitor_width;
    double monitor_height;
    double window_width;
    double window_height;

    if (parent_hwnd == GetDesktopWindow()) parent_hwnd = 0;

    /* Child windows don't have an effective parent, unless they are children
     * of the desktop (thus effectively top-level). */
    if ((style & WS_CHILD) && parent_hwnd) return 0;

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

    /* Many applications use top level, unowned (or owned by the desktop)
     * popup windows for menus and tooltips and depend on screen
     * coordinates for correct positioning. Since wayland can't deal with
     * screen coordinates, try to guess the effective parent window of such
     * popups and manage them as wayland subsurfaces. Our heuristic is to
     * treat all unowned, undecorated top-level windows with reasonable size
     * as transient popups.  */
    if (!parent_hwnd && (style & WS_CAPTION) != WS_CAPTION &&
        window_width > 1.0 && window_height > 1.0 &&
        window_width * window_height < 0.5 * monitor_width * monitor_height)
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

    TRACE("hwnd=%p parent=%p effective_parent=%p style=0x%08x rect=%s monitor=%s\n",
          data->hwnd, parent_hwnd, effective_parent_hwnd, style,
          wine_dbgstr_rect(&data->window_rect),
          wine_dbgstr_rect(&mi.rcMonitor));

    return effective_parent_hwnd;
}

static BOOL wayland_win_data_wayland_surface_needs_update(struct wayland_win_data *data)
{
    if (data->wayland_surface_needs_first_update)
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
    DWORD style;

    TRACE("hwnd=%p\n", data->hwnd);

    data->wayland_surface_needs_first_update = FALSE;

    wayland = thread_init_wayland();

    if (data->wayland_surface)
    {
        wayland_surface_unref(data->wayland_surface);
        data->wayland_surface = NULL;
    }

    /* Only create wayland surfaces for toplevel windows. Let Wine core handle
     * the drawing of other windows in their corresponding top level window
     * surface. */
    if (data->parent) return;

    effective_parent_hwnd = wayland_win_data_get_effective_parent(data);
    parent_surface = NULL;

    if (effective_parent_hwnd)
    {
        parent_surface = wayland_surface_for_hwnd_lock(effective_parent_hwnd);
        wayland_surface_for_hwnd_unlock(parent_surface);
    }

    data->effective_parent = effective_parent_hwnd;
    style = GetWindowLongW(data->hwnd, GWL_STYLE);

    /* Use wayland subsurfaces for owned win32 windows that are transient (i.e., don't have
     * a titlebar). Otherwise, if the window is visible make it wayland toplevel. Finally,
     * if the window is not visible create a plain (without a role) surface to avoid
     * polluting the compositor with empty xdg_toplevels. */
    if ((style & WS_CAPTION) != WS_CAPTION && parent_surface)
        data->wayland_surface = wayland_surface_create_subsurface(wayland, parent_surface);
    else if (data->visible)
        data->wayland_surface = wayland_surface_create_toplevel(wayland, parent_surface);
    else
        data->wayland_surface = wayland_surface_create_plain(wayland);

    if (data->wayland_surface)
        data->wayland_surface->hwnd = data->hwnd;
}

static void update_wayland_state(struct wayland_win_data *data)
{
    if (wayland_win_data_wayland_surface_needs_update(data))
        wayland_win_data_update_wayland_surface(data);

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
    if (data->window_surface) window_surface_release(data->window_surface);
    data->window_surface = surface;

    update_wayland_state(data);

    wayland_win_data_release(data);
}
