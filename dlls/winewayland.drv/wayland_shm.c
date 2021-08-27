/*
 * Wayland SHM buffers
 *
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

/* For memfd_create */
#define _GNU_SOURCE

#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/mman.h>

#include "waylanddrv.h"
#include "wine/heap.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static int fd_resize(int fd, off_t size)
{
    /*
     * Filesystems that do support fallocate will return EINVAL or
     * EOPNOTSUPP. In this case we need to fall back to ftruncate
     */
    errno = posix_fallocate(fd, 0, size);
    if (errno == 0)
        return 0;
    else if (errno != EINVAL && errno != EOPNOTSUPP)
        return -1;
    if (ftruncate(fd, size) < 0)
        return -1;

    return 0;
}

static int shm_fd_create(off_t size)
{
    int fd;

    fd = memfd_create("wayland-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd >= 0)
    {
        /* We can add this seal before calling posix_fallocate(), as
         * the file is currently zero-sized anyway.
         *
         * There is also no need to check for the return value, we
         * couldn't do anything with it anyway.
         */
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
    }

    while (TRUE)
    {
        int ret = fd_resize(fd, size);
        if (ret == 0) break;
        if (ret < 0 && errno == EINTR) continue;
        close(fd);
        return -1;
    }

    return fd;
}

/**********************************************************************
 *          wayland_shm_buffer_create
 *
 * Creates a SHM buffer with the specified width, height and format.
 */
struct wayland_shm_buffer *wayland_shm_buffer_create(struct wayland *wayland,
                                                     int width, int height,
                                                     enum wl_shm_format format)
{
    struct wayland_shm_buffer *shm_buffer;
    struct wl_shm_pool *pool;
    int fd = -1, size, stride;
    void *data;

    shm_buffer = heap_alloc_zero(sizeof(*shm_buffer));
    if (!shm_buffer)
        goto err;

    wl_list_init(&shm_buffer->link);

    assert(format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888);

    stride = width * 4;
    size = stride * height;

    TRACE("%p %dx%d format=%d size=%d\n", shm_buffer, width, height, format, size);

    if (size == 0)
        return shm_buffer;

    fd = shm_fd_create(size);
    if (fd < 0)
    {
        ERR("creating a buffer fd size %d failed: %s\n", size, strerror(errno));
        goto err;
    }

    data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED)
    {
        ERR("mmap failed: %s size=%d\n", strerror(errno), size);
        goto err;
    }

    pool = wl_shm_create_pool(wayland->wl_shm, fd, size);
    shm_buffer->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
                                                      stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    fd = -1;

    /* Buffer events go to their own queue so that we can dispatch them
     * independently. */
    wl_proxy_set_queue((struct wl_proxy *) shm_buffer->wl_buffer,
                       wayland->buffer_wl_event_queue);

    shm_buffer->width = width;
    shm_buffer->height = height;
    shm_buffer->stride = stride;
    shm_buffer->format = format;
    shm_buffer->map_data = data;
    shm_buffer->map_size = size;
    shm_buffer->damage_region = CreateRectRgn(0, 0, 0, 0);
    if (!shm_buffer->damage_region)
    {
        ERR("failed to create buffer damage region\n");
        goto err;
    }

    TRACE("%p %dx%d size=%d => map=%p\n", shm_buffer, width, height, size, data);

    return shm_buffer;

err:
    if (fd >= 0)
        close(fd);
    if (shm_buffer)
        wayland_shm_buffer_destroy(shm_buffer);
    return NULL;
}

/**********************************************************************
 *          wayland_shm_buffer_destroy
 *
 * Destroys a SHM buffer.
 */
void wayland_shm_buffer_destroy(struct wayland_shm_buffer *shm_buffer)
{
    TRACE("%p map=%p\n", shm_buffer, shm_buffer->map_data);

    wl_list_remove(&shm_buffer->link);

    if (shm_buffer->wl_buffer)
        wl_buffer_destroy(shm_buffer->wl_buffer);
    if (shm_buffer->map_data)
        munmap(shm_buffer->map_data, shm_buffer->map_size);
    if (shm_buffer->damage_region)
        DeleteObject(shm_buffer->damage_region);

    heap_free(shm_buffer);
}

/**********************************************************************
 *          wayland_shm_buffer_clear_damage
 *
 *  Clears all damage accumulated by a SHM buffer.
 */
void wayland_shm_buffer_clear_damage(struct wayland_shm_buffer *shm_buffer)
{
    SetRectRgn(shm_buffer->damage_region, 0, 0, 0, 0);
}

/**********************************************************************
 *          wayland_shm_buffer_add_damage
 *
 *  Adds damage (i.e., a region which needs update) to a SHM buffer.
 */
void wayland_shm_buffer_add_damage(struct wayland_shm_buffer *shm_buffer, HRGN damage)
{
    CombineRgn(shm_buffer->damage_region, shm_buffer->damage_region, damage, RGN_OR);
}

/**********************************************************************
 *          wayland_shm_buffer_get_damage_clipped
 *
 * Returns the damage region data for this buffer clipped within the
 * provided clip region (if any).
 *
 * The returned RGNDATA* should be freed by the caller.
 */
RGNDATA *wayland_shm_buffer_get_damage_clipped(struct wayland_shm_buffer *shm_buffer,
                                               HRGN clip)
{
    RGNDATA *data = NULL;
    DWORD size;
    HRGN damage_region;

    if (clip)
    {
        damage_region = CreateRectRgn(0, 0, 0, 0);
        if (!damage_region) goto err;
        CombineRgn(damage_region, shm_buffer->damage_region, clip, RGN_AND);
    }
    else
    {
        damage_region = shm_buffer->damage_region;
    }

    if (!(size = GetRegionData( damage_region, 0, NULL ))) goto err;
    if (!(data = heap_alloc_zero( size ))) goto err;
    if (!GetRegionData( damage_region, size, data )) goto err;

    if (damage_region != shm_buffer->damage_region)
        DeleteObject(damage_region);

    return data;

err:
    if (damage_region && damage_region != shm_buffer->damage_region)
        DeleteObject(damage_region);
    heap_free(data);
    return NULL;
}
