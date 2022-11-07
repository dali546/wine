/*
 * Wayland data device (clipboard and DnD) handling
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

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

WINE_DEFAULT_DEBUG_CHANNEL(clipboard);

#define WINEWAYLAND_TAG_MIME_TYPE "application/x.winewayland.tag"

struct wayland_data_offer
{
    struct wayland *wayland;
    struct wl_data_offer *wl_data_offer;
    struct wl_array types;
    uint32_t source_actions;
    uint32_t action;
};

/* Normalize the mime type by skipping inconsequential characters, such as
 * spaces and double quotes, and converting to lower case. */
static char *normalize_mime_type(const char *mime)
{
    char *new_mime;
    const char *cur_read;
    char *cur_write;
    size_t new_mime_len = 0;

    cur_read = mime;
    for (; *cur_read != '\0'; cur_read++)
    {
        if (*cur_read != ' ' && *cur_read != '"')
            new_mime_len++;
    }

    new_mime = malloc(new_mime_len + 1);
    if (!new_mime) return NULL;
    cur_read = mime;
    cur_write = new_mime;

    for (; *cur_read != '\0'; cur_read++)
    {
        if (*cur_read != ' ' && *cur_read != '"')
            *cur_write++ = tolower(*cur_read);
    }

    *cur_write = '\0';

    return new_mime;
}

/**********************************************************************
 *          wl_data_offer handling
 */

static void data_offer_offer(void *data, struct wl_data_offer *wl_data_offer,
                             const char *type)
{
    struct wayland_data_offer *data_offer = data;
    char **p;

    p = wl_array_add(&data_offer->types, sizeof *p);
    *p = normalize_mime_type(type);
}

static void data_offer_source_actions(void *data,
                                      struct wl_data_offer *wl_data_offer,
                                      uint32_t source_actions)
{
    struct wayland_data_offer *data_offer = data;

    data_offer->source_actions = source_actions;
}

static void data_offer_action(void *data, struct wl_data_offer *wl_data_offer,
                              uint32_t dnd_action)
{
    struct wayland_data_offer *data_offer = data;

    data_offer->action = dnd_action;
}

static const struct wl_data_offer_listener data_offer_listener = {
    data_offer_offer,
    data_offer_source_actions,
    data_offer_action
};

static void wayland_data_offer_create(struct wayland *wayland,
                                      struct wl_data_offer *wl_data_offer)
{
    struct wayland_data_offer *data_offer;

    data_offer = calloc(1, sizeof(*data_offer));
    if (!data_offer)
    {
        ERR("Failed to allocate memory for data offer\n");
        return;
    }

    data_offer->wayland = wayland;
    data_offer->wl_data_offer = wl_data_offer;
    wl_array_init(&data_offer->types);
    wl_data_offer_add_listener(data_offer->wl_data_offer,
                               &data_offer_listener, data_offer);
}

static void wayland_data_offer_destroy(struct wayland_data_offer *data_offer)
{
    char **p;

    wl_data_offer_destroy(data_offer->wl_data_offer);
    wl_array_for_each(p, &data_offer->types)
        free(*p);
    wl_array_release(&data_offer->types);

    free(data_offer);
}

/**********************************************************************
 *          wl_data_device handling
 */

static void wayland_data_device_destroy_clipboard_data_offer(struct wayland_data_device *data_device)
{
    if (data_device->clipboard_wl_data_offer)
    {
        struct wayland_data_offer *data_offer =
            wl_data_offer_get_user_data(data_device->clipboard_wl_data_offer);
        wayland_data_offer_destroy(data_offer);
        data_device->clipboard_wl_data_offer = NULL;
    }
}

static void wayland_data_device_destroy_dnd_data_offer(struct wayland_data_device *data_device)
{
    if (data_device->dnd_wl_data_offer)
    {
        struct wayland_data_offer *data_offer =
            wl_data_offer_get_user_data(data_device->dnd_wl_data_offer);
        wayland_data_offer_destroy(data_offer);
        data_device->dnd_wl_data_offer = NULL;
    }
}

static void data_device_data_offer(void *data,
                                   struct wl_data_device *wl_data_device,
                                   struct wl_data_offer *wl_data_offer)
{
    struct wayland_data_device *data_device = data;

    wayland_data_offer_create(data_device->wayland, wl_data_offer);
}

static void data_device_enter(void *data, struct wl_data_device *wl_data_device,
                              uint32_t serial, struct wl_surface *wl_surface,
                              wl_fixed_t x_w, wl_fixed_t y_w,
                              struct wl_data_offer *wl_data_offer)
{
    struct wayland_data_device *data_device = data;

    /* Any previous dnd offer should have been freed by a drop or leave event. */
    assert(data_device->dnd_wl_data_offer == NULL);

    data_device->dnd_wl_data_offer = wl_data_offer;
}

static void data_device_leave(void *data, struct wl_data_device *wl_data_device)
{
    struct wayland_data_device *data_device = data;

    wayland_data_device_destroy_dnd_data_offer(data_device);
}

static void data_device_motion(void *data, struct wl_data_device *wl_data_device,
                               uint32_t time, wl_fixed_t x_w, wl_fixed_t y_w)
{
}

static void data_device_drop(void *data, struct wl_data_device *wl_data_device)
{
    struct wayland_data_device *data_device = data;

    wayland_data_device_destroy_dnd_data_offer(data_device);
}

static void data_device_selection(void *data,
                                  struct wl_data_device *wl_data_device,
                                  struct wl_data_offer *wl_data_offer)
{
    struct wayland_data_device *data_device = data;

    /* Destroy any previous data offer. */
    wayland_data_device_destroy_clipboard_data_offer(data_device);

    data_device->clipboard_wl_data_offer = wl_data_offer;
}

static const struct wl_data_device_listener data_device_listener = {
    data_device_data_offer,
    data_device_enter,
    data_device_leave,
    data_device_motion,
    data_device_drop,
    data_device_selection
};

/**********************************************************************
 *          wayland_data_device_init
 *
 * Initializes the data_device extension in order to support clipboard
 * operations.
 */
void wayland_data_device_init(struct wayland_data_device *data_device,
                              struct wayland *wayland)
{
    data_device->wayland = wayland;
    data_device->wl_data_device =
        wl_data_device_manager_get_data_device(wayland->wl_data_device_manager,
                                               wayland->wl_seat);

    wl_data_device_add_listener(data_device->wl_data_device, &data_device_listener,
                                data_device);
}

/**********************************************************************
 *          wayland_data_device_deinit
 */
void wayland_data_device_deinit(struct wayland_data_device *data_device)
{
    if (data_device->wl_data_device)
        wl_data_device_destroy(data_device->wl_data_device);

    memset(data_device, 0, sizeof(*data_device));
}

/**********************************************************************
 *          wl_data_source handling
 */

static void wayland_data_source_export(struct wayland_data_device_format *format, int32_t fd)
{
    struct get_clipboard_params params = { .data_only = TRUE, .data_size = 0 };
    static const size_t buffer_size = 1024;

    if (!(params.data = malloc(buffer_size))) return;

    if (!NtUserOpenClipboard(thread_wayland()->clipboard_hwnd, 0))
    {
        TRACE("failed to open clipboard for export\n");
        goto out;
    }

    params.size = buffer_size;
    if (NtUserGetClipboardData(format->clipboard_format, &params))
    {
        format->export(format, fd, params.data, params.size);
    }
    else if (params.data_size)
    {
        /* If 'buffer_size' is too small, NtUserGetClipboardData writes the
         * minimum size in 'params.data_size', so we retry with that. */
        free(params.data);
        params.data = malloc(params.data_size);
        if (params.data)
        {
            params.size = params.data_size;
            if (NtUserGetClipboardData(format->clipboard_format, &params))
                format->export(format, fd, params.data, params.size);
        }
    }

    NtUserCloseClipboard();

out:
    free(params.data);
}

static void data_source_target(void *data, struct wl_data_source *source,
                               const char *mime_type)
{
}

static void data_source_send(void *data, struct wl_data_source *source,
                             const char *mime_type, int32_t fd)
{
    struct wayland_data_device_format *format =
        wayland_data_device_format_for_mime_type(mime_type);

    TRACE("source=%p mime_type=%s\n", source, mime_type);

    if (format) wayland_data_source_export(format, fd);

    close(fd);
}

static void data_source_cancelled(void *data, struct wl_data_source *source)
{
    TRACE("source=%p\n", source);
    wl_data_source_destroy(source);
}

static void data_source_dnd_drop_performed(void *data,
                                           struct wl_data_source *source)
{
}

static void data_source_dnd_finished(void *data, struct wl_data_source *source)
{
}

static void data_source_action(void *data, struct wl_data_source *source,
                               uint32_t dnd_action)
{
}

static const struct wl_data_source_listener data_source_listener = {
    data_source_target,
    data_source_send,
    data_source_cancelled,
    data_source_dnd_drop_performed,
    data_source_dnd_finished,
    data_source_action,
};

/**********************************************************************
 *          clipboard window handling
 */

static void clipboard_update(void)
{
    struct wayland *wayland = thread_wayland();
    uint32_t enter_serial;
    struct wl_data_source *source;
    UINT clipboard_format = 0;

    TRACE("WM_CLIPBOARDUPDATE wayland %p enter_serial=%d/%d\n",
          wayland,
          wayland ? wayland->keyboard.enter_serial : -1,
          wayland ? wayland->pointer.enter_serial : -1);

    if (!wayland)
        return;

    enter_serial = wayland->keyboard.enter_serial ? wayland->keyboard.enter_serial
                                                  : wayland->pointer.enter_serial;

    if (!enter_serial)
        return;

    if (!NtUserOpenClipboard(wayland->clipboard_hwnd, 0))
    {
        TRACE("failed to open clipboard\n");
        return;
    }

    source = wl_data_device_manager_create_data_source(wayland->wl_data_device_manager);

    while ((clipboard_format = NtUserEnumClipboardFormats(clipboard_format)))
    {
        struct wayland_data_device_format *format =
            wayland_data_device_format_for_clipboard_format(clipboard_format, NULL);
        if (format)
        {
            TRACE("Offering source=%p mime=%s\n", source, format->mime_type);
            wl_data_source_offer(source, format->mime_type);
        }
    }

    /* Add a special entry so that we can detect when an offer is coming from us. */
    wl_data_source_offer(source, WINEWAYLAND_TAG_MIME_TYPE);

    wl_data_source_add_listener(source, &data_source_listener, NULL);
    wl_data_device_set_selection(wayland->data_device.wl_data_device, source,
                                 enter_serial);

    NtUserCloseClipboard();
}

/**********************************************************************
 *          waylanddrv_unix_clipboard_message
 */
NTSTATUS waylanddrv_unix_clipboard_message(void *arg)
{
    struct waylanddrv_unix_clipboard_message_params *params = arg;

    switch (params->msg)
    {
    case WM_NCCREATE:
        return TRUE;
    case WM_CLIPBOARDUPDATE:
        TRACE("WM_CLIPBOARDUPDATE\n");
        /* Ignore our own updates */
        if (NtUserGetClipboardOwner() != params->hwnd) clipboard_update();
        break;
    }

    return NtUserMessageCall(params->hwnd, params->msg, params->wparam,
                             params->lparam, NULL, NtUserDefWindowProc, FALSE);
}

/**********************************************************************
 *          wayland_data_device_ensure_clipboard_window
 *
 * Creates (if not already created) the window which handles clipboard
 * messages for the specified wayland instance.
 */
void wayland_data_device_ensure_clipboard_window(struct wayland *wayland)
{
    if (!wayland->clipboard_hwnd)
    {
        wayland->clipboard_hwnd =
            ULongToHandle(WAYLANDDRV_CLIENT_CALL(create_clipboard_window, NULL, 0));
    }
}
