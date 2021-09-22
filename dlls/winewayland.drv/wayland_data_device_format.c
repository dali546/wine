/*
 * Wayland data device format handling
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
#include <unistd.h>

WINE_DEFAULT_DEBUG_CHANNEL(clipboard);

static void write_all(int fd, const void *buf, size_t count)
{
    size_t nwritten = 0;

    while (nwritten < count)
    {
        ssize_t ret = write(fd, (const char*)buf + nwritten, count - nwritten);
        if (ret == -1 && errno != EINTR)
        {
            WARN("Failed to write all data, had %zu bytes, wrote %zu bytes (errno: %d)\n",
                 count, nwritten, errno);
            break;
        }
        else if (ret > 0)
        {
            nwritten += ret;
        }
    }
}

static HGLOBAL import_text_as_unicode(struct wayland_data_device_format *format,
                                      const void *data, size_t data_size)
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

static void export_text(struct wayland_data_device_format *format, int fd)
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
    write_all(fd, bytes, byte_count);

    heap_free(bytes);

    GlobalUnlock(mem_handle);

    CloseClipboard();
}

static HGLOBAL import_data(struct wayland_data_device_format *format,
                           const void *data, size_t data_size)
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

static void export_data(struct wayland_data_device_format *format, int fd)
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

    write_all(fd, mem, GlobalSize(mem_handle));

    GlobalUnlock(mem_handle);

    CloseClipboard();
}

#define CP_ASCII 20127

/* Order is important. When selecting a mime-type for a clipboard format we
 * will choose the first entry that matches the specified clipboard format. */
static struct wayland_data_device_format supported_formats[] =
{
    {"text/plain;charset=utf-8", CF_UNICODETEXT, NULL, import_text_as_unicode, export_text, CP_UTF8},
    {"text/plain;charset=us-ascii", CF_UNICODETEXT, NULL, import_text_as_unicode, export_text, CP_ASCII},
    {"text/plain", CF_UNICODETEXT, NULL, import_text_as_unicode, export_text, CP_ASCII},
    {"text/rtf", 0, "Rich Text Format", import_data, export_data, 0},
    {"text/richtext", 0, "Rich Text Format", import_data, export_data, 0},
    {NULL, 0, NULL, 0},
};

void wayland_data_device_init_formats(void)
{
    struct wayland_data_device_format *format = supported_formats;

    while (format->mime_type)
    {
        if (format->clipboard_format == 0)
            format->clipboard_format = RegisterClipboardFormatA(format->register_name);
        format++;
    }
}

struct wayland_data_device_format *wayland_data_device_format_for_mime_type(const char *mime)
{
    struct wayland_data_device_format *format = supported_formats;

    while (format->mime_type)
    {
        if (!strcmp(mime, format->mime_type))
            return format;
        format++;
    }

    return NULL;
}

struct wayland_data_device_format *wayland_data_device_format_for_clipboard_format(UINT clipboard_format)
{
    struct wayland_data_device_format *format = supported_formats;

    while (format->mime_type)
    {
        if (format->clipboard_format == clipboard_format)
            return format;
        format++;
    }

    return NULL;
}
