/*
 * Wayland core handling
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

#include "config.h"
#include "wine/port.h"

#include "waylanddrv.h"
#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/unicode.h"
#include "winuser.h"
#include <linux/input.h>

#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include <poll.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static CRITICAL_SECTION thread_wayland_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &thread_wayland_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": thread_wayland_section") }
};
static CRITICAL_SECTION thread_wayland_section = { &critsect_debug, -1, 0, 0, 0, 0 };

static struct wl_list thread_wayland_list = {&thread_wayland_list, &thread_wayland_list};

/**********************************************************************
 *          Output handling
 */
static void output_handle_geometry(void *data, struct wl_output *wl_output,
                                   int32_t x, int32_t y,
                                   int32_t physical_width, int32_t physical_height,
                                   int32_t subpixel,
                                   const char *make, const char *model,
                                   int32_t output_transform)
{
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
                               uint32_t flags, int32_t width, int32_t height,
                               int32_t refresh)
{
    struct wayland_output *output = data;
    struct wayland_output_mode *mode = heap_alloc_zero(sizeof(*mode));

    mode->width = width;
    mode->height = height;
    mode->refresh = refresh;
    mode->current = (flags & WL_OUTPUT_MODE_CURRENT);

    wl_list_insert(&output->mode_list, &mode->link);
}

static void output_handle_done(void *data, struct wl_output *wl_output)
{
}

static void output_handle_scale(void *data, struct wl_output *wl_output,
                                int32_t scale)
{
}

static const struct wl_output_listener output_listener = {
    output_handle_geometry,
    output_handle_mode,
    output_handle_done,
    output_handle_scale
};

static void wayland_add_output(struct wayland *wayland, uint32_t id, uint32_t version)
{
    struct wayland_output *output = heap_alloc_zero (sizeof(*output));
    static const WCHAR name_fmtW[] = { '\\','\\','.','\\','D','I','S','P','L','A','Y','%','d',0 };
    static int output_id = 0;

    output->wl_output = wl_registry_bind(wayland->wl_registry, id,
                                         &wl_output_interface, 1);
    wl_output_add_listener(output->wl_output, &output_listener, output);
    wl_list_init(&output->mode_list);
    sprintfW(output->name, name_fmtW, ++output_id);

    wl_list_insert(&wayland->output_list, &output->link);
}

static void wayland_output_destroy(struct wayland_output *output)
{
    struct wayland_output_mode *mode,*tmp;

    wl_list_for_each_safe(mode, tmp, &output->mode_list, link)
    {
        wl_list_remove(&mode->link);
        heap_free(mode);
    }

    wl_output_destroy(output->wl_output);

    heap_free(output);
}

/**********************************************************************
 *          xdg_wm_base handling
 */

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    xdg_wm_base_ping,
};

static struct wayland_surface *get_wayland_surface(struct wayland *wayland,
                                                   struct wl_surface *wl_surface)
{
    struct wayland_surface *surface;

    wl_list_for_each(surface, &wayland->surface_list, link)
    {
        if (surface->wl_surface == wl_surface)
            return surface;
    }

    return NULL;
}

/**********************************************************************
 *          Keyboard handling
 */

static void keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
                                   uint32_t format, int fd, uint32_t size)
{
    /* TODO: Handle keymaps */
    close(fd);
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
                                  uint32_t serial, struct wl_surface *surface,
                                  struct wl_array *keys)
{
    struct wayland *wayland = data;
    struct wayland_surface *wayland_surface = get_wayland_surface(wayland, surface);

    if (wayland_surface && wayland_surface->hwnd)
    {
        TRACE("surface=%p hwnd=%p\n", wayland_surface, wayland_surface->hwnd);
        wayland->keyboard.focused_surface = wayland_surface;
        wayland->keyboard.enter_serial = serial;
        SetFocus(wayland_surface->hwnd);
    }
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, struct wl_surface *surface)
{
    struct wayland *wayland = data;

    if (wayland->keyboard.focused_surface &&
        wayland->keyboard.focused_surface->wl_surface == surface)
    {
        TRACE("surface=%p hwnd=%p\n",
              wayland->keyboard.focused_surface,
              wayland->keyboard.focused_surface->hwnd);
        KillTimer(wayland->keyboard.focused_surface->hwnd, (UINT_PTR)keyboard);
        wayland->keyboard.focused_surface = NULL;
        wayland->keyboard.enter_serial = 0;
    }
}

static void CALLBACK repeat_key(HWND hwnd, UINT msg, UINT_PTR timer_id, DWORD elapsed)
{
    struct wayland *wayland = thread_wayland();

    wayland_keyboard_emit(wayland->keyboard.pressed_key,
                          WL_KEYBOARD_KEY_STATE_PRESSED, hwnd);

    SetTimer(hwnd, timer_id, wayland->keyboard.repeat_interval_ms,
             repeat_key);
}

static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
                                uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t state)
{
    struct wayland *wayland = data;
    HWND focused_hwnd = wayland->keyboard.focused_surface ?
                        wayland->keyboard.focused_surface->hwnd : 0;
    UINT_PTR repeat_key_timer_id = (UINT_PTR)keyboard;

    if (!focused_hwnd)
        return;

    TRACE("key=%d state=%#x focused_hwnd=%p\n", key, state, focused_hwnd);

    wayland->last_dispatch_mask |= QS_KEY | QS_HOTKEY;
    wayland->last_event_type = INPUT_KEYBOARD;

    wayland_keyboard_emit(key, state, focused_hwnd);

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
    {
        wayland->keyboard.pressed_key = key;
        SetTimer(focused_hwnd, repeat_key_timer_id, wayland->keyboard.repeat_delay_ms,
                 repeat_key);
    }
    else
    {
        wayland->keyboard.pressed_key = 0;
        KillTimer(focused_hwnd, repeat_key_timer_id);
    }
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
                                      uint32_t serial, uint32_t mods_depressed,
                                      uint32_t mods_latched, uint32_t mods_locked,
                                      uint32_t group)
{
}

static void keyboard_handle_repeat_info(void *data, struct wl_keyboard *keyboard,
                                        int rate, int delay)
{
    struct wayland *wayland = data;

    TRACE("rate=%d delay=%d\n", rate, delay);

    wayland->keyboard.repeat_interval_ms = 1000 / rate;
    wayland->keyboard.repeat_delay_ms = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers,
    keyboard_handle_repeat_info,
};

/**********************************************************************
 *          Pointer handling
 */

static void pointer_handle_enter(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface,
                                 wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland *wayland = data;
    struct wayland_surface *wayland_surface = get_wayland_surface(wayland, surface);

    if (wayland_surface && wayland_surface->hwnd) {
        TRACE("surface=%p hwnd=%p\n", wayland_surface, wayland_surface->hwnd);
        wayland->pointer.focused_surface = wayland_surface;
        wayland->pointer.enter_serial = serial;
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

static void pointer_handle_motion(void *data, struct wl_pointer *pointer,
                                  uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland *wayland = data;
    HWND focused_hwnd = wayland->pointer.focused_surface ?
                        wayland->pointer.focused_surface->hwnd : 0;
    INPUT input = {0};
    POINT screen_pos;

    if (!focused_hwnd)
        return;

    screen_pos = wayland_surface_coords_to_screen(wayland->pointer.focused_surface,
                                                  wl_fixed_to_int(sx),
                                                  wl_fixed_to_int(sy));

    input.type           = INPUT_MOUSE;
    input.mi.dx          = screen_pos.x;
    input.mi.dy          = screen_pos.y;
    input.mi.dwFlags     = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;

    wayland->last_dispatch_mask |= QS_MOUSEMOVE;
    wayland->last_event_type = INPUT_MOUSE;

    __wine_send_input(focused_hwnd, &input);
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

    __wine_send_input(focused_hwnd, &input);
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

    __wine_send_input(focused_hwnd, &input);
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

static void wayland_pointer_deinit(struct wayland_pointer *pointer)
{
    if (pointer->wl_pointer)
    {
        wl_pointer_destroy(pointer->wl_pointer);
        pointer->wl_pointer = NULL;
    }
    if (pointer->cursor_wl_surface)
    {
        wl_surface_destroy(pointer->cursor_wl_surface);
        pointer->cursor_wl_surface = NULL;
    }
    if (pointer->cursor)
    {
        wayland_cursor_destroy(pointer->cursor);
        pointer->cursor = NULL;
    }

    pointer->wayland = NULL;
    pointer->focused_surface = NULL;
    pointer->enter_serial = 0;
}

/**********************************************************************
 *          Seat handling
 */

static void seat_handle_capabilities(void *data, struct wl_seat *seat,
                                     enum wl_seat_capability caps)
{
    struct wayland *wayland = data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wayland->pointer.wl_pointer)
    {
        wayland->pointer.wayland = wayland;
        wayland->pointer.wl_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wayland->pointer.wl_pointer, &pointer_listener, wayland);
        wayland->pointer.cursor_wl_surface =
            wl_compositor_create_surface(wayland->wl_compositor);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wayland->pointer.wl_pointer)
    {
        wayland_pointer_deinit(&wayland->pointer);
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wayland->keyboard.wl_keyboard)
    {
        wayland->keyboard.wl_keyboard = wl_seat_get_keyboard(seat);
        /* Some sensible default values for the repeat rate and delay. */
        wayland->keyboard.repeat_interval_ms = 40;
        wayland->keyboard.repeat_delay_ms = 400;
        wl_keyboard_add_listener(wayland->keyboard.wl_keyboard, &keyboard_listener, wayland);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && wayland->keyboard.wl_keyboard)
    {
        wl_keyboard_destroy(wayland->keyboard.wl_keyboard);
        wayland->keyboard.wl_keyboard = NULL;
    }
}

static void seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
    seat_handle_name,
};

/**********************************************************************
 *          Registry handling
 */

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t id, const char *interface,
                                   uint32_t version)
{
    struct wayland *wayland = data;

    TRACE("interface=%s version=%d\n", interface, version);

    if (strcmp(interface, "wl_compositor") == 0)
    {
        wayland->wl_compositor =
            wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    }
    else if (strcmp(interface, "wl_subcompositor") == 0)
    {
        wayland->wl_subcompositor =
            wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
    }
    else if (strcmp(interface, "xdg_wm_base") == 0)
    {
        wayland->xdg_wm_base = wl_registry_bind(registry, id,
                &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wayland->xdg_wm_base, &xdg_wm_base_listener, wayland);
    }
    else if (strcmp(interface, "wl_shm") == 0)
    {
        wayland->wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, "wl_seat") == 0)
    {
        wayland->wl_seat = wl_registry_bind(registry, id, &wl_seat_interface,
                                            version < 5 ? version : 5);
        wl_seat_add_listener(wayland->wl_seat, &seat_listener, wayland);
    }
    else if (strcmp(interface, "wp_viewporter") == 0)
    {
        wayland->wp_viewporter = wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
    }
    else if (strcmp(interface, "wl_data_device_manager") == 0)
    {
        wayland->wl_data_device_manager =
            wl_registry_bind(registry, id, &wl_data_device_manager_interface,
                             version < 3 ? version : 3);
        TRACE("manager=%p\n", wayland->wl_data_device_manager);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
                                          uint32_t name)
{
    /* TODO */
}

static void process_registry_handle_global(void *data, struct wl_registry *registry,
                                           uint32_t id, const char *interface,
                                           uint32_t version)
{
    struct wayland *wayland = data;

    TRACE("interface=%s version=%d\n", interface, version);

    /* For the process wayland instance, we only care about output events
     * and configuration. */
    if (strcmp(interface, "wl_output") == 0)
        wayland_add_output(wayland, id, version);
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

static const struct wl_registry_listener process_registry_listener = {
    process_registry_handle_global,
    registry_handle_global_remove
};

/**********************************************************************
 *          wayland_init
 *
 *  Initialise a wayland instance.
 *
 *  If process_wayland is not NULL, then this initialises a wayland
 *  instance for a thread, using any needed information from process_wayland.
 *  If process_wayland is NULL then this function initialises a wayland
 *  instance for a process.
 *
 *  Each process has a single, limited wayland instance, which creates the
 *  connection to the wayland compositor and gets some basic info (outputs etc).
 *  Each thread then gets its own full instance, using the same connection as
 *  the process wayland instance.
 */
BOOL wayland_init(struct wayland *wayland, const struct wayland *process_wayland)
{
    struct wayland_output *output;

    TRACE("wayland=%p process_wayland=%p\n", wayland, process_wayland);

    wl_list_init(&wayland->thread_link);
    wayland->event_notification_pipe[0] = -1;
    wayland->event_notification_pipe[1] = -1;

    wayland->wl_display =
        process_wayland ? process_wayland->wl_display : wl_display_connect(NULL);

    if (!wayland->wl_display)
    {
        ERR("Failed to connect to wayland compositor\n");
        return FALSE;
    }

    if (!(wayland->wl_event_queue = wl_display_create_queue(wayland->wl_display)))
    {
        ERR("Failed to create event queue\n");
        return FALSE;
    }

    if (!(wayland->buffer_wl_event_queue = wl_display_create_queue(wayland->wl_display)))
    {
        ERR("Failed to create buffer event queue\n");
        return FALSE;
    }

    if (!(wayland->wl_registry = wl_display_get_registry(wayland->wl_display)))
    {
        ERR("Failed to get to wayland registry\n");
        return FALSE;
    }
    wl_proxy_set_queue((struct wl_proxy *) wayland->wl_registry, wayland->wl_event_queue);

    wl_list_init(&wayland->output_list);
    wl_list_init(&wayland->surface_list);

    /* Populate registry */
    if (process_wayland)
        wl_registry_add_listener(wayland->wl_registry, &registry_listener, wayland);
    else
        wl_registry_add_listener(wayland->wl_registry, &process_registry_listener, wayland);

    /* We need two roundtrips. One to get and bind globals, and one to handle all
     * initial events produced from registering the globals. */
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);

    if (wayland->wl_data_device_manager && wayland->wl_seat)
        wayland_data_device_init(wayland);

    InitializeCriticalSection(&wayland->crit);
    wayland->crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": wayland");

    if (process_wayland)
    {
        int flags;
        /* Thread wayland instance have notification pipes to inform them when
         * there might be new events in their queues. The read part of the pipe
         * is also used as the wine server queue fd. */
        if (pipe2(wayland->event_notification_pipe, O_CLOEXEC) == -1)
            return FALSE;
        /* Make just the read end non-blocking */
        if ((flags = fcntl(wayland->event_notification_pipe[0], F_GETFL)) == -1)
            return FALSE;
        if (fcntl(wayland->event_notification_pipe[0], F_SETFL, flags | O_NONBLOCK) == -1)
            return FALSE;
        /* Keep a list of all thread wayland instances, so we can notify them. */
        EnterCriticalSection(&thread_wayland_section);
        wl_list_insert(&thread_wayland_list, &wayland->thread_link);
        LeaveCriticalSection(&thread_wayland_section);
    }

    wl_list_for_each(output, &wayland->output_list, link)
    {
        struct wayland_output_mode *mode;

        TRACE("Output with modes:\n");

        wl_list_for_each(mode, &output->mode_list, link)
        {
            TRACE("  mode %dx%d @ %d %s\n",
                  mode->width, mode->height, mode->refresh, mode->current ? "*" : "");
        }
    }

    return TRUE;
}

/**********************************************************************
 *          wayland_deinit
 *
 *  Deinitialise a wayland instance, releasing all associated resources.
 */
void wayland_deinit(struct wayland *wayland)
{
    struct wayland_output *output, *output_tmp;
    struct wayland_surface *surface, *surface_tmp;
    BOOL owns_wl_display =
        wl_proxy_get_listener((struct wl_proxy *)wayland->wl_registry) ==
            &process_registry_listener;

    TRACE("%p\n", wayland);

    wayland->crit.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&wayland->crit);

    EnterCriticalSection(&thread_wayland_section);
    wl_list_remove(&wayland->thread_link);
    LeaveCriticalSection(&thread_wayland_section);

    if (wayland->pointer.wl_pointer)
        wayland_pointer_deinit(&wayland->pointer);

    if (wayland->keyboard.wl_keyboard)
    {
        wl_keyboard_destroy(wayland->keyboard.wl_keyboard);
        wayland->keyboard.wl_keyboard = NULL;
    }

    if (wayland->wl_data_device)
    {
        wl_data_device_destroy(wayland->wl_data_device);
        wayland->wl_data_device = NULL;
    }

    if (wayland->wl_seat)
    {
        wl_seat_destroy(wayland->wl_seat);
        wayland->wl_seat = NULL;
    }

    if (wayland->wl_shm)
    {
        wl_shm_destroy(wayland->wl_shm);
        wayland->wl_shm = NULL;
    }

    if (wayland->xdg_wm_base)
    {
        xdg_wm_base_destroy(wayland->xdg_wm_base);
        wayland->xdg_wm_base = NULL;
    }

    if (wayland->wl_subcompositor)
    {
        wl_subcompositor_destroy(wayland->wl_subcompositor);
        wayland->wl_subcompositor = NULL;
    }

    if (wayland->wl_compositor)
    {
        wl_compositor_destroy(wayland->wl_compositor);
        wayland->wl_compositor = NULL;
    }

    if (wayland->wl_event_queue)
    {
        wl_event_queue_destroy(wayland->wl_event_queue);
        wayland->wl_event_queue = NULL;
    }

    if (wayland->buffer_wl_event_queue)
    {
        wl_event_queue_destroy(wayland->buffer_wl_event_queue);
        wayland->buffer_wl_event_queue = NULL;
    }

    if (wayland->wl_registry)
    {
        wl_registry_destroy(wayland->wl_registry);
        wayland->wl_registry = NULL;
    }

    wl_list_for_each_safe(output, output_tmp, &wayland->output_list, link)
        wayland_output_destroy(output);

    /* GL surfaces will be destroyed along with their parent surface, so
     * remove them from the list to avoid direct destruction. */
    wl_list_for_each_safe(surface, surface_tmp, &wayland->surface_list, link)
    {
        if (surface->wl_egl_window)
            wl_list_remove(&surface->link);
    }

    wl_list_for_each_safe(surface, surface_tmp, &wayland->surface_list, link)
        wayland_surface_destroy(surface);

    wl_display_flush(wayland->wl_display);

    if (owns_wl_display)
        wl_display_disconnect(wayland->wl_display);

    wayland->wl_display = NULL;
}

/**********************************************************************
 *          wayland_dispatch_non_buffer
 *
 * Dispatch all non-buffer events for the specified wayland instance.
 *
 * Returns the number of events dispatched.
 */
int wayland_dispatch_non_buffer(struct wayland *wayland)
{
    char buf[64];

    TRACE("wayland=%p queue=%p\n", wayland, wayland->wl_event_queue);

    wl_display_flush(wayland->wl_display);

    /* Consume notifications */
    while (TRUE)
    {
        int ret = read(wayland->event_notification_pipe[0], buf, sizeof(buf));
        if (ret > 0) continue;
        if (ret == -1)
        {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) break; /* no data to read */
            ERR("failed to read from notification pipe: %s\n", strerror(errno));
            break;
        }
        if (ret == 0)
        {
            ERR("failed to read from notification pipe: pipe is closed\n");
            break;
        }
    }

    return wl_display_dispatch_queue_pending(wayland->wl_display,
                                             wayland->wl_event_queue);
}

/**********************************************************************
 *          wayland_dispatch_buffer
 *
 * Dispatch buffer related events for the specified wayland instance.
 *
 * Returns the number of events dispatched.
 */
int wayland_dispatch_buffer(struct wayland *wayland)
{
    TRACE("wayland=%p buffer_queue=%p\n", wayland, wayland->buffer_wl_event_queue);

    wl_display_flush(wayland->wl_display);

    return wl_display_dispatch_queue_pending(wayland->wl_display,
                                             wayland->buffer_wl_event_queue);
}

static void wayland_notify_threads(void)
{
    struct wayland *w;
    int ret;

    EnterCriticalSection(&thread_wayland_section);

    wl_list_for_each(w, &thread_wayland_list, thread_link)
    {
        while ((ret = write(w->event_notification_pipe[1], "a", 1)) != 1)
        {
            if (ret == -1 && errno != EINTR)
            {
                ERR("failed to write to notification pipe: %s\n", strerror(errno));
                break;
            }
        }
    }

    LeaveCriticalSection(&thread_wayland_section);
}

/**********************************************************************
 *          wayland_read_events
 *
 * Read wayland events from the compositor, place them in their proper
 * event queues and notify threads about the possibility of new events.
 *
 * Returns whether the operation succeeded.
 */
BOOL wayland_read_events(void)
{
    struct wayland *wayland = &process_wayland;
    struct pollfd pfd = {0};

    pfd.fd = wl_display_get_fd(wayland->wl_display);
    pfd.events = POLLIN;

    TRACE("waiting for events...\n");

    /* In order to read events we need to prepare the read on some
     * queue. We can safely use the default queue, since it's
     * otherwise unused (all struct wayland instances dispatch to
     * their own queues). */
    while (wl_display_prepare_read(wayland->wl_display) != 0)
        wl_display_dispatch_pending(wayland->wl_display);

    wl_display_flush(wayland->wl_display);

    if (poll(&pfd, 1, -1) > 0)
    {
        wl_display_read_events(wayland->wl_display);
        wl_display_dispatch_pending(wayland->wl_display);
        wayland_notify_threads();
        TRACE("waiting for events... done\n");
        return TRUE;
    }

    wl_display_cancel_read(wayland->wl_display);
    return FALSE;
}
