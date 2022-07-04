/*
 * Wayland input handling
 *
 * Copyright (c) 2020 Alexandros Frantzis for Collabora Ltd
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "waylanddrv.h"

#include "wine/debug.h"

#include "winuser.h"

#include <linux/input.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

/**********************************************************************
 *          Pointer handling
 */

static void pointer_handle_motion_internal(void *data, struct wl_pointer *pointer,
                                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland *wayland = data;
    HWND focused_hwnd = wayland->pointer.focused_surface ?
                        wayland->pointer.focused_surface->hwnd : 0;
    INPUT input = {0};
    int screen_x, screen_y;

    if (!focused_hwnd)
        return;

    wayland_surface_coords_to_screen(wayland->pointer.focused_surface,
                                     wl_fixed_to_double(sx),
                                     wl_fixed_to_double(sy),
                                     &screen_x, &screen_y);

    TRACE("surface=%p hwnd=%p wayland_xy=%.2f,%.2f screen_xy=%d,%d\n",
          wayland->pointer.focused_surface, focused_hwnd,
          wl_fixed_to_double(sx), wl_fixed_to_double(sy),
          screen_x, screen_y);

    input.type           = INPUT_MOUSE;
    input.mi.dx          = screen_x;
    input.mi.dy          = screen_y;
    input.mi.dwFlags     = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;

    wayland->last_dispatch_mask |= QS_MOUSEMOVE;
    wayland->last_event_type = INPUT_MOUSE;

    __wine_send_input(focused_hwnd, &input, NULL);
}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer,
                                  uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland *wayland = data;

    /* Don't handle absolute motion events if we are in relative mode. */
    if (wayland->pointer.zwp_relative_pointer_v1)
        return;

    pointer_handle_motion_internal(data, pointer, time, sx, sy);
}

static void CALLBACK set_cursor_if_current_invalid(HWND hwnd, UINT msg,
                                                   UINT_PTR timer_id,
                                                   DWORD elapsed)
{
    TRACE("hwnd=%p\n", hwnd);
    NtUserKillTimer(hwnd, timer_id);
    wayland_set_cursor_if_current_invalid((HCURSOR)timer_id);
}

static void pointer_handle_enter(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface,
                                 wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland *wayland = data;
    struct wayland_surface *wayland_surface =
        surface ? wl_surface_get_user_data(surface) : NULL;

    /* Since pointer events can arrive in multiple threads, ensure we only
     * handle them in the thread that owns the surface, to avoid passing
     * duplicate events to Wine. */
    if (wayland_surface && wayland_surface->hwnd &&
        wayland_surface->wayland == wayland)
    {
        HCURSOR hcursor;
        TRACE("surface=%p hwnd=%p\n", wayland_surface, wayland_surface->hwnd);
        wayland->pointer.focused_surface = wayland_surface;
        wayland->pointer.enter_serial = serial;
        /* Invalidate the set cursor cache, so that next update is
         * unconditionally applied. */
        hcursor = wayland_invalidate_set_cursor();
        /* Schedule a cursor update, to ensure the current cursor is applied on
         * this surface, but only if the application hasn't updated the cursor
         * in the meantime. */
        NtUserSetTimer(wayland_surface->hwnd, (UINT_PTR)hcursor, USER_TIMER_MINIMUM,
                       set_cursor_if_current_invalid, TIMERV_DEFAULT_COALESCING);
        /* Handle the enter as a motion, to account for cases where the
         * window first appears beneath the pointer and won't get a separate
         * motion event. */
        pointer_handle_motion_internal(data, pointer, 0, sx, sy);
    }
}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface)
{
    struct wayland *wayland = data;

    if (wayland->pointer.focused_surface &&
        wayland->pointer.focused_surface->wl_surface == surface)
    {
        TRACE("surface=%p hwnd=%p\n",
              wayland->pointer.focused_surface,
              wayland->pointer.focused_surface->hwnd);
        wayland->pointer.focused_surface = NULL;
        wayland->pointer.enter_serial = 0;
    }
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t serial, uint32_t time, uint32_t button,
                                  uint32_t state)
{
    struct wayland *wayland = data;
    HWND focused_hwnd = wayland->pointer.focused_surface ?
                        wayland->pointer.focused_surface->hwnd : 0;
    INPUT input = {0};

    if (!focused_hwnd)
        return;

    TRACE("button=%#x state=%#x hwnd=%p\n", button, state, focused_hwnd);

    input.type = INPUT_MOUSE;

    switch (button)
    {
    case BTN_LEFT: input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
    case BTN_RIGHT: input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; break;
    case BTN_MIDDLE: input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN; break;
    default: break;
    }

    if (state == WL_POINTER_BUTTON_STATE_RELEASED)
        input.mi.dwFlags <<= 1;

    wayland->last_dispatch_mask |= QS_MOUSEBUTTON;
    wayland->last_event_type = INPUT_MOUSE;

    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
        wayland->last_button_serial = serial;
    else
        wayland->last_button_serial = 0;

    __wine_send_input(focused_hwnd, &input, NULL);
}

static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
                                uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static void pointer_handle_frame(void *data, struct wl_pointer *wl_pointer)
{
}

static void pointer_handle_axis_source(void *data, struct wl_pointer *wl_pointer,
                                       uint32_t axis_source)
{
}

static void pointer_handle_axis_stop(void *data, struct wl_pointer *wl_pointer,
                                     uint32_t time, uint32_t axis)
{
}

static void pointer_handle_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                                         uint32_t axis, int32_t discrete)
{
    struct wayland *wayland = data;
    HWND focused_hwnd = wayland->pointer.focused_surface ?
                        wayland->pointer.focused_surface->hwnd : 0;
    INPUT input = {0};

    if (!focused_hwnd)
        return;

    TRACE("axis=%#x discrete=%d hwnd=%p\n", axis, discrete, focused_hwnd);

    input.type = INPUT_MOUSE;

    switch (axis)
    {
    case WL_POINTER_AXIS_VERTICAL_SCROLL:
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = -WHEEL_DELTA * discrete;
        break;
    case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
        input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = WHEEL_DELTA * discrete;
        break;
    default: break;
    }

    wayland->last_dispatch_mask |= QS_MOUSEBUTTON;
    wayland->last_event_type = INPUT_MOUSE;

    __wine_send_input(focused_hwnd, &input, NULL);
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
    pointer_handle_frame,
    pointer_handle_axis_source,
    pointer_handle_axis_stop,
    pointer_handle_axis_discrete,
};

static void relative_pointer_handle_motion(void *data,
                                           struct zwp_relative_pointer_v1 *rpointer,
                                           uint32_t utime_hi,
                                           uint32_t utime_lo,
                                           wl_fixed_t dx,
                                           wl_fixed_t dy,
                                           wl_fixed_t dx_unaccel,
                                           wl_fixed_t dy_unaccel)
{
    struct wayland *wayland = data;
    HWND focused_hwnd = wayland->pointer.focused_surface ?
                        wayland->pointer.focused_surface->hwnd : 0;
    int wine_dx, wine_dy;
    INPUT input = {0};

    if (!focused_hwnd)
        return;

    wayland_surface_coords_to_wine(wayland->pointer.focused_surface,
                                   wl_fixed_to_int(dx), wl_fixed_to_int(dy),
                                   &wine_dx, &wine_dy);

    TRACE("surface=%p hwnd=%p wayland_dxdy=%d,%d wine_dxdy=%d,%d\n",
          wayland->pointer.focused_surface, focused_hwnd,
          wl_fixed_to_int(dx), wl_fixed_to_int(dy), wine_dx, wine_dy);

    input.type           = INPUT_MOUSE;
    input.mi.dx          = wine_dx;
    input.mi.dy          = wine_dy;
    input.mi.dwFlags     = MOUSEEVENTF_MOVE;

    wayland->last_dispatch_mask |= QS_MOUSEMOVE;
    wayland->last_event_type = INPUT_MOUSE;

    __wine_send_input(focused_hwnd, &input, NULL);
}

static const struct zwp_relative_pointer_v1_listener zwp_relative_pointer_v1_listener = {
    relative_pointer_handle_motion,
};

void wayland_pointer_init(struct wayland_pointer *pointer, struct wayland *wayland,
                          struct wl_pointer *wl_pointer)
{
    wayland->pointer.wayland = wayland;
    wayland->pointer.wl_pointer = wl_pointer;
    wl_pointer_add_listener(wayland->pointer.wl_pointer, &pointer_listener, wayland);
    wayland->pointer.cursor_wl_surface =
        wl_compositor_create_surface(wayland->wl_compositor);
    pointer->zwp_relative_pointer_v1 = NULL;
}

void wayland_pointer_deinit(struct wayland_pointer *pointer)
{
    if (pointer->zwp_relative_pointer_v1)
        zwp_relative_pointer_v1_destroy(pointer->zwp_relative_pointer_v1);

    if (pointer->wl_pointer)
        wl_pointer_destroy(pointer->wl_pointer);

    if (pointer->cursor_wl_surface)
        wl_surface_destroy(pointer->cursor_wl_surface);

    if (pointer->cursor)
        wayland_cursor_destroy(pointer->cursor);

    memset(pointer, 0, sizeof(*pointer));
}

/**********************************************************************
 *          wayland_pointer_set_relative
 *
 * Set whether the pointer emits relative (if able) or absolute motion events.
 * The default is to emit absolute motion events.
 */
void wayland_pointer_set_relative(struct wayland_pointer *pointer, BOOL relative)
{
    if (!pointer->wayland->zwp_relative_pointer_manager_v1)
        return;

    if (!pointer->zwp_relative_pointer_v1 && relative)
    {
        pointer->zwp_relative_pointer_v1 =
            zwp_relative_pointer_manager_v1_get_relative_pointer(
                pointer->wayland->zwp_relative_pointer_manager_v1,
                pointer->wl_pointer);

        zwp_relative_pointer_v1_add_listener(pointer->zwp_relative_pointer_v1,
                                             &zwp_relative_pointer_v1_listener,
                                             pointer->wayland);
    }
    else if (pointer->zwp_relative_pointer_v1 && !relative)
    {
        zwp_relative_pointer_v1_destroy(pointer->zwp_relative_pointer_v1);
        pointer->zwp_relative_pointer_v1 = NULL;
    }
}