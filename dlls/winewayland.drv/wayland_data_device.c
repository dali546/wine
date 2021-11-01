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

#include "config.h"
#include "wine/port.h"

#include "waylanddrv.h"

#include "wine/heap.h"
#include "wine/debug.h"

#include "winuser.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>

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

    new_mime = heap_alloc(new_mime_len + 1);
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

    data_offer = heap_alloc_zero(sizeof(*data_offer));
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
        heap_free(*p);
    wl_array_release(&data_offer->types);

    heap_free(data_offer);
}

static void *wayland_data_offer_receive_data(struct wayland_data_offer *data_offer,
                                             const char *mime_type,
                                             size_t *size_out)
{
    int data_pipe[2] = {-1, -1};
    size_t buffer_size = 4096;
    int total = 0;
    unsigned char *buffer;
    int nread;

    buffer = heap_alloc(buffer_size);
    if (buffer == NULL)
        goto out;

    if (pipe2(data_pipe, O_CLOEXEC) == -1)
        goto out;

    wl_data_offer_receive(data_offer->wl_data_offer, mime_type, data_pipe[1]);
    close(data_pipe[1]);

    /* Flush to ensure our receive request reaches the server. */
    wl_display_flush(data_offer->wayland->wl_display);

    do
    {
        struct pollfd pfd = { .fd = data_pipe[0], .events = POLLIN };
        int ret;

        /* Wait a limited amount of time for the data to arrive, since otherwise
         * a misbehaving data source could block us indefinitely. */
        while ((ret = poll(&pfd, 1, 3000)) == -1 && errno == EINTR) continue;
        if (ret <= 0 || !(pfd.revents & (POLLIN | POLLHUP)))
        {
            TRACE("failed polling data offer pipe ret=%d errno=%d revents=0x%x\n",
                  ret, ret == -1 ? errno : 0, pfd.revents);
            total = 0;
            goto out;
        }

        nread = read(data_pipe[0], buffer + total, buffer_size - total);
        if (nread == -1 && errno != EINTR)
        {
            ERR("failed to read data offer pipe\n");
            total = 0;
            goto out;
        }
        else if (nread > 0)
        {
            total += nread;
            if (total == buffer_size)
            {
                buffer_size += 4096;
                buffer = heap_realloc(buffer, buffer_size);
            }
        }
    } while (nread > 0);

    TRACE("received %d bytes\n", total);

out:
    if (data_pipe[0] >= 0)
        close(data_pipe[0]);

    if (total == 0 && buffer != NULL)
    {
        heap_free(buffer);
        buffer = NULL;
    }

    *size_out = total;

    return buffer;
}

static HGLOBAL wayland_data_offer_import_format(struct wayland_data_offer *data_offer,
                                                struct wayland_data_device_format *format)
{
    size_t data_size;
    void *data;
    HGLOBAL mem_handle;

    data = wayland_data_offer_receive_data(data_offer, format->mime_type, &data_size);
    if (!data)
        return NULL;

    mem_handle = format->import(format, data, data_size);

    heap_free(data);

    return mem_handle;
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
    struct wayland *wayland = thread_wayland();
    struct wayland_data_offer *data_offer;
    char **p;

    TRACE("wl_data_offer=%u\n",
          wl_data_offer ? wl_proxy_get_id((struct wl_proxy*)wl_data_offer) : 0);

    /* We may get a selection event before we have had a chance to create the
     * clipboard window after thread init (see wayland_init_thread_data), so
     * we need to ensure we have a valid window here. */
    wayland_data_device_ensure_clipboard_window(wayland);

    /* Destroy any previous data offer. */
    wayland_data_device_destroy_clipboard_data_offer(data_device);

    /* If we didn't get an offer and we are the clipboard owner, empty the
     * clipboard. Otherwise ignore the empty offer completely. */
    if (!wl_data_offer)
    {
        if (GetClipboardOwner() == wayland->clipboard_hwnd)
        {
            OpenClipboard(NULL);
            EmptyClipboard();
            CloseClipboard();
        }
        return;
    }

    data_offer = wl_data_offer_get_user_data(wl_data_offer);

    /* If this offer contains the special winewayland tag mime-type, it was sent
     * from us to notify external wayland clients about a wine clipboard update.
     * The clipboard already contains all the required data, plus we need to ignore
     * this in order to avoid an endless notification loop. */
    wl_array_for_each(p, &data_offer->types)
    {
        if (!strcmp(*p, WINEWAYLAND_TAG_MIME_TYPE))
        {
            TRACE("ignoring offer produced by winewayland\n");
            goto ignore_selection;
        }
    }

    if (!OpenClipboard(data_offer->wayland->clipboard_hwnd))
    {
        WARN("failed to open clipboard for selection\n");
        goto ignore_selection;
    }

    EmptyClipboard();

    /* For each mime type, mark that we have available clipboard data. */
    wl_array_for_each(p, &data_offer->types)
    {
        struct wayland_data_device_format *format =
            wayland_data_device_format_for_mime_type(*p);
        if (format)
        {
            TRACE("Avalaible clipboard format for %s => %u\n", *p, format->clipboard_format);
            SetClipboardData(format->clipboard_format, 0);
        }
    }

    CloseClipboard();

    data_device->clipboard_wl_data_offer = wl_data_offer;

    return;

ignore_selection:
    wayland_data_offer_destroy(data_offer);
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

    if (format) format->export(format, fd);

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

    if (!OpenClipboard(wayland->clipboard_hwnd))
    {
        TRACE("failed to open clipboard\n");
        return;
    }

    source = wl_data_device_manager_create_data_source(wayland->wl_data_device_manager);

    while ((clipboard_format = EnumClipboardFormats(clipboard_format)))
    {
        struct wayland_data_device_format *format =
            wayland_data_device_format_for_clipboard_format(clipboard_format);
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

    CloseClipboard();
}

static void clipboard_render_format(UINT clipboard_format)
{
    struct wayland_data_device *data_device;
    struct wayland_data_offer *data_offer;
    char **p;

    data_device = wl_data_device_get_user_data(thread_wayland()->data_device.wl_data_device);
    if (!data_device->clipboard_wl_data_offer)
        return;

    data_offer = wl_data_offer_get_user_data(data_device->clipboard_wl_data_offer);
    if (!data_offer)
        return;

    wl_array_for_each(p, &data_offer->types)
    {
        struct wayland_data_device_format *format =
            wayland_data_device_format_for_mime_type(*p);
        if (format && format->clipboard_format == clipboard_format)
        {
            HGLOBAL mem_handle = wayland_data_offer_import_format(data_offer, format);
            SetClipboardData(format->clipboard_format, mem_handle);
            break;
        }
    }
}

static void clipboard_destroy(void)
{
    struct wayland *wayland = thread_wayland();
    struct wayland_data_device *data_device =
        wl_data_device_get_user_data(wayland->data_device.wl_data_device);
    wayland_data_device_destroy_clipboard_data_offer(data_device);
}

static LRESULT CALLBACK clipboard_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_NCCREATE:
        return TRUE;
    case WM_CLIPBOARDUPDATE:
        TRACE("WM_CLIPBOARDUPDATE\n");
        /* Ignore our own updates */
        if (GetClipboardOwner() != hwnd) clipboard_update();
        break;
    case WM_RENDERFORMAT:
        TRACE("WM_RENDERFORMAT: %ld\n", wp);
        clipboard_render_format(wp);
        break;
    case WM_DESTROYCLIPBOARD:
        TRACE("WM_DESTROYCLIPBOARD: lost ownership clipboard_hwnd=%p\n", hwnd);
        clipboard_destroy();
        break;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static HWND wayland_data_device_create_clipboard_window(void)
{
    static const WCHAR clipboard_classname[] = {
        '_','_','w','i','n','e','_','c','l','i','p','b','o','a','r','d',
        '_','m','a','n','a','g','e','r',0
    };
    WNDCLASSW class;
    HWND clipboard_hwnd;

    memset(&class, 0, sizeof(class));
    class.lpfnWndProc = clipboard_wndproc;
    class.lpszClassName = clipboard_classname;

    if (!RegisterClassW(&class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        ERR("could not register clipboard window class err %u\n", GetLastError());
        return 0;
    }

    if (!(clipboard_hwnd = CreateWindowW(clipboard_classname, NULL, 0, 0, 0, 0, 0,
                                         HWND_MESSAGE, 0, 0, NULL)))
    {
        ERR("failed to create clipboard window err %u\n", GetLastError());
        return 0;
    }

    wayland_data_device_init_formats();

    if (!AddClipboardFormatListener(clipboard_hwnd))
        ERR("failed to set clipboard listener %u\n", GetLastError());

    TRACE("clipboard_hwnd=%p\n", clipboard_hwnd);
    return clipboard_hwnd;
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
        wayland->clipboard_hwnd = wayland_data_device_create_clipboard_window();
}
