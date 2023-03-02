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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "waylanddrv.h"

#include "wine/debug.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

struct wl_display *process_wl_display = NULL;

static struct wayland_mutex process_wayland_mutex =
{
    PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP, 0, 0, __FILE__ ": process_wayland_mutex"
};
static struct wayland *process_wayland = NULL;

static struct wayland_mutex thread_wayland_mutex =
{
    PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP, 0, 0, __FILE__ ": thread_wayland_mutex"
};
static struct wl_list thread_wayland_list = {&thread_wayland_list, &thread_wayland_list};

/**********************************************************************
 *          wayland_mutex_lock
 *
 *  Lock a mutex, emitting error messages in cases of suspected deadlock.
 *  In case of an unrecoverable error abort to ensure the program doesn't
 *  continue with an inconsistent state.
 */
void wayland_mutex_lock(struct wayland_mutex *wayland_mutex)
{
    UINT tid = GetCurrentThreadId();
    struct timespec timeout;
    int err;

    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 5;

    while (TRUE)
    {
        err = pthread_mutex_timedlock(&wayland_mutex->mutex, &timeout);
        if (!err) break;

        if (err == ETIMEDOUT)
        {
            ERR("mutex %p %s lock timed out in thread %04x, blocked by %04x, retrying (60 sec)\n",
                wayland_mutex, wayland_mutex->name, tid, wayland_mutex->owner_tid);
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 60;
        }
        else
        {
            ERR("error locking mutex %p %s errno=%d, aborting\n",
                wayland_mutex, wayland_mutex->name, errno);
            abort();
        }
    }

    wayland_mutex->owner_tid = tid;
    wayland_mutex->lock_count++;
}

/**********************************************************************
 *          wayland_mutex_unlock
 *
 *  Unlock a mutex.
 */
void wayland_mutex_unlock(struct wayland_mutex *wayland_mutex)
{
    int err;

    wayland_mutex->lock_count--;

    if (wayland_mutex->lock_count == 0)
    {
        wayland_mutex->owner_tid = 0;
    }
    else if (wayland_mutex->lock_count < 0)
    {
        ERR("mutex %p %s lock_count is %d < 0\n",
             wayland_mutex, wayland_mutex->name, wayland_mutex->lock_count);
    }

    if ((err = pthread_mutex_unlock(&wayland_mutex->mutex)))
    {
        ERR("failed to unlock mutex %p %s errno=%d\n",
            wayland_mutex, wayland_mutex->name, err);
    }
}

/**********************************************************************
 *          Registry handling
 */

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t id, const char *interface,
                                   uint32_t version)
{
    struct wayland *wayland = data;

    TRACE("interface=%s version=%u id=%u\n", interface, version, id);

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
    else if (strcmp(interface, "wl_shm") == 0)
    {
        wayland->wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
                                          uint32_t id)
{
    struct wayland *wayland = data;
    struct wayland_output *output, *tmp;

    TRACE("id=%u\n", id);

    wl_list_for_each_safe(output, tmp, &wayland->output_list, link)
    {
        if (output->global_id == id)
        {
            TRACE("removing output->name=%s\n", output->name);
            wayland_output_destroy(output);
            if (wayland_is_process(wayland))
            {
                /* Temporarily release the per-process instance lock, so that
                 * wayland_init_display_devices can perform more fine grained
                 * locking to avoid deadlocks. */
                wayland_process_release();
                wayland_init_display_devices();
                wayland_process_acquire();
            }
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
    struct wl_display *wl_display_wrapper;

    TRACE("wayland=%p wl_display=%p\n", wayland, process_wl_display);

    wl_list_init(&wayland->thread_link);

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

    if (!(wl_display_wrapper = wl_proxy_create_wrapper(wayland->wl_display)))
    {
        ERR("Failed to create proxy wrapper for wl_display\n");
        return FALSE;
    }
    wl_proxy_set_queue((struct wl_proxy *) wl_display_wrapper, wayland->wl_event_queue);

    wayland->wl_registry = wl_display_get_registry(wl_display_wrapper);
    wl_proxy_wrapper_destroy(wl_display_wrapper);
    if (!wayland->wl_registry)
    {
        ERR("Failed to get to wayland registry\n");
        return FALSE;
    }

    wl_list_init(&wayland->output_list);

    /* Populate registry */
    wl_registry_add_listener(wayland->wl_registry, &registry_listener, wayland);

    /* We need three roundtrips. One to get and bind globals, one to handle all
     * initial events produced from registering the globals and one more to
     * handle potential third-order registrations. */
    if (wayland_is_process(wayland)) wayland_process_acquire();
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);
    if (wayland_is_process(wayland)) wayland_process_release();

    if (!wayland_is_process(wayland))
    {
        /* Keep a list of all thread wayland instances. */
        wayland_mutex_lock(&thread_wayland_mutex);
        wl_list_insert(&thread_wayland_list, &wayland->thread_link);
        wayland_mutex_unlock(&thread_wayland_mutex);
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

    TRACE("%p\n", wayland);

    wayland_mutex_lock(&thread_wayland_mutex);
    wl_list_remove(&wayland->thread_link);
    wayland_mutex_unlock(&thread_wayland_mutex);

    wl_list_for_each_safe(output, output_tmp, &wayland->output_list, link)
        wayland_output_destroy(output);

    if (wayland->wl_shm)
        wl_shm_destroy(wayland->wl_shm);

    if (wayland->zxdg_output_manager_v1)
        zxdg_output_manager_v1_destroy(wayland->zxdg_output_manager_v1);

    if (wayland->wl_compositor)
        wl_compositor_destroy(wayland->wl_compositor);

    if (wayland->wl_registry)
        wl_registry_destroy(wayland->wl_registry);

    if (wayland->wl_event_queue)
        wl_event_queue_destroy(wayland->wl_event_queue);

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

    process_wayland = calloc(1, sizeof(*process_wayland));
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
    wayland_mutex_lock(&process_wayland_mutex);
    return process_wayland;
}

/**********************************************************************
 *          wayland_process_release
 *
 *  Releases the per-process wayland instance.
 */
void wayland_process_release(void)
{
    wayland_mutex_unlock(&process_wayland_mutex);
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

    wayland_mutex_lock(&thread_wayland_mutex);

    /* Each thread maintains its own output information, so we need to notify
     * all threads about the change. We can't guarantee that all threads will
     * have windows to which we could potentially send the notification message
     * to, so we use the internal send function to target the threads directly.
     * We can't use PostThreadMessage since we require synchronous message
     * handling. */
    wl_list_for_each(w, &thread_wayland_list, thread_link)
    {
        LRESULT res;
        TRACE("notifying thread %04x\n", (UINT)w->thread_id);
        /* Use a timeout of 50ms to avoid blocking indefinitely if the
         * target thread is not processing (and to avoid deadlocks). */
        res = __wine_send_internal_message_timeout(w->process_id, w->thread_id,
                                                   WM_WAYLAND_MONITOR_CHANGE,
                                                   0, 0, 0, 50, NULL);
        /* If we weren't able to synchronously send the message, post it. */
        if (!res)
            NtUserPostThreadMessage(w->thread_id, WM_WAYLAND_MONITOR_CHANGE, 0, 0);
    }

    wayland_mutex_unlock(&thread_wayland_mutex);
}

/**********************************************************************
 *          wayland_dispatch_queue
 *
 * Dispatch events from the specified queue. If the queue is empty,
 * wait for timeout_ms for events to arrive and then dispatch any events in
 * the queue.
 *
 * Returns the number of events dispatched, -1 on error
 */
int wayland_dispatch_queue(struct wl_event_queue *queue, int timeout_ms)
{
    struct pollfd pfd = {0};
    BOOL is_process_queue = queue == process_wayland->wl_event_queue;
    int ret;

    TRACE("waiting for events with timeout=%d ...\n", timeout_ms);

    pfd.fd = wl_display_get_fd(process_wl_display);

    if (wl_display_prepare_read_queue(process_wl_display, queue) == -1)
    {
        if (is_process_queue) wayland_process_acquire();
        if ((ret = wl_display_dispatch_queue_pending(process_wl_display, queue)) == -1)
            TRACE("... failed wl_display_dispatch_queue_pending errno=%d\n", errno);
        if (is_process_queue) wayland_process_release();
        TRACE("... done early\n");
        return ret;
    }

    while (TRUE)
    {
        ret = wl_display_flush(process_wl_display);

        if (ret != -1 || errno != EAGAIN)
            break;

        pfd.events = POLLOUT;
        while ((ret = poll(&pfd, 1, timeout_ms)) == -1 && errno == EINTR) continue;

        if (ret == -1)
        {
            TRACE("... failed poll out errno=%d\n", errno);
            wl_display_cancel_read(process_wl_display);
            return -1;
        }
    }

    if (ret < 0 && errno != EPIPE)
    {
        wl_display_cancel_read(process_wl_display);
        return -1;
    }

    pfd.events = POLLIN;
    while ((ret = poll(&pfd, 1, timeout_ms)) == -1 && errno == EINTR) continue;

    if (ret == 0)
    {
        TRACE("... done => 0 events (timeout)\n");
        wl_display_cancel_read(process_wl_display);
        return 0;
    }

    if (ret == -1)
    {
        TRACE("... failed poll errno=%d\n", errno);
        wl_display_cancel_read(process_wl_display);
        return -1;
    }

    if (wl_display_read_events(process_wl_display) == -1)
    {
        TRACE("... failed wl_display_read_events errno=%d\n", errno);
        return -1;
    }

    if (is_process_queue) wayland_process_acquire();
    ret = wl_display_dispatch_queue_pending(process_wl_display, queue);
    if (is_process_queue) wayland_process_release();
    if (ret == -1)
    {
        TRACE("... failed wl_display_dispatch_queue_pending errno=%d\n", errno);
        return -1;
    }

    TRACE("... done => %d events\n", ret);

    return ret;
}
