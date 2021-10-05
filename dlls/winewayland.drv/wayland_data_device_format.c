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

/* Order is important. When selecting a mime-type for a clipboard format we
 * will choose the first entry that matches the specified clipboard format. */
static struct wayland_data_device_format supported_formats[] =
{
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
