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

#include "waylanddrv.h"

#include "wine/debug.h"
#include "wine/heap.h"

#include "winuser.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

struct wl_display *process_wl_display = NULL;
static struct wayland *process_wayland = NULL;
CRITICAL_SECTION process_wayland_section;
static CRITICAL_SECTION_DEBUG process_wayland_critsect_debug =
{
    0, 0, &process_wayland_section,
    { &process_wayland_critsect_debug.ProcessLocksList,
      &process_wayland_critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": process_wayland_section") }
};
CRITICAL_SECTION process_wayland_section = { &process_wayland_critsect_debug,
                                             -1, 0, 0, 0, 0 };

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
 *          xdg_wm_base handling
 */

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    xdg_wm_base_ping,
};

/**********************************************************************
 *          Seat handling
 */

static void seat_handle_capabilities(void *data, struct wl_seat *seat,
                                     enum wl_seat_capability caps)
{
    struct wayland *wayland = data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wayland->pointer.wl_pointer)
    {
        wayland_pointer_init(&wayland->pointer, wayland, wl_seat_get_pointer(seat));
    }
    else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wayland->pointer.wl_pointer)
    {
        wayland_pointer_deinit(&wayland->pointer);
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wayland->keyboard.wl_keyboard)
    {
        wayland_keyboard_init(&wayland->keyboard, wayland, wl_seat_get_keyboard(seat));
    }
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && wayland->keyboard.wl_keyboard)
    {
        wayland_keyboard_deinit(&wayland->keyboard);
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

    TRACE("interface=%s version=%d\n id=%u\n", interface, version, id);

    if (strcmp(interface, "wl_output") == 0)
    {
        if (!wayland_output_create(wayland, id, version))
            ERR("Failed to create wayland_output for global id=%u\n", id);
    }
    else if (strcmp(interface, "zxdg_output_manager_v1") == 0)
    {
        struct wayland_output *output;

        wayland->zxdg_output_manager_v1 =
            wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface,
                             version < 3 ? version : 3);

        /* Add zxdg_output_v1 to existing outputs. */
        wl_list_for_each(output, &wayland->output_list, link)
            wayland_output_use_xdg_extension(output);
    }

    /* The per-process wayland instance only handles output related globals. */
    if (wayland_is_process(wayland)) return;

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
    }
    else if (strcmp(interface, "zwp_pointer_constraints_v1") == 0)
    {
        wayland->zwp_pointer_constraints_v1 =
            wl_registry_bind(registry, id, &zwp_pointer_constraints_v1_interface, 1);
    }
    else if (strcmp(interface, "zwp_relative_pointer_manager_v1") == 0)
    {
        wayland->zwp_relative_pointer_manager_v1 =
            wl_registry_bind(registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
                                          uint32_t id)
{
    struct wayland *wayland = data;
    struct wayland_output *output, *tmp;

    TRACE("id=%d\n", id);

    wl_list_for_each_safe(output, tmp, &wayland->output_list, link)
    {
        if (output->global_id == id)
        {
            struct wayland_surface *surface;

            TRACE("removing output->name=%s\n", output->name);

            /* Remove the output from toplevels, as some compositors don't send
             * a leave event if the output is disconnected. */
            wl_list_for_each(surface, &wayland->toplevel_list, link)
            {
                wayland_surface_leave_output(surface, output);
                if (surface->wine_output == output)
                    wayland_surface_set_wine_output(surface, NULL);
            }

            wayland_output_destroy(output);
            if (wayland_is_process(wayland))
                wayland_init_display_devices();
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

/**********************************************************************
 *          wayland_init
 *
 *  Initialise a wayland instance.
 */
BOOL wayland_init(struct wayland *wayland)
{
    int flags;

    TRACE("wayland=%p wl_display=%p\n", wayland, process_wl_display);

    wl_list_init(&wayland->thread_link);
    wayland->event_notification_pipe[0] = -1;
    wayland->event_notification_pipe[1] = -1;

    wayland->process_id = GetCurrentProcessId();
    wayland->thread_id = GetCurrentThreadId();
    wayland->wl_display = process_wl_display;

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
    wl_list_init(&wayland->toplevel_list);

    SetRect(&wayland->cursor_clip, INT_MIN, INT_MIN, INT_MAX, INT_MAX);

    /* Populate registry */
    wl_registry_add_listener(wayland->wl_registry, &registry_listener, wayland);

    /* We need three roundtrips. One to get and bind globals, one to handle all
     * initial events produced from registering the globals and one more to
     * handle potential third-order registrations. */
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);

    if (!wayland_is_process(wayland))
    {
        if (wayland->wl_data_device_manager && wayland->wl_seat)
            wayland_data_device_init(&wayland->data_device, wayland);

        /* Thread wayland instances have notification pipes to inform them when
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

    wayland->initialized = TRUE;

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
    struct wayland_surface *toplevel, *toplevel_tmp;

    TRACE("%p\n", wayland);

    EnterCriticalSection(&thread_wayland_section);
    wl_list_remove(&wayland->thread_link);
    LeaveCriticalSection(&thread_wayland_section);

    wl_list_for_each_safe(toplevel, toplevel_tmp, &wayland->toplevel_list, link)
        wayland_surface_destroy(toplevel);

    if (wayland->event_notification_pipe[0] >= 0)
        close(wayland->event_notification_pipe[0]);
    if (wayland->event_notification_pipe[1] >= 0)
        close(wayland->event_notification_pipe[1]);

    wl_list_for_each_safe(output, output_tmp, &wayland->output_list, link)
        wayland_output_destroy(output);

    if (wayland->pointer.wl_pointer)
        wayland_pointer_deinit(&wayland->pointer);

    if (wayland->keyboard.wl_keyboard)
        wayland_keyboard_deinit(&wayland->keyboard);

    if (wayland->data_device.wl_data_device)
        wayland_data_device_deinit(&wayland->data_device);

    if (wayland->wl_data_device_manager)
        wl_data_device_manager_destroy(wayland->wl_data_device_manager);

    if (wayland->wl_seat)
        wl_seat_destroy(wayland->wl_seat);

    if (wayland->zwp_relative_pointer_manager_v1)
        zwp_relative_pointer_manager_v1_destroy(wayland->zwp_relative_pointer_manager_v1);

    if (wayland->wp_viewporter)
        wp_viewporter_destroy(wayland->wp_viewporter);

    if (wayland->wl_shm)
        wl_shm_destroy(wayland->wl_shm);

    if (wayland->zxdg_output_manager_v1)
        zxdg_output_manager_v1_destroy(wayland->zxdg_output_manager_v1);

    if (wayland->xdg_wm_base)
        xdg_wm_base_destroy(wayland->xdg_wm_base);

    if (wayland->wl_subcompositor)
        wl_subcompositor_destroy(wayland->wl_subcompositor);

    if (wayland->wl_compositor)
        wl_compositor_destroy(wayland->wl_compositor);

    if (wayland->wl_registry)
        wl_registry_destroy(wayland->wl_registry);

    if (wayland->wl_event_queue)
        wl_event_queue_destroy(wayland->wl_event_queue);

    if (wayland->buffer_wl_event_queue)
    {
        wl_event_queue_destroy(wayland->buffer_wl_event_queue);
        wayland->buffer_wl_event_queue = NULL;
    }

    wl_display_flush(wayland->wl_display);

    memset(wayland, 0, sizeof(*wayland));
}

/**********************************************************************
 *          wayland_process_init
 *
 *  Initialise the per process wayland objects.
 *
 */
BOOL wayland_process_init(void)
{
    process_wl_display = wl_display_connect(NULL);
    if (!process_wl_display)
        return FALSE;

    process_wayland = heap_alloc_zero(sizeof(*process_wayland));
    if (!process_wayland)
        return FALSE;

    return wayland_init(process_wayland);
}

/**********************************************************************
 *          wayland_is_process
 *
 *  Checks whether a wayland instance is the per-process one.
 */
BOOL wayland_is_process(struct wayland *wayland)
{
    return wayland == process_wayland;
}

/**********************************************************************
 *          wayland_process_acquire
 *
 *  Acquires the per-process wayland instance.
 */
struct wayland *wayland_process_acquire(void)
{
    EnterCriticalSection(&process_wayland_section);
    return process_wayland;
}

/**********************************************************************
 *          wayland_process_release
 *
 *  Releases the per-process wayland instance.
 */
void wayland_process_release(void)
{
    LeaveCriticalSection(&process_wayland_section);
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
    struct pollfd pfd = {0};
    int ret;

    pfd.fd = wl_display_get_fd(process_wl_display);
    pfd.events = POLLIN;

    TRACE("waiting for events...\n");

    /* In order to read events we need to prepare the read on some
     * queue. We can safely use the default queue, since it's
     * otherwise unused (all struct wayland instances dispatch to
     * their own queues). */
    while (wl_display_prepare_read(process_wl_display) != 0)
    {
        if (wl_display_dispatch_pending(process_wl_display) == -1)
        {
            TRACE("... failed wl_display_dispatch_pending errno=%d\n", errno);
            return FALSE;
        }
    }

    wl_display_flush(process_wl_display);

    while ((ret = poll(&pfd, 1, -1)) == -1 && errno == EINTR) continue;

    if (ret == -1 || !(pfd.revents & POLLIN))
    {
        TRACE("... failed poll errno=%d revents=0x%x\n",
              ret == -1 ? errno : 0, pfd.revents);
        wl_display_cancel_read(process_wl_display);
        return FALSE;
    }

    if (wl_display_read_events(process_wl_display) == -1)
    {
        TRACE("... failed wl_display_read_events errno=%d\n", errno);
        return FALSE;
    }

    if (wl_display_dispatch_pending(process_wl_display) == -1)
    {
        TRACE("... failed wl_display_dispatch_pending errno=%d\n", errno);
        return FALSE;
    }

    wayland_notify_threads();

    TRACE("... done\n");

    return TRUE;
}

static BOOL process_wayland_events(DWORD mask)
{
    struct wayland *wayland = thread_wayland();
    int dispatched;

    if (!wayland)
        return FALSE;

    wayland->last_dispatch_mask = 0;

    dispatched = wayland_dispatch_non_buffer(wayland);
    if (dispatched)
        wayland->last_dispatch_mask |= QS_SENDMESSAGE;

    TRACE("dispatched=%d mask=%s%s%s%s%s%s%s\n",
          dispatched,
          (wayland->last_dispatch_mask & QS_KEY) ? "QS_KEY|" : "",
          (wayland->last_dispatch_mask & QS_MOUSEMOVE) ? "QS_MOUSEMOVE|" : "",
          (wayland->last_dispatch_mask & QS_MOUSEBUTTON) ? "QS_MOUSEBUTTON|" : "",
          (wayland->last_dispatch_mask & QS_INPUT) ? "QS_INPUT|" : "",
          (wayland->last_dispatch_mask & QS_PAINT) ? "QS_PAINT|" : "",
          (wayland->last_dispatch_mask & QS_POSTMESSAGE) ? "QS_POSTMESSAGE|" : "",
          (wayland->last_dispatch_mask & QS_SENDMESSAGE) ? "QS_SENDMESSAGE|" : "");

    return wayland->last_dispatch_mask & mask;
}

/***********************************************************************
 *           WAYLAND_MsgWaitForMultipleObjectsEx
 */
DWORD CDECL WAYLAND_MsgWaitForMultipleObjectsEx(DWORD count, const HANDLE *handles,
                                                DWORD timeout, DWORD mask, DWORD flags)
{
    DWORD ret;

    if (process_wayland_events(mask))
        return count - 1;

    ret = WaitForMultipleObjectsEx(count, handles, flags & MWMO_WAITALL,
                                   timeout, flags & MWMO_ALERTABLE);

    return ret;
}

/**********************************************************************
 *          wayland_notify_wine_monitor_change
 *
 * Notify all wayland instances about a change in the state of wine monitors.
 * The notification is synchronous, this function returns after all wayland
 * instances have handled the event, except if it a thread is slow to process
 * the message, and thus likely to be blocked by this synchronous operation.
 */
void wayland_notify_wine_monitor_change(void)
{
    struct wayland *w;

    EnterCriticalSection(&thread_wayland_section);

    /* Each thread maintains its own output mode information, so we need to
     * notify all threads about the wine mode change. We can't guarantee that
     * all threads will have windows to which we could potentially send the
     * notification message to, so we use the internal send function to target
     * the threads directly. We can't use PostThreadMessage since we require
     * synchronous message handling. */
    wl_list_for_each(w, &thread_wayland_list, thread_link)
    {
        /* Use a timeout of 50ms to avoid blocking indefinitely if the
         * target thread is not processing (and to avoid deadlocks). */
        __wine_send_internal_message_timeout(w->process_id, w->thread_id,
                                             WM_WAYLAND_MONITOR_CHANGE,
                                             0, 0, 0, 50, NULL);
    }

    LeaveCriticalSection(&thread_wayland_section);
}
