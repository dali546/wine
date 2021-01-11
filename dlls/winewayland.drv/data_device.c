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

#include "shlobj.h"
#include "winuser.h"
#include "winnls.h"
#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/unicode.h"

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

WINE_DEFAULT_DEBUG_CHANNEL(clipboard);

#define WINEWAYLAND_TAG_MIME_TYPE "application/x.winewayland.tag"

struct data_device
{
    struct wayland *wayland;
    struct wl_data_offer *clipboard_wl_data_offer;
};

struct data_offer
{
    struct wayland *wayland;
    struct wl_data_offer *wl_data_offer;
    struct wl_array types;
};

struct format
{
    const char *mime_type;
    UINT clipboard_format;
    const char *register_name;
    HGLOBAL (*import)(struct format *format, const void *data, size_t data_size);
    void (*export)(struct format *format, int fd);
    UINT_PTR extra;
};

static HGLOBAL import_text_as_unicode(struct format *format, const void *data, size_t data_size)
{
    int wide_count;
    HGLOBAL mem_handle;
    void *mem;

    wide_count = MultiByteToWideChar(format->extra, 0, data, data_size, NULL, 0);
    mem_handle = GlobalAlloc(GMEM_MOVEABLE, wide_count * sizeof(WCHAR) + 1);
    if (!mem_handle || !(mem = GlobalLock(mem_handle)))
    {
        if (mem_handle) GlobalFree(mem_handle);
        return NULL;
    }

    MultiByteToWideChar(CP_UTF8, 0, data, data_size, mem, wide_count);
    ((unsigned char*)mem)[wide_count * sizeof(WCHAR)] = 0;
    GlobalUnlock(mem_handle);

    return mem_handle;
}

static void export_text(struct format *format, int fd)
{
    HGLOBAL mem_handle;
    void *mem;
    int byte_count;
    char *bytes;

    if (!OpenClipboard(thread_wayland()->clipboard_hwnd))
    {
        WARN("failed to open clipboard for export\n");
        return;
    }

    mem_handle = GetClipboardData(format->clipboard_format);
    mem = GlobalLock(mem_handle);

    byte_count = WideCharToMultiByte(format->extra, 0, mem, -1, NULL, 0, NULL, NULL);
    bytes = heap_alloc(byte_count);
    WideCharToMultiByte(format->extra, 0, mem, -1, bytes, byte_count, NULL, NULL);
    write(fd, bytes, byte_count);
    heap_free(bytes);

    GlobalUnlock(mem_handle);

    CloseClipboard();
}

static HGLOBAL import_data(struct format *format, const void *data, size_t data_size)
{
    HGLOBAL mem_handle;
    void *mem;

    mem_handle = GlobalAlloc(GMEM_MOVEABLE, data_size);
    if (!mem_handle || !(mem = GlobalLock(mem_handle)))
    {
        if (mem_handle) GlobalFree(mem_handle);
        return NULL;
    }

    memcpy(mem, data, data_size);
    GlobalUnlock(mem_handle);

    return mem_handle;
}

static void export_data(struct format *format, int fd)
{
    HGLOBAL mem_handle;
    void *mem;

    if (!OpenClipboard(thread_wayland()->clipboard_hwnd))
    {
        TRACE("failed to open clipboard for export\n");
        return;
    }

    mem_handle = GetClipboardData(format->clipboard_format);
    mem = GlobalLock(mem_handle);

    write(fd, mem, GlobalSize(mem_handle));

    GlobalUnlock(mem_handle);

    CloseClipboard();
}

/* Adapted from winex11.drv/clipboard.c */
static char *decode_uri(const char *uri, size_t uri_length)
{
    char *decoded = heap_alloc_zero(uri_length + 1);
    size_t uri_i = 0;
    size_t decoded_i = 0;

    if (decoded == NULL)
        goto err;

    while (uri_i < uri_length)
    {
        if (uri[uri_i] == '%')
        {
            unsigned long number;
            char buffer[3];

            if (uri_i + 1 == uri_length || uri_i + 2 == uri_length)
                goto err;

            buffer[0] = uri[uri_i + 1];
            buffer[1] = uri[uri_i + 2];
            buffer[2] = '\0';
            errno = 0;
            number = strtoul(buffer, NULL, 16);
            if (errno != 0)
                goto err;
            decoded[decoded_i] = number;

            uri_i += 3;
            decoded_i++;
        }
        else
        {
            decoded[decoded_i++] = uri[uri_i++];
        }
    }

    decoded[decoded_i] = '\0';

    return decoded;

err:
    if (decoded)
        heap_free(decoded);
    return NULL;
}

/* Adapted from winex11.drv/clipboard.c */
static WCHAR* decoded_uri_to_dos(const char *uri)
{
    WCHAR *ret = NULL;

    if (strncmp(uri, "file:/", 6))
        return NULL;

    if (uri[6] == '/')
    {
        if (uri[7] == '/')
        {
            /* file:///path/to/file (nautilus, thunar) */
            ret = wine_get_dos_file_name(&uri[7]);
        }
        else if (uri[7])
        {
            /* file://hostname/path/to/file (X file drag spec) */
            char hostname[256];
            char *path = strchr(&uri[7], '/');
            if (path)
            {
                *path = '\0';
                if (strcmp(&uri[7], "localhost") == 0)
                {
                    *path = '/';
                    ret = wine_get_dos_file_name(path);
                }
                else if (gethostname(hostname, sizeof(hostname)) == 0)
                {
                    if (strcmp(hostname, &uri[7]) == 0)
                    {
                        *path = '/';
                        ret = wine_get_dos_file_name(path);
                    }
                }
            }
        }
    }
    else if (uri[6])
    {
        /* file:/path/to/file (konqueror) */
        ret = wine_get_dos_file_name(&uri[5]);
    }

    return ret;
}

static HGLOBAL import_uri_list(struct format *format, const void *data, size_t data_size)
{
    HGLOBAL mem_handle = 0;
    DROPFILES *drop_files;
    const char *data_end = (const char *) data + data_size;
    const char *line_start = data;
    const char *line_end;
    WCHAR **path;
    struct wl_array paths;
    size_t total_chars = 0;
    WCHAR *dst;

    TRACE("data=%p size=%lu\n", data, (unsigned long)data_size);

    wl_array_init(&paths);

    while (line_start < data_end)
    {
        line_end = strchr(line_start, '\r');
        if (line_end == NULL || line_end == data_end - 1 || line_end[1] != '\n')
        {
            WARN("URI list line doesn't end in \\r\\n\n");
            break;
        }

        if (line_start[0] != '#')
        {
            char *decoded_uri = decode_uri(line_start, line_end - line_start);
            TRACE("decoded_uri=%s\n", decoded_uri);
            path = wl_array_add(&paths, sizeof *path);
            if (!path)
                goto out;
            *path = decoded_uri_to_dos(decoded_uri);
            total_chars += strlenW(*path) + 1;
            heap_free(decoded_uri);
        }

        line_start = line_end + 2;
    }

    /* DROPFILES points to an array of consecutive null terminated WCHAR strings,
     * followed by a final 0 WCHAR to denote the end of the array. We place that
     * array just after the DROPFILE struct itself. */
    mem_handle = GlobalAlloc(GMEM_MOVEABLE, sizeof(DROPFILES) + (total_chars + 1) * sizeof(WCHAR));
    if (!mem_handle || !(drop_files = GlobalLock(mem_handle)))
    {
        if (mem_handle)
        {
            GlobalFree(mem_handle);
            mem_handle = NULL;
        }
        goto out;
    }

    drop_files->pFiles = sizeof(*drop_files);
    drop_files->pt.x = 0;
    drop_files->pt.y = 0;
    drop_files->fNC = FALSE;
    drop_files->fWide = TRUE;

    dst = (WCHAR*)(drop_files + 1);
    wl_array_for_each(path, &paths)
    {
        strcpyW(dst, *path);
        dst += strlenW(*path) + 1;
    }
    *dst = 0;

    GlobalUnlock(mem_handle);

out:
    wl_array_for_each(path, &paths)
        heap_free(*path);

    wl_array_release(&paths);

    return mem_handle;
}

static void export_uri_list(struct format *format, int fd)
{
    HGLOBAL mem_handle;
    void *mem;

    if (!OpenClipboard(thread_wayland()->clipboard_hwnd))
    {
        TRACE("failed to open clipboard for export\n");
        return;
    }

    mem_handle = GetClipboardData(format->clipboard_format);
    mem = GlobalLock(mem_handle);

    write(fd, mem, GlobalSize(mem_handle));

    GlobalUnlock(mem_handle);

    CloseClipboard();
}

#define CP_ASCII 20127

/* Order is important. When selecting a mime-type for a clipboard format we
 * will choose the first entry that matches the specified clipboard format. */
struct format supported_formats[] = {
    {"text/plain;charset=utf-8", CF_UNICODETEXT, NULL, import_text_as_unicode, export_text, CP_UTF8},
    {"text/plain;charset=us-ascii", CF_UNICODETEXT, NULL, import_text_as_unicode, export_text, CP_ASCII},
    {"text/plain", CF_UNICODETEXT, NULL, import_text_as_unicode, export_text, CP_ASCII},
    {"text/rtf", 0, "Rich Text Format", import_data, export_data, 0},
    {"text/richtext", 0, "Rich Text Format", import_data, export_data, 0},
    {"text/uri-list", CF_HDROP, NULL, import_uri_list, export_uri_list, 0},
    {"image/tiff", CF_TIFF, 0, import_data, export_data, 0},
    {"image/png", 0, "PNG", import_data, export_data, 0},
    {"image/jpeg", 0, "JFIF", import_data, export_data, 0},
    {"image/gif", 0, "GIF", import_data, export_data, 0},
    {NULL, 0, NULL, 0},
};

struct format *registered_formats = NULL;

static void init_supported_formats(void)
{
    struct format *format = supported_formats;

    while (format->mime_type)
    {
        if (format->clipboard_format == 0)
            format->clipboard_format = RegisterClipboardFormatA(format->register_name);
        format++;
    }
}

static struct format *format_for_mime_type(const char *mime)
{
    struct format *format = supported_formats;

    while (format->mime_type)
    {
        if (!strcmp(mime, format->mime_type))
            return format;
        format++;
    }

    return NULL;
}

static struct format *format_for_clipboard_format(UINT clipboard_format)
{
    struct format *format = supported_formats;

    while (format->mime_type)
    {
        if (format->clipboard_format == clipboard_format)
            return format;
        format++;
    }

    return NULL;
}

static char *normalize_mime_type(const char *mime)
{
    char *new_mime;
    const char *cur_read;
    char *cur_write;
    int remove_count = 0;

    cur_read = mime;
    while (*cur_read != 0)
    {
        if (*cur_read == ' ' || *cur_read == '"')
            remove_count++;
        cur_read++;
    }

    new_mime = heap_alloc((cur_read - mime) - remove_count + 1);
    cur_read = mime;
    cur_write = new_mime;

    do
    {
        if (*cur_read != ' ' && *cur_read != '"')
            *cur_write++ = tolower(*cur_read);
    } while (*cur_read++);

    return new_mime;
}

static void data_offer_offer(void *data, struct wl_data_offer *wl_data_offer,
                             const char *type)
{
    struct data_offer *offer = data;
    char **p;

    p = wl_array_add(&offer->types, sizeof *p);
    *p = normalize_mime_type(type);
}

static void data_offer_source_actions(void *data,
                                      struct wl_data_offer *wl_data_offer,
                                      uint32_t source_actions)
{
}

static void data_offer_action(void *data, struct wl_data_offer *wl_data_offer,
                              uint32_t dnd_action)
{
}

static const struct wl_data_offer_listener data_offer_listener = {
    data_offer_offer,
    data_offer_source_actions,
    data_offer_action
};

static void data_offer_create(struct wayland *wayland, struct wl_data_offer *wl_data_offer)
{
    struct data_offer *data_offer;

    data_offer = heap_alloc_zero(sizeof(*data_offer));

    data_offer->wayland = wayland;
    data_offer->wl_data_offer = wl_data_offer;
    wl_array_init(&data_offer->types);
    wl_data_offer_add_listener(data_offer->wl_data_offer,
                               &data_offer_listener, data_offer);
}

static void data_offer_destroy(struct data_offer *offer)
{
    char **p;

    wl_data_offer_destroy(offer->wl_data_offer);
    wl_array_for_each(p, &offer->types)
        heap_free(*p);
    wl_array_release(&offer->types);

    heap_free(offer);
}

static void *data_offer_receive_data(struct data_offer *data_offer,
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

static HGLOBAL data_offer_import_format(struct data_offer *data_offer, struct format *format)
{
    size_t data_size;
    void *data;
    HGLOBAL mem_handle;

    data = data_offer_receive_data(data_offer, format->mime_type, &data_size);
    if (!data)
        return NULL;

    mem_handle = format->import(format, data, data_size);

    heap_free(data);

    return mem_handle;
}

static void data_device_destroy_clipboard_data_offer(struct data_device *data_device)
{
    if (data_device->clipboard_wl_data_offer)
    {
        struct data_offer *data_offer =
            wl_data_offer_get_user_data(data_device->clipboard_wl_data_offer);
        data_offer_destroy(data_offer);
        data_device->clipboard_wl_data_offer = NULL;
    }
}

static void data_device_data_offer(void *data,
                                   struct wl_data_device *wl_data_device,
                                   struct wl_data_offer *wl_data_offer)
{
    struct data_device *data_device = data;

    TRACE("wl_data_offer@%u\n", wl_proxy_get_id((struct wl_proxy*)wl_data_offer));

    data_offer_create(data_device->wayland, wl_data_offer);
}

static void data_device_enter(void *data, struct wl_data_device *wl_data_device,
                              uint32_t serial, struct wl_surface *wl_surface,
                              wl_fixed_t x_w, wl_fixed_t y_w,
                              struct wl_data_offer *wl_data_offer)
{
}

static void data_device_leave(void *data, struct wl_data_device *wl_data_device)
{
}

static void data_device_motion(void *data, struct wl_data_device *wl_data_device,
                               uint32_t time, wl_fixed_t x_w, wl_fixed_t y_w)
{
}

static void data_device_drop(void *data, struct wl_data_device *wl_data_device)
{
}

static void data_device_selection(void *data,
                                  struct wl_data_device *wl_data_device,
                                  struct wl_data_offer *wl_data_offer)
{
    struct data_device *data_device = data;
    struct wayland *wayland = thread_wayland();
    struct data_offer *data_offer;
    char **p;

    TRACE("wl_data_offer=%u\n",
          wl_data_offer ? wl_proxy_get_id((struct wl_proxy*)wl_data_offer) : 0);

    /* Destroy any previous data offer. */
    data_device_destroy_clipboard_data_offer(data_device);

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
        struct format *format = format_for_mime_type(*p);
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
    data_offer_destroy(data_offer);
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
void wayland_data_device_init(struct wayland *wayland)
{
    struct data_device *data_device;

    wayland->wl_data_device =
        wl_data_device_manager_get_data_device(wayland->wl_data_device_manager,
                                               wayland->wl_seat);
    if (!wayland->wl_data_device)
    {
        ERR("failed to get wl_data_device\n");
        return;
    }

    data_device = heap_alloc_zero(sizeof(*data_device));
    data_device->wayland = wayland;

    wl_data_device_add_listener(wayland->wl_data_device, &data_device_listener,
                                data_device);
}

static void clipboard_render_format(UINT clipboard_format)
{
    struct data_device *data_device;
    struct data_offer *data_offer;
    char **p;

    data_device = wl_data_device_get_user_data(thread_wayland()->wl_data_device);
    if (!data_device->clipboard_wl_data_offer)
        return;

    data_offer = wl_data_offer_get_user_data(data_device->clipboard_wl_data_offer);
    if (!data_offer)
        return;

    wl_array_for_each(p, &data_offer->types)
    {
        struct format *format = format_for_mime_type(*p);
        if (format && format->clipboard_format == clipboard_format)
        {
            HGLOBAL mem_handle = data_offer_import_format(data_offer, format);
            SetClipboardData(format->clipboard_format, mem_handle);
            break;
        }
    }
}

static void data_source_target(void *data, struct wl_data_source *source,
                               const char *mime_type)
{
}

static void data_source_send(void *data, struct wl_data_source *source,
                             const char *mime_type, int32_t fd)
{
    struct format *format = format_for_mime_type(mime_type);
    TRACE("source=%p mime_type=%s\n", source, mime_type);
    if (format)
        format->export(format, fd);
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

static void clipboard_update(void)
{
    struct wayland *wayland = thread_wayland();
    struct wl_data_source *source;
    UINT clipboard_format = 0;

    TRACE("WM_CLIPBOARDUPDATE wayland %p enter_serial=%d\n",
            wayland, wayland ? wayland->keyboard.enter_serial : -1);
    if (!wayland || !wayland->keyboard.enter_serial)
        return;

    if (!OpenClipboard(wayland->clipboard_hwnd))
    {
        TRACE("failed to open clipboard\n");
        return;
    }

    source = wl_data_device_manager_create_data_source(wayland->wl_data_device_manager);

    while ((clipboard_format = EnumClipboardFormats(clipboard_format)))
    {
        struct format *format = format_for_clipboard_format(clipboard_format);
        if (format)
        {
            TRACE("Offering source=%p mime=%s\n", source, format->mime_type);
            wl_data_source_offer(source, format->mime_type);
        }
    }

    /* Add a special entry so that we can detect when an offer is coming from us. */
    wl_data_source_offer(source, WINEWAYLAND_TAG_MIME_TYPE);

    wl_data_source_add_listener(source, &data_source_listener, NULL);
    wl_data_device_set_selection(wayland->wl_data_device, source,
                                 wayland->keyboard.enter_serial);

    CloseClipboard();
}

static void clipboard_destroy(void)
{
    struct wayland *wayland = thread_wayland();
    struct data_device *data_device =
        wl_data_device_get_user_data(wayland->wl_data_device);
    data_device_destroy_clipboard_data_offer(data_device);
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
        if (GetClipboardOwner() != hwnd)
            clipboard_update();
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

/**********************************************************************
 *          wayland_data_device_init_clipboard_window
 *
 * Initializes the window which handles clipboard messages.
 */
HWND wayland_data_device_create_clipboard_window(void)
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

    init_supported_formats();
    if (!AddClipboardFormatListener(clipboard_hwnd))
        ERR("failed to set clipboard listener %u\n", GetLastError());

    TRACE("clipboard_hwnd=%p\n", clipboard_hwnd);
    return clipboard_hwnd;
}
