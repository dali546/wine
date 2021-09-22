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

#include "shlobj.h"
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

static HGLOBAL import_uri_list(struct wayland_data_device_format *format,
                               const void *data, size_t data_size)
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

static void export_uri_list(struct wayland_data_device_format *format, int fd)
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
    {"text/uri-list", CF_HDROP, NULL, import_uri_list, export_uri_list, 0},
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
