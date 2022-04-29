/*
 * Wayland dmabuf buffers
 *
 * Copyright 2022 Alexandros Frantzis for Collabora Ltd
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

#include <drm_fourcc.h>
#include <stdlib.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static struct wayland_dmabuf_format *wayland_dmabuf_find_format(struct wayland_dmabuf *dmabuf,
                                                                uint32_t format)
{
    struct wayland_dmabuf_format *dmabuf_format;

    wl_list_for_each(dmabuf_format, &dmabuf->formats, link)
        if (dmabuf_format->format == format) break;

    if (&dmabuf_format->link == &dmabuf->formats) dmabuf_format = NULL;

    return dmabuf_format;
}

static BOOL wayland_dmabuf_format_find_modifier(struct wayland_dmabuf_format *format, uint64_t modifier)
{
    uint64_t *mod;

    wl_array_for_each(mod, &format->modifiers)
        if (*mod == modifier) return TRUE;

    return FALSE;
}

static void *wayland_dmabuf_add_format_modifier(struct wayland_dmabuf *dmabuf,
                                                uint32_t format, uint64_t modifier)
{
    struct wayland_dmabuf_format *dmabuf_format;
    uint64_t *mod;

    if ((dmabuf_format = wayland_dmabuf_find_format(dmabuf, format)))
    {
        /* Avoid a possible duplicate, e.g., if compositor sends both format and
         * modifier event with a DRM_FORMAT_MOD_INVALID. */
        if (wayland_dmabuf_format_find_modifier(dmabuf_format, modifier))
            goto out;
    }
    else
    {
        if (!(dmabuf_format = malloc(sizeof(struct wayland_dmabuf_format))))
            return NULL;
        dmabuf_format->format = format;
        wl_array_init(&dmabuf_format->modifiers);
        wl_list_insert(&dmabuf->formats, &dmabuf_format->link);
    }

    mod = wl_array_add(&dmabuf_format->modifiers, sizeof(uint64_t));
    if (mod) *mod = modifier;
    else dmabuf_format = NULL;

out:
    return dmabuf_format;
}

/**********************************************************************
 *          zwp_linux_dmabuf_v1 implementation
 */

static void dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_dmabuf, uint32_t format)
{
    struct wayland_dmabuf *dmabuf = data;

    if (!wayland_dmabuf_add_format_modifier(dmabuf, format, DRM_FORMAT_MOD_INVALID))
        WARN("Could not add format 0x%08x\n", format);
}

static void dmabuf_modifiers(void *data, struct zwp_linux_dmabuf_v1 *zwp_dmabuf, uint32_t format,
                             uint32_t mod_hi, uint32_t mod_lo)
{
    struct wayland_dmabuf *dmabuf = data;
    const uint64_t modifier = (uint64_t)mod_hi << 32 | mod_lo;

    if (!wayland_dmabuf_add_format_modifier(dmabuf, format, modifier))
        WARN("Could not add format/modifier 0x%08x/0x%llx\n", format, modifier);
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    dmabuf_format,
    dmabuf_modifiers
};

/**********************************************************************
 *          zwp_linux_dmabuf_feedback_v1 handling
 */
static void dmabuf_feedback_main_device(void *data,
                                        struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
                                        struct wl_array *device)
{
    struct wayland_dmabuf_feedback *feedback = data;

    if (device->size != sizeof(feedback->main_device))
        return;

    memcpy(&feedback->main_device, device->data, device->size);
}

static void dmabuf_feedback_format_table(void *data,
                                         struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
                                         int32_t fd, uint32_t size)
{
    /* ignore for now */
}

static void dmabuf_feedback_tranche_target_device(void *data,
                                                  struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
                                                  struct wl_array *device)
{
    /* ignore for now */
}

static void dmabuf_feedback_tranche_formats(void *data,
                                            struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
                                            struct wl_array *indices)
{
    /* ignore for now */
}

static void dmabuf_feedback_tranche_flags(void *data,
                                          struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
                                          uint32_t flags)
{
    /* ignore for now */
}

static void dmabuf_feedback_tranche_done(void *data,
                                         struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1)
{
    /* ignore for now */
}

static void dmabuf_feedback_done(void *data,
                                 struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1)
{
    struct wayland_dmabuf_feedback *feedback = data;

    zwp_linux_dmabuf_feedback_v1_destroy(feedback->zwp_linux_dmabuf_feedback_v1);
    feedback->zwp_linux_dmabuf_feedback_v1 = NULL;
}

static const struct zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_listener =
{
    .main_device = dmabuf_feedback_main_device,
    .format_table = dmabuf_feedback_format_table,
    .tranche_target_device = dmabuf_feedback_tranche_target_device,
    .tranche_formats = dmabuf_feedback_tranche_formats,
    .tranche_flags = dmabuf_feedback_tranche_flags,
    .tranche_done = dmabuf_feedback_tranche_done,
    .done = dmabuf_feedback_done,
};

static void wayland_dmabuf_feedback_init(struct wayland_dmabuf_feedback *feedback,
                                         struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1)
{
    feedback->zwp_linux_dmabuf_feedback_v1 =
        zwp_linux_dmabuf_v1_get_default_feedback(zwp_linux_dmabuf_v1);
    zwp_linux_dmabuf_feedback_v1_add_listener(feedback->zwp_linux_dmabuf_feedback_v1,
                                              &dmabuf_feedback_listener, feedback);
}

static void wayland_dmabuf_feedback_deinit(struct wayland_dmabuf_feedback *feedback)
{
    if (feedback->zwp_linux_dmabuf_feedback_v1)
        zwp_linux_dmabuf_feedback_v1_destroy(feedback->zwp_linux_dmabuf_feedback_v1);
}

/***********************************************************************
 *           wayland_dmabuf_init
 */
void wayland_dmabuf_init(struct wayland_dmabuf *dmabuf,
                         struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1)
{
    uint32_t dmabuf_version =
        wl_proxy_get_version((struct wl_proxy *)zwp_linux_dmabuf_v1);

    dmabuf->zwp_linux_dmabuf_v1 = zwp_linux_dmabuf_v1;
    wl_list_init(&dmabuf->formats);

    /* If the compositor supports dmabuf feedback events, it must not send
     * format and modifier events, so don't even listen for them in that case. */
    if (dmabuf_version >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION)
        wayland_dmabuf_feedback_init(&dmabuf->feedback, zwp_linux_dmabuf_v1);
    else
        zwp_linux_dmabuf_v1_add_listener(zwp_linux_dmabuf_v1, &dmabuf_listener, dmabuf);
}

/***********************************************************************
 *           wayland_dmabuf_deinit
 */
void wayland_dmabuf_deinit(struct wayland_dmabuf *dmabuf)
{
    struct wayland_dmabuf_format *format, *tmp;

    wayland_dmabuf_feedback_deinit(&dmabuf->feedback);

    if (dmabuf->zwp_linux_dmabuf_v1)
        zwp_linux_dmabuf_v1_destroy(dmabuf->zwp_linux_dmabuf_v1);

    wl_list_for_each_safe(format, tmp, &dmabuf->formats, link)
    {
        wl_array_release(&format->modifiers);
        wl_list_remove(&format->link);
        free(format);
    }
}

/**********************************************************************
 *          wayland_dmabuf_buffer_from_native
 *
 * Creates a wayland dmabuf buffer from the specified native buffer.
 */
struct wayland_dmabuf_buffer *wayland_dmabuf_buffer_create_from_native(struct wayland *wayland,
                                                                       struct wayland_native_buffer *native)
{
    struct wayland_dmabuf_buffer *dmabuf_buffer;
    struct zwp_linux_buffer_params_v1 *params;
    int i;

    dmabuf_buffer = calloc(1, sizeof(*dmabuf_buffer));
    if (!dmabuf_buffer)
        goto err;

    params = zwp_linux_dmabuf_v1_create_params(wayland->dmabuf.zwp_linux_dmabuf_v1);
    for (i = 0; i < native->plane_count; i++)
    {
        zwp_linux_buffer_params_v1_add(params,
                                       native->fds[i],
                                       i,
                                       native->offsets[i],
                                       native->strides[i],
                                       DRM_FORMAT_MOD_INVALID >> 32,
                                       DRM_FORMAT_MOD_INVALID & 0xffffffff);
    }

    dmabuf_buffer->wl_buffer =
        zwp_linux_buffer_params_v1_create_immed(params,
                                                native->width,
                                                native->height,
                                                native->format,
                                                0);

    zwp_linux_buffer_params_v1_destroy(params);

    return dmabuf_buffer;

err:
    if (dmabuf_buffer)
        wayland_dmabuf_buffer_destroy(dmabuf_buffer);
    return NULL;
}

/**********************************************************************
 *          wayland_dmabuf_buffer_destroy
 *
 * Destroys a dmabuf buffer.
 */
void wayland_dmabuf_buffer_destroy(struct wayland_dmabuf_buffer *dmabuf_buffer)
{
    TRACE("%p\n", dmabuf_buffer);

    if (dmabuf_buffer->wl_buffer)
        wl_buffer_destroy(dmabuf_buffer->wl_buffer);

    free(dmabuf_buffer);
}

/**********************************************************************
 *          wayland_dmabuf_buffer_steal_wl_buffer_and_destroy
 *
 * Steal the wl_buffer from a dmabuf buffer and destroy the dmabuf buffer.
 */
struct wl_buffer *wayland_dmabuf_buffer_steal_wl_buffer_and_destroy(struct wayland_dmabuf_buffer *dmabuf_buffer)
{
    struct wl_buffer *wl_buffer;

    wl_buffer = dmabuf_buffer->wl_buffer;
    dmabuf_buffer->wl_buffer = NULL;

    wayland_dmabuf_buffer_destroy(dmabuf_buffer);

    return wl_buffer;
}
