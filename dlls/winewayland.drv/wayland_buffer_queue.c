/*
 * Wayland buffer queue
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
#include "wine/unicode.h"
#include "winuser.h"

#include <errno.h>
#include <assert.h>
#include <time.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct wayland_shm_buffer *shm_buffer = data;

    TRACE("shm_buffer=%p\n", shm_buffer);

    shm_buffer->busy = FALSE;
}

static const struct wl_buffer_listener buffer_listener = {
    buffer_release
};

/**********************************************************************
 *          wayland_buffer_queue_create
 *
 * Creates a buffer queue containing buffers with the specified width, height
 * and format.
 */
struct wayland_buffer_queue *wayland_buffer_queue_create(struct wayland *wayland,
                                                         int width, int height,
                                                         enum wl_shm_format format)
{
    struct wayland_buffer_queue *queue;

    queue = heap_alloc_zero(sizeof(*queue));
    if (!queue)
        return NULL;

    queue->wayland = wayland;
    queue->width = width;
    queue->height = height;
    queue->format = format;

    wl_list_init(&queue->buffer_list);

    return queue;
}

/**********************************************************************
 *          wayland_buffer_queue_destroy
 *
 * Destroys a buffer queue and any contained buffers.
 */
void wayland_buffer_queue_destroy(struct wayland_buffer_queue *queue)
{
    struct wayland_shm_buffer *shm_buffer, *next;

    wl_list_for_each_safe(shm_buffer, next, &queue->buffer_list, link)
        wayland_shm_buffer_destroy(shm_buffer);

    heap_free(queue);
}

/**********************************************************************
 *          wayland_buffer_queue_acquire_buffer
 *
 * Acquires a free buffer from the buffer queue. If no free buffers
 * are available this function blocks until it can provide one.
 *
 * The returned buffer is marked as unavailable until committed to
 * a surface and subsequently released by the compositor.
 */
struct wayland_shm_buffer *wayland_buffer_queue_acquire_buffer(struct wayland_buffer_queue *queue)
{
    struct wayland_shm_buffer *shm_buffer;

    TRACE("queue=%p\n", queue);

    while (TRUE)
    {
        int nbuffers = 0;

        /* Search through our buffers to find an available one. */
        wl_list_for_each(shm_buffer, &queue->buffer_list, link)
        {
            if (!shm_buffer->busy)
            {
                shm_buffer->busy = TRUE;
                goto out;
            }
            nbuffers++;
        }

        /* Dynamically create up to 3 buffers. */
        if (nbuffers < 3)
        {
            HRGN full_dmg = CreateRectRgn(0, 0, queue->width, queue->height);
            shm_buffer = wayland_shm_buffer_create(queue->wayland, queue->width,
                                                   queue->height, queue->format);
            if (shm_buffer)
            {
                wl_buffer_add_listener(shm_buffer->wl_buffer, &buffer_listener,
                                       shm_buffer);
                wl_list_insert(&queue->buffer_list, &shm_buffer->link);
                wayland_shm_buffer_add_damage(shm_buffer, full_dmg);
                shm_buffer->busy = TRUE;
            }
            DeleteObject(full_dmg);
            /* If we failed to allocate a new buffer, but we have at least two
             * buffers busy, there is a good chance the compositor will
             * eventually release one of them, so dispatch events and wait
             * below. Otherwise, give up and return a NULL buffer. */
            if (shm_buffer)
            {
                goto out;
            }
            else if (nbuffers < 2)
            {
                ERR(" => failed to acquire buffer\n");
                return NULL;
            }
        }

        /* Keep dispatching buffer events to handle any pending buffer
         * releases. If we have no events (and thus no newly available buffer),
         * yield to allow other threads to perform work. */
        while (wayland_dispatch_buffer(queue->wayland) == 0)
            Sleep(0);
    }

out:
    TRACE(" => %p %dx%d stride=%d map=[%p, %p)\n",
          shm_buffer, shm_buffer->width, shm_buffer->height,
          shm_buffer->stride, shm_buffer->map_data,
          (unsigned char*)shm_buffer->map_data + shm_buffer->map_size);

    return shm_buffer;
}

/**********************************************************************
 *          wayland_buffer_queue_add_damage
 *
 * Adds damage to all buffers in this queue.
 */
void wayland_buffer_queue_add_damage(struct wayland_buffer_queue *queue, HRGN damage)
{
    struct wayland_shm_buffer *shm_buffer;

    wl_list_for_each(shm_buffer, &queue->buffer_list, link)
        wayland_shm_buffer_add_damage(shm_buffer, damage);
}
