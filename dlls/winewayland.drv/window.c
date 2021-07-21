/*
 * Window related functions
 *
 * Copyright 1993, 1994, 1995, 1996, 2001, 2013-2017 Alexandre Julliard
 * Copyright 1993 David Metcalfe
 * Copyright 1995, 1996 Alex Korobka
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

#define NONAMELESSUNION
#define NONAMELESSSTRUCT

#include "config.h"

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "wine/unicode.h"

#include "waylanddrv.h"
#include "wine/gdi_driver.h"
#include "wine/server.h"
#include "wine/debug.h"
#include "wine/heap.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

/* Change to 1 to dump flushed surface buffer contents to disk */
#define DEBUG_DUMP_FLUSH_SURFACE_BUFFER 0

/* private window data */
struct wayland_win_data
{
    /* hwnd that this private data belongs to */
    HWND           hwnd;
    /* parent hwnd for child windows */
    HWND           parent;
    /* effective parent hwnd (what the driver considers to
     * be the parent for relative positioning, see create_win_data) */
    HWND           effective_parent;
    /* USER window rectangle relative to parent */
    RECT           window_rect;
    /* client area relative to parent */
    RECT           client_rect;
    /* wayland surface (if any) representing this window on the wayland side */
    struct wayland_surface *wayland_surface;
    /* wine window_surface backing this window */
    struct window_surface *surface;
    /* whether this window is currently being resized */
    BOOL           resizing;
    /* the window_rect this window should be restored to after unmaximizing */
    RECT           restore_rect;
    /* whether the window is currently fullscreen */
    BOOL           fullscreen;
    /* whether the window is currently maximized */
    BOOL           maximized;
    /* whether we are currently handling a wayland configure event */
    BOOL           handling_wayland_configure_event;
    /* the configure flags for the configure event we are handling */
    enum wayland_configure_flags wayland_configure_event_flags;
    /* whether this window has been deactivated by wayland after minimization */
    BOOL deactivated_after_minimization;
    /* whether this window is visible */
    BOOL visible;
    /* Save previous state to be able to decide when to recreate wayland surface */
    HWND           old_parent;
    RECT           old_window_rect;
    /* Whether a first wayland surface update is needed */
    BOOL           wayland_surface_needs_first_update;
    /* Whether we have a pending/unprocessed WM_WAYLAND_STATE_UPDATE message */
    BOOL           pending_state_update_message;
};

static CRITICAL_SECTION win_data_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &win_data_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": win_data_section") }
};
static CRITICAL_SECTION win_data_section = {&critsect_debug, -1, 0, 0, 0, 0};

static struct wayland_win_data *win_data_context[32768];

static HCURSOR last_cursor;
static HCURSOR invalid_cursor;

static inline int context_idx(HWND hwnd)
{
    return LOWORD(hwnd) >> 1;
}

static void set_surface_region(struct window_surface *window_surface, HRGN win_region);

/* only for use on sanitized BITMAPINFO structures */
static inline int get_dib_info_size(const BITMAPINFO *info, UINT coloruse)
{
    if (info->bmiHeader.biCompression == BI_BITFIELDS)
        return sizeof(BITMAPINFOHEADER) + 3 * sizeof(DWORD);
    if (coloruse == DIB_PAL_COLORS)
        return sizeof(BITMAPINFOHEADER) + info->bmiHeader.biClrUsed * sizeof(WORD);
    return FIELD_OFFSET(BITMAPINFO, bmiColors[info->bmiHeader.biClrUsed]);
}

static inline int get_dib_stride(int width, int bpp)
{
    return ((width * bpp + 31) >> 3) & ~3;
}

static inline int get_dib_image_size(const BITMAPINFO *info)
{
    return get_dib_stride(info->bmiHeader.biWidth, info->bmiHeader.biBitCount) *
           abs(info->bmiHeader.biHeight);
}

static HWND guess_popup_parent(struct wayland *wayland)
{
    HWND cursor_hwnd;
    HWND keyboard_hwnd;
    POINT cursor;

    GetCursorPos(&cursor);
    cursor_hwnd = wayland->pointer.focused_surface ?
                  wayland->pointer.focused_surface->hwnd :
                  WindowFromPoint(cursor);
    cursor_hwnd = GetAncestor(cursor_hwnd, GA_ROOT);
    keyboard_hwnd = GetFocus();

    TRACE("cursor_hwnd=%p keyboard_hwnd=%p\n", cursor_hwnd, keyboard_hwnd);

    /* If we have a recent mouse event, the popup parent is likely the window
     * under the cursor, so prefer it. Otherwise prefer the window with
     * the keyboard focus. */
    if (wayland->last_event_type == INPUT_MOUSE)
        return cursor_hwnd ? cursor_hwnd : keyboard_hwnd;
    else
        return keyboard_hwnd ? keyboard_hwnd : cursor_hwnd;
}

static HWND get_effective_parent(HWND hwnd, const RECT *window_rect)
{
    struct wayland *wayland = thread_init_wayland();
    DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
    /* GWLP_HWNDPARENT gets the owner for any kind of toplevel windows,
     * and the parent for child windows. */
    HWND parent_hwnd = (HWND)GetWindowLongPtrW(hwnd, GWLP_HWNDPARENT);
    HWND effective_parent_hwnd;
    MONITORINFOEXW mi;
    double monitor_width;
    double monitor_height;
    double window_width;
    double window_height;

    if (parent_hwnd == GetDesktopWindow()) parent_hwnd = 0;

    /* Child windows don't have an effective parent, unless they are children
     * of the desktop (thus effectively top-level). */
    if ((style & WS_CHILD) && parent_hwnd) return 0;

    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromRect(window_rect, MONITOR_DEFAULTTOPRIMARY),
                    (MONITORINFO *)&mi);

    monitor_width = mi.rcMonitor.right - mi.rcMonitor.left;
    monitor_height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    window_width = window_rect->right - window_rect->left;
    window_height = window_rect->bottom - window_rect->top;

    /* Many applications use top level, unowned (or owned by the desktop)
     * popup windows for menus and tooltips and depend on screen
     * coordinates for correct positioning. Since wayland can't deal with
     * screen coordinates, try to guess the effective parent window of such
     * popups and manage them as wayland subsurfaces. Our heuristic is to
     * treat all unowned, undecorated top-level windows with reasonable size
     * as transient popups.  */
    if (!parent_hwnd && (style & WS_CAPTION) != WS_CAPTION &&
        window_width > 1.0 && window_height > 1.0 &&
        window_width * window_height < 0.5 * monitor_width * monitor_height)
    {
        effective_parent_hwnd = guess_popup_parent(wayland);
        if (effective_parent_hwnd == hwnd ||
            effective_parent_hwnd == GetDesktopWindow())
        {
            effective_parent_hwnd = 0;
        }
    }
    else
    {
        effective_parent_hwnd = parent_hwnd;
    }

    TRACE("hwnd=%p parent=%p effective_parent=%p style=0x%08x rect=%s monitor=%s\n",
          hwnd, parent_hwnd, effective_parent_hwnd, style,
          wine_dbgstr_rect(window_rect),
          wine_dbgstr_rect(&mi.rcMonitor));

    return effective_parent_hwnd;
}

/***********************************************************************
 *           free_win_data
 */
static void free_win_data(struct wayland_win_data *data)
{
    TRACE("hwnd=%p\n", data->hwnd);
    win_data_context[context_idx(data->hwnd)] = NULL;

    if (data->surface) window_surface_release(data->surface);
    if (data->wayland_surface) wayland_surface_unref(data->wayland_surface);
    heap_free(data);

    LeaveCriticalSection(&win_data_section);
}

/***********************************************************************
 *           get_win_data
 *
 * Lock and return the data structure associated with a window.
 */
static struct wayland_win_data *get_win_data(HWND hwnd)
{
    struct wayland_win_data *data;

    if (!hwnd) return NULL;

    EnterCriticalSection(&win_data_section);
    if ((data = win_data_context[context_idx(hwnd)]) && data->hwnd == hwnd)
        return data;
    LeaveCriticalSection(&win_data_section);

    return NULL;
}

/***********************************************************************
 *           release_win_data
 *
 * Release the data returned by get_win_data.
 */
static void release_win_data(struct wayland_win_data *data)
{
    if (data) LeaveCriticalSection(&win_data_section);
}

/***********************************************************************
 *           wayland_surface_for_hwnd_lock
 */
struct wayland_surface *wayland_surface_for_hwnd_lock(HWND hwnd)
{
    struct wayland_win_data *data = get_win_data(hwnd);

    if (data && data->wayland_surface)
        return data->wayland_surface;

    release_win_data(data);

    return NULL;
}

/***********************************************************************
 *           wayland_surface_for_hwnd_unlock
 */
void wayland_surface_for_hwnd_unlock(struct wayland_surface *surface)
{
    if (surface) LeaveCriticalSection(&win_data_section);
}

/* Window surface support */

struct wayland_window_surface
{
    struct window_surface header;
    HWND                  hwnd;
    struct wayland_surface *wayland_surface; /* owned by wayland_win_data */
    struct wayland_buffer_queue *wayland_buffer_queue;
    RECT                  bounds;
    BOOL                  byteswap;
    HRGN                  region; /* region set through window_surface funcs */
    HRGN                  total_region; /* Total region (combined surface->region AND window_region)*/
    BYTE                  alpha;
    BOOL                  src_alpha;
    COLORREF              color_key;
    void                 *bits;
    CRITICAL_SECTION      crit;
    BITMAPINFO            info;   /* variable size, must be last */
    BOOL                  last_flush_missing_wayland;
};

static struct wayland_window_surface *get_wayland_window_surface(struct window_surface *surface)
{
    return (struct wayland_window_surface *)surface;
}

static inline void reset_bounds(RECT *bounds)
{
    bounds->left = bounds->top = INT_MAX;
    bounds->right = bounds->bottom = INT_MIN;
}

static inline void add_bounds_rect(RECT *bounds, const RECT *rect)
{
    if (rect->left >= rect->right || rect->top >= rect->bottom) return;
    bounds->left   = min(bounds->left, rect->left);
    bounds->top    = min(bounds->top, rect->top);
    bounds->right  = max(bounds->right, rect->right);
    bounds->bottom = max(bounds->bottom, rect->bottom);
}

/***********************************************************************
 *           wayland_window_surface_lock
 */
static void CDECL wayland_window_surface_lock(struct window_surface *window_surface)
{
    struct wayland_window_surface *surface = get_wayland_window_surface(window_surface);
    EnterCriticalSection(&surface->crit);
}

/***********************************************************************
 *           wayland_window_surface_unlock
 */
static void CDECL wayland_window_surface_unlock(struct window_surface *window_surface)
{
    struct wayland_window_surface *surface = get_wayland_window_surface(window_surface);
    LeaveCriticalSection(&surface->crit);
}

/***********************************************************************
 *           wayland_window_surface_get_bitmap_info
 */
static void CDECL *wayland_window_surface_get_bitmap_info(struct window_surface *window_surface,
                                                          BITMAPINFO *info)
{
    struct wayland_window_surface *surface = get_wayland_window_surface(window_surface);

    memcpy(info, &surface->info, get_dib_info_size(&surface->info, DIB_RGB_COLORS));
    return surface->bits;
}

/***********************************************************************
 *           wayland_window_surface_get_bounds
 */
static RECT CDECL *wayland_window_surface_get_bounds(struct window_surface *window_surface)
{
    struct wayland_window_surface *surface = get_wayland_window_surface(window_surface);

    return &surface->bounds;
}

/***********************************************************************
 *           wayland_window_surface_set_region
 */
static void CDECL wayland_window_surface_set_region(struct window_surface *window_surface,
                                                    HRGN region)
{
    struct wayland_window_surface *surface = get_wayland_window_surface(window_surface);

    TRACE("updating hwnd=%p surface=%p region=%p\n", surface->hwnd, surface, region);

    window_surface->funcs->lock(window_surface);
    if (!region)
    {
        if (surface->region) DeleteObject(surface->region);
        surface->region = NULL;
    }
    else
    {
        if (!surface->region) surface->region = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(surface->region, region, 0, RGN_COPY);
    }
    window_surface->funcs->unlock(window_surface);
    set_surface_region(&surface->header, (HRGN)1);
}

static void CDECL wayland_window_surface_flush(struct window_surface *window_surface)
{
    struct wayland_window_surface *surface = get_wayland_window_surface(window_surface);
    struct wayland_shm_buffer *buffer;
    RECT damage_rect;
    BOOL needs_flush;
    RGNDATA *buffer_damage;
    HRGN surface_damage_region;
    RECT *rgn_rect;
    RECT *rgn_rect_end;

    window_surface->funcs->lock(window_surface);

    TRACE("hwnd=%p surface_rect=%s bounds=%s\n", surface->hwnd,
          wine_dbgstr_rect(&surface->header.rect), wine_dbgstr_rect(&surface->bounds));

    needs_flush = IntersectRect(&damage_rect, &surface->header.rect, &surface->bounds);

    if (needs_flush &&
        (!surface->wayland_surface || !surface->wayland_buffer_queue ||
         !wayland_surface_is_drawing_allowed(surface->wayland_surface)))
    {
        TRACE("missing wayland surface=%p buffer_queue=%p or drawing disallowed, returning\n",
              surface->wayland_surface, surface->wayland_buffer_queue);
        surface->last_flush_missing_wayland = TRUE;
        goto done;
    }
    surface->last_flush_missing_wayland = FALSE;

    reset_bounds(&surface->bounds);
    if (!needs_flush) goto done;

    TRACE("flushing surface %p hwnd %p surface_rect %s bits %p alpha %02x src_alpha %d key %08x compression %d region %p\n",
           surface, surface->hwnd, wine_dbgstr_rect(&surface->header.rect),
           surface->bits, surface->alpha, surface->src_alpha, surface->color_key,
           surface->info.bmiHeader.biCompression,
           surface->total_region);

    assert(surface->wayland_buffer_queue);

    surface_damage_region = CreateRectRgnIndirect(&damage_rect);

    if (DEBUG_DUMP_FLUSH_SURFACE_BUFFER)
    {
        static int dbgid = 0;
        dbgid++;
        dump_pixels("/tmp/winedbg/flush-%.3d.pam", dbgid, surface->bits,
                    surface->info.bmiHeader.biWidth, -surface->info.bmiHeader.biHeight,
                    surface->wayland_buffer_queue->format == WL_SHM_FORMAT_ARGB8888,

                    surface_damage_region, surface->total_region);

    }

    /* If the window has a region set, only flush damaged parts of the surface
     * that intersect with it. */
    if (surface->total_region)
    {
        CombineRgn(surface_damage_region, surface_damage_region,
                   surface->total_region, RGN_AND);
    }

    wayland_buffer_queue_add_damage(surface->wayland_buffer_queue, surface_damage_region);
    buffer = wayland_buffer_queue_acquire_buffer(surface->wayland_buffer_queue);
    buffer_damage = wayland_shm_buffer_get_damage_clipped(buffer, surface->total_region);

    rgn_rect = (RECT *)buffer_damage->Buffer;
    rgn_rect_end = rgn_rect + buffer_damage->rdh.nCount;

    /* Flush damaged buffer region from window_surface bitmap to wayland SHM buffer. */
    for (;rgn_rect < rgn_rect_end; rgn_rect++)
    {
        unsigned int *src, *dst;
        int x, y, width, height;
        BOOL apply_surface_alpha;

        TRACE("damage %s\n", wine_dbgstr_rect(rgn_rect));

        if (IsRectEmpty(rgn_rect))
            continue;

        src = (unsigned int *)surface->bits +
              rgn_rect->top * surface->info.bmiHeader.biWidth +
              rgn_rect->left;
        dst = (unsigned int *)((unsigned char *)buffer->map_data +
              rgn_rect->top * buffer->stride +
              rgn_rect->left * 4);
        width = min(rgn_rect->right, buffer->width) - rgn_rect->left;
        height = min(rgn_rect->bottom, buffer->height) - rgn_rect->top;

        /* If we have an ARGB buffer and the surface alpha is not 255 we
         * need to explicitly apply it when copying the data to the
         * destination buffer. We need to do the same for an ARGB buffer without
         * src alpha, to ensure he dst alpha has a sensible value. */
        apply_surface_alpha = (buffer->format == WL_SHM_FORMAT_ARGB8888 &&
                               (surface->alpha != 255 || !surface->src_alpha));

        /* Fast path for full width rectangles. */
        if (width == buffer->width && !apply_surface_alpha &&
            surface->color_key == CLR_INVALID)
        {
            memcpy(dst, src, height * buffer->stride);
            continue;
        }

        for (y = 0; y < height; y++)
        {
            if (!apply_surface_alpha)
            {
                memcpy(dst, src, width * 4);
            }
            else if (surface->alpha == 255)
            {
                for (x = 0; x < width; x++)
                    dst[x] = 0xff000000 | src[x];
            }
            else
            {
                for (x = 0; x < width; x++)
                {
                    dst[x] = ((surface->alpha << 24) |
                              (((BYTE)(src[x] >> 16) * surface->alpha / 255) << 16) |
                              (((BYTE)(src[x] >> 8) * surface->alpha / 255) << 8) |
                              (((BYTE)src[x] * surface->alpha / 255)));
                }
            }

            if (surface->color_key != CLR_INVALID)
                for (x = 0; x < width; x++) if ((src[x] & 0xffffff) == surface->color_key) dst[x] = 0;
            src += surface->info.bmiHeader.biWidth;
            dst = (unsigned int*)((unsigned char*)dst + buffer->stride);
        }
    }

    wayland_surface_commit_buffer(surface->wayland_surface, buffer, surface_damage_region);
    wayland_shm_buffer_clear_damage(buffer);

    heap_free(buffer_damage);
    DeleteObject(surface_damage_region);

done:
    window_surface->funcs->unlock(window_surface);
}

/***********************************************************************
 *           wayland_window_surface_destroy
 */
static void CDECL wayland_window_surface_destroy(struct window_surface *window_surface)
{
    struct wayland_window_surface *surface = get_wayland_window_surface(window_surface);

    TRACE("surface=%p\n", surface);

    surface->crit.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&surface->crit);
    if (surface->region) DeleteObject(surface->region);
    if (surface->total_region) DeleteObject(surface->total_region);
    if (surface->wayland_buffer_queue)
        wayland_buffer_queue_destroy(surface->wayland_buffer_queue);
    heap_free(surface->bits);
    heap_free(surface);
}

static const struct window_surface_funcs wayland_window_surface_funcs =
{
    wayland_window_surface_lock,
    wayland_window_surface_unlock,
    wayland_window_surface_get_bitmap_info,
    wayland_window_surface_get_bounds,
    wayland_window_surface_set_region,
    wayland_window_surface_flush,
    wayland_window_surface_destroy
};

static BOOL is_wayland_window_surface(struct window_surface *surface)
{
    return surface && surface->funcs == &wayland_window_surface_funcs;
}

static BOOL is_wayland_layered_window_surface(struct window_surface *surface)
{
    return surface && surface->funcs == &wayland_window_surface_funcs &&
        get_wayland_window_surface(surface)->wayland_buffer_queue->format ==
            WL_SHM_FORMAT_ARGB8888 &&
        get_wayland_window_surface(surface)->src_alpha;
}

/***********************************************************************
 *           set_color_key
 */
static void set_color_key(struct wayland_window_surface *surface, COLORREF key)
{
    assert(surface->info.bmiHeader.biBitCount == 32);

    if (key == CLR_INVALID)
        surface->color_key = CLR_INVALID;
    else if (key & (1 << 24))  /* PALETTEINDEX */
        surface->color_key = 0;
    else if (key >> 16 == 0x10ff)  /* DIBINDEX */
        surface->color_key = 0;
    else
        surface->color_key = (GetRValue(key) << 16) | (GetGValue(key) << 8) | GetBValue(key);
}

/***********************************************************************
 *           wayland_window_surface_preferred_format
 */
static int wayland_window_surface_get_preferred_format(struct wayland_window_surface *wws)
{
    int format;
    HRGN window_region = CreateRectRgn(0, 0, 0, 0);

    /* If the surface has whole or src alpha use ARGB buffers. Also use ARGB to
     * implement window regions (areas out of the region are transparent). */
    if ((window_region && GetWindowRgn(wws->hwnd, window_region) != ERROR) ||
        wws->src_alpha || wws->alpha != 255)
        format = WL_SHM_FORMAT_ARGB8888;
    else
        format = WL_SHM_FORMAT_XRGB8888;

    if (window_region) DeleteObject(window_region);

    return format;
}

/***********************************************************************
 *           update_wayland_buffer_queue
 */
static void update_wayland_buffer_queue(struct wayland_window_surface *surface)
{
    int width;
    int height;
    int format;

    if (!surface->wayland_buffer_queue) return;

    width = surface->wayland_buffer_queue->width;
    height = surface->wayland_buffer_queue->height;
    format = wayland_window_surface_get_preferred_format(surface);

    wayland_buffer_queue_destroy(surface->wayland_buffer_queue);

    surface->wayland_buffer_queue =
        wayland_buffer_queue_create(surface->wayland_surface->wayland,
                                    width, height, format);
}

/***********************************************************************
 *           set_surface_region
 */
static void set_surface_region(struct window_surface *window_surface, HRGN win_region)
{
    struct wayland_window_surface *surface;
    HRGN region = 0;

    if (!is_wayland_window_surface(window_surface)) return;

    surface = get_wayland_window_surface(window_surface);

    TRACE("hwnd %p surface %p region %p\n", surface->hwnd, surface, win_region);

    if (win_region == (HRGN)1)  /* hack: win_region == 1 means retrieve region from server */
    {
        region = CreateRectRgn(0, 0, 0, 0);
        if (region && GetWindowRgn(surface->hwnd, region) == ERROR)
        {
            DeleteObject(region);
            region = 0;
        }
    }
    else if (win_region)
    {
        region = CreateRectRgn(0, 0, 0, 0);
        if (region) CombineRgn(region, win_region, 0, RGN_COPY);
    }

    if (surface->region)
    {
        if (region)
        {
            CombineRgn(region, region, surface->region, RGN_AND);
        }
        else
        {
            region = CreateRectRgn(0, 0, 0, 0);
            if (region) CombineRgn(region, surface->region, 0, RGN_COPY);
        }
    }

    window_surface->funcs->lock(window_surface);

    if (surface->total_region) DeleteObject(surface->total_region);
    surface->total_region = region;
    *window_surface->funcs->get_bounds(window_surface) = surface->header.rect;
    /* Unconditionally update the buffer queue to ensure we have clean buffers, so
     * that areas outside the region are transparent. */
    update_wayland_buffer_queue(surface);

    TRACE("hwnd %p bounds %s rect %s\n", surface->hwnd,
          wine_dbgstr_rect(window_surface->funcs->get_bounds(window_surface)),
          wine_dbgstr_rect(&surface->header.rect));

    window_surface->funcs->unlock(window_surface);
}

/***********************************************************************
 *           create_surface
 */
static struct window_surface *create_surface(HWND hwnd, const RECT *rect,
                                             BYTE alpha, COLORREF color_key, BOOL src_alpha)
{
    struct wayland_window_surface *surface;
    int width = rect->right - rect->left, height = rect->bottom - rect->top;

    TRACE("win %p rect %s alpha %x src_alpha %d\n", hwnd, wine_dbgstr_rect(rect), alpha, src_alpha);
    surface = heap_alloc_zero(sizeof(*surface));
    if (!surface) return NULL;
    surface->info.bmiHeader.biSize = sizeof(surface->info.bmiHeader);
    surface->info.bmiHeader.biClrUsed = 0;
    surface->info.bmiHeader.biBitCount = 32;
    surface->info.bmiHeader.biCompression = BI_RGB;
    surface->info.bmiHeader.biWidth       = width;
    surface->info.bmiHeader.biHeight      = -height; /* top-down */
    surface->info.bmiHeader.biPlanes      = 1;
    surface->info.bmiHeader.biSizeImage   = get_dib_image_size(&surface->info);

    InitializeCriticalSection(&surface->crit);
    surface->crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": surface");

    surface->header.funcs = &wayland_window_surface_funcs;
    surface->header.rect  = *rect;
    surface->header.ref   = 1;
    surface->hwnd         = hwnd;
    surface->alpha        = alpha;
    surface->src_alpha    = src_alpha;
    set_color_key(surface, color_key);
    set_surface_region(&surface->header, (HRGN)1);
    reset_bounds(&surface->bounds);

    if (!(surface->bits = heap_alloc(surface->info.bmiHeader.biSizeImage)))
        goto failed;

    TRACE("created %p hwnd %p %s bits %p-%p compression %d\n", surface, hwnd, wine_dbgstr_rect(rect),
           surface->bits, (char *)surface->bits + surface->info.bmiHeader.biSizeImage,
           surface->info.bmiHeader.biCompression);

    return &surface->header;

failed:
    wayland_window_surface_destroy(&surface->header);
    return NULL;
}

/***********************************************************************
 *           set_surface_layered
 */
static void set_surface_layered(struct window_surface *window_surface, BYTE alpha,
                                COLORREF color_key)
{
    struct wayland_window_surface *surface;
    COLORREF prev_key;
    BYTE prev_alpha;

    if (!is_wayland_window_surface(window_surface)) return;

    surface = get_wayland_window_surface(window_surface);

    window_surface->funcs->lock(window_surface);
    prev_key = surface->color_key;
    prev_alpha = surface->alpha;
    surface->alpha = alpha;
    surface->src_alpha = TRUE;
    set_color_key(surface, color_key);
    if (alpha != prev_alpha || surface->color_key != prev_key)  /* refresh */
        *window_surface->funcs->get_bounds(window_surface) = surface->header.rect;

    if (surface->wayland_buffer_queue &&
        surface->wayland_buffer_queue->format != WL_SHM_FORMAT_ARGB8888)
    {
        update_wayland_buffer_queue(surface);
    }

    window_surface->funcs->unlock(window_surface);
}

static BOOL process_wayland_events(DWORD mask)
{
    struct wayland *wayland = thread_wayland();
    int dispatched;

    if (!wayland)
        return FALSE;

    wayland->last_dispatch_mask = 0;

    dispatched = wayland_dispatch_non_buffer(wayland);
    if (dispatched)
        wayland->last_dispatch_mask |= QS_SENDMESSAGE;

    TRACE("dispatched=%d mask=%s%s%s%s%s%s%s\n",
          dispatched,
          (wayland->last_dispatch_mask & QS_KEY) ? "QS_KEY|" : "",
          (wayland->last_dispatch_mask & QS_MOUSEMOVE) ? "QS_MOUSEMOVE|" : "",
          (wayland->last_dispatch_mask & QS_MOUSEBUTTON) ? "QS_MOUSEBUTTON|" : "",
          (wayland->last_dispatch_mask & QS_INPUT) ? "QS_INPUT|" : "",
          (wayland->last_dispatch_mask & QS_PAINT) ? "QS_PAINT|" : "",
          (wayland->last_dispatch_mask & QS_POSTMESSAGE) ? "QS_POSTMESSAGE|" : "",
          (wayland->last_dispatch_mask & QS_SENDMESSAGE) ? "QS_SENDMESSAGE|" : "");

    return wayland->last_dispatch_mask & mask;
}

/***********************************************************************
 *           WAYLAND_MsgWaitForMultipleObjectsEx
 */
DWORD CDECL WAYLAND_MsgWaitForMultipleObjectsEx(DWORD count, const HANDLE *handles,
                                                DWORD timeout, DWORD mask, DWORD flags)
{
    DWORD ret;

    if (process_wayland_events(mask))
        return count - 1;

    ret = WaitForMultipleObjectsEx(count, handles, flags & MWMO_WAITALL,
                                   timeout, flags & MWMO_ALERTABLE);

    return ret;
}

/**********************************************************************
 *           WAYLAND_CreateWindow
 */
BOOL CDECL WAYLAND_CreateWindow(HWND hwnd)
{
    TRACE("%p\n", hwnd);

    if (hwnd == GetDesktopWindow())
    {
        /* Initialize wayland so that the desktop process has access
         * to all the wayland related information (e.g., displays). */
        thread_init_wayland();
    }

    return TRUE;
}

/***********************************************************************
 *           WAYLAND_DestroyWindow
 */
void CDECL WAYLAND_DestroyWindow(HWND hwnd)
{
    struct wayland_win_data *data;

    TRACE("%p\n", hwnd);

    if (!(data = get_win_data(hwnd))) return;
    wayland_destroy_gl_drawable(hwnd);
    wayland_invalidate_vulkan_objects(hwnd);
    free_win_data(data);
}

/***********************************************************************
 *           create_win_data
 *
 * Create a data window structure for an existing window.
 */
static struct wayland_win_data *create_win_data(HWND hwnd)
{
    struct wayland_win_data *data;
    HWND parent;

    /* Don't create win_data for desktop or HWND_MESSAGE windows. */
    if (!(parent = GetAncestor(hwnd, GA_PARENT))) return NULL;
    if (parent != GetDesktopWindow() && !GetAncestor(parent, GA_PARENT)) return NULL;

    if (!(data = heap_alloc_zero(sizeof(*data))))
        return NULL;

    data->hwnd = hwnd;
    data->wayland_surface_needs_first_update = TRUE;

    EnterCriticalSection(&win_data_section);
    win_data_context[context_idx(hwnd)] = data;

    TRACE("hwnd=%p\n", data->hwnd);

    return data;
}

static BOOL win_data_wayland_surface_needs_update(struct wayland_win_data *data)
{
    if (data->wayland_surface_needs_first_update)
        return TRUE;

    /* Change of parentage (either actual or effective) requires recreating the
     * whole win_data to ensure we have a properly owned wayland surface. We
     * check for change of effective parent only if the window moved in any
     * way. */
    if ((!EqualRect(&data->window_rect, &data->old_window_rect) &&
         data->effective_parent != get_effective_parent(data->hwnd, &data->window_rect)) ||
        data->parent != data->old_parent)
    {
        return TRUE;
    }

    /* If this is currently or potentially a toplevel surface, and its
     * visibility state has changed, recreate win_data so that we only have
     * xdg_toplevels for visible windows. */
    if (data->wayland_surface && !data->wayland_surface->wl_subsurface)
    {
        BOOL visible = data->wayland_surface->xdg_toplevel != NULL;
        if (data->visible != visible)
            return TRUE;
    }

    return FALSE;
}

/***********************************************************************
 *           WAYLAND_WindowPosChanging
 */
BOOL CDECL WAYLAND_WindowPosChanging(HWND hwnd, HWND insert_after, UINT swp_flags,
                                     const RECT *window_rect, const RECT *client_rect, RECT *visible_rect,
                                     struct window_surface **surface)
{
    struct wayland_win_data *data = get_win_data(hwnd);
    RECT surface_rect;
    DWORD flags;
    COLORREF key;
    BYTE alpha;
    BOOL exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
    HWND parent = GetAncestor(hwnd, GA_PARENT);

    TRACE("win %p window %s client %s visible %s style %08x ex %08x flags %08x after %p\n",
           hwnd, wine_dbgstr_rect(window_rect), wine_dbgstr_rect(client_rect),
           wine_dbgstr_rect(visible_rect), style, exstyle,
           swp_flags, insert_after);

    if (!data && !(data = create_win_data(hwnd))) return TRUE;

    data->old_parent = data->parent;
    data->parent = (parent == GetDesktopWindow()) ? 0 : parent;
    data->old_window_rect = data->window_rect;
    data->window_rect = *window_rect;
    data->client_rect = *client_rect;
    data->visible = (style & WS_VISIBLE) == WS_VISIBLE || (swp_flags & SWP_SHOWWINDOW);

    *visible_rect = *window_rect;

    /* create the window surface if necessary */
    if (*surface) window_surface_release(*surface);
    *surface = NULL;  /* indicate that we want to draw directly to the window */

    /* If we don't want a dedicated surface for this window... */
    if (data->parent) goto done;

    if (swp_flags & SWP_HIDEWINDOW) goto done;
    surface_rect = *window_rect;
    OffsetRect(&surface_rect, -surface_rect.left, -surface_rect.top);

    if (data->surface)
    {
        if (!memcmp(&data->surface->rect, &surface_rect, sizeof(surface_rect)))
        {
            /* existing surface is good enough */
            window_surface_add_ref(data->surface);
            if (*surface) window_surface_release(*surface);
            *surface = data->surface;
            TRACE("reusing surface %p\n", *surface);
            goto done;
        }
    }
    if (!(swp_flags & SWP_SHOWWINDOW) && !(GetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE)) goto done;

    key = alpha = flags = 0;
    if (!(exstyle & WS_EX_LAYERED) || !GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags)) flags = 0;
    if (!(flags & LWA_ALPHA)) alpha = 255;
    if (!(flags & LWA_COLORKEY)) key = CLR_INVALID;

    *surface = create_surface(data->hwnd, &surface_rect, alpha, key, FALSE);

    TRACE("new surface %p\n", *surface);
done:
    release_win_data(data);
    return TRUE;
}

static void win_data_update_window_surface(struct wayland_win_data *data)
{
    struct wayland_window_surface *wws;

    data->surface->funcs->lock(data->surface);

    wws = get_wayland_window_surface(data->surface);
    wws->wayland_surface = data->wayland_surface;

    if (!wws->wayland_buffer_queue && data->wayland_surface)
    {
        wws->wayland_buffer_queue =
            wayland_buffer_queue_create(data->wayland_surface->wayland,
                    wws->info.bmiHeader.biWidth, -wws->info.bmiHeader.biHeight,
                    wayland_window_surface_get_preferred_format(wws));
    }

    data->surface->funcs->unlock(data->surface);
}

static void win_data_update_wayland_surface(struct wayland_win_data *data)
{
    struct wayland *wayland;
    HWND effective_parent_hwnd;
    struct wayland_surface *parent_surface;
    DWORD style;

    TRACE("hwnd=%p\n", data->hwnd);

    data->wayland_surface_needs_first_update = FALSE;

    wayland = thread_init_wayland();

    wayland_invalidate_vulkan_objects(data->hwnd);

    if (data->wayland_surface)
    {
        wayland_surface_unref(data->wayland_surface);
        data->wayland_surface = NULL;
    }

    /* Only create wayland surfaces for toplevel windows. Let Wine core handle
     * the drawing of other windows in their corresponding top level window
     * surface. */
    if (data->parent) goto out;

    effective_parent_hwnd = get_effective_parent(data->hwnd, &data->window_rect);
    parent_surface = NULL;

    if (effective_parent_hwnd)
    {
        parent_surface = wayland_surface_for_hwnd_lock(effective_parent_hwnd);
        wayland_surface_for_hwnd_unlock(parent_surface);
    }

    data->effective_parent = effective_parent_hwnd;
    style = GetWindowLongW(data->hwnd, GWL_STYLE);

    /* Use wayland subsurfaces for owned win32 windows that are transient (i.e., don't have
     * a titlebar). Otherwise, if the window is visible make it wayland toplevel. Finally,
     * if the window is not visible create a plain (without a role) surface to avoid
     * polluting the compositor with empty xdg_toplevels. */
    if ((style & WS_CAPTION) != WS_CAPTION && parent_surface)
        data->wayland_surface = wayland_surface_create_subsurface(wayland, parent_surface);
    else if (data->visible)
        data->wayland_surface = wayland_surface_create_toplevel(wayland, parent_surface);
    else
        data->wayland_surface = wayland_surface_create_plain(wayland);

    data->wayland_surface->hwnd = data->hwnd;

    if (data->wayland_surface && data->wayland_surface->xdg_toplevel)
    {
        WCHAR text[1024];
        if (!InternalGetWindowText(data->hwnd, text, ARRAY_SIZE(text))) text[0] = 0;
        wayland_surface_set_title(data->wayland_surface, text);
    }

out:
    wayland_update_gl_drawable(data->hwnd, data->wayland_surface);
}

static void win_data_update_wayland_surface_state(struct wayland_win_data *data)
{
    RECT offset_rect = {0};
    MONITORINFOEXW mi;
    int width = data->window_rect.right - data->window_rect.left;
    int height = data->window_rect.bottom - data->window_rect.top;
    int monitor_width;
    int monitor_height;
    struct wayland_win_data *parent_data;
    enum wayland_configure_flags conf_flags = 0;
    struct wayland_output *output;
    DWORD style = GetWindowLongW(data->hwnd, GWL_STYLE);

    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(data->hwnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO *)&mi);
    monitor_width = mi.rcMonitor.right - mi.rcMonitor.left;
    monitor_height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    output = wayland_get_output_by_wine_name(data->wayland_surface->wayland, mi.szDevice);

    TRACE("hwnd=%p window=%dx%d monitor=%dx%d maximized=%d fullscreen=%d handling_event=%d\n",
          data->hwnd, width, height, monitor_width, monitor_height,
          data->maximized, data->fullscreen, data->handling_wayland_configure_event);

    wayland_surface_set_main_output(data->wayland_surface, output);

    if (!(style & WS_VISIBLE))
    {
        wayland_surface_set_drawing_allowed(data->wayland_surface, TRUE);
        wayland_surface_unmap(data->wayland_surface);
        return;
    }

    /* If we are currently handling a wayland configure event (i.e., we are
     * being called through handle_wm_wayland_configure() -> SetWindowPos()),
     * use the event configure flags directly. Otherwise try to infer the flags
     * from the window style and rectangle. */
    if (data->handling_wayland_configure_event)
    {
        conf_flags = data->wayland_configure_event_flags;
    }
    else
    {
        /* Set the wayland fullscreen state if the window rect covers the
         * current monitor exactly. Note that we set/maintain the fullscreen
         * wayland state, even if the window style is also maximized. */
        if (EqualRect(&data->window_rect, &mi.rcMonitor) &&
            !(style & (WS_MINIMIZE|WS_CAPTION)))
        {
            conf_flags |= WAYLAND_CONFIGURE_FLAG_FULLSCREEN;
        }
        if (style & WS_MAXIMIZE)
        {
            conf_flags |= WAYLAND_CONFIGURE_FLAG_MAXIMIZED;
        }
    }

    /* First do all state unsettings, before setting new state. Some wayland
     * compositors misbehave if the order is reversed. */
    if (data->maximized && !(conf_flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED))
    {
        if (!data->handling_wayland_configure_event)
            xdg_toplevel_unset_maximized(data->wayland_surface->xdg_toplevel);
        data->maximized = FALSE;
    }

    if (data->fullscreen && !(conf_flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN))
    {
        if (!data->handling_wayland_configure_event)
            xdg_toplevel_unset_fullscreen(data->wayland_surface->xdg_toplevel);
        data->fullscreen = FALSE;
    }

    if (!data->maximized && (conf_flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED))
    {
        if (!data->handling_wayland_configure_event)
            xdg_toplevel_set_maximized(data->wayland_surface->xdg_toplevel);
        data->maximized = TRUE;
    }

   /* Set the fullscreen state after the maximized state on the wayland surface
    * to ensure compositors apply the final fullscreen state properly. */
    if (!data->fullscreen && (conf_flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN))
    {
        if (!data->handling_wayland_configure_event)
        {
            xdg_toplevel_set_fullscreen(data->wayland_surface->xdg_toplevel,
                                        output ? output->wl_output : NULL);
        }
        data->fullscreen = TRUE;
    }

    if (!(conf_flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN) &&
        !(conf_flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED) &&
        !(style & WS_MINIMIZE))
    {
        data->restore_rect = data->window_rect;
        TRACE("setting hwnd=%p restore_rect=%s\n",
              data->hwnd, wine_dbgstr_rect(&data->restore_rect));
    }

    TRACE("hwnd=%p current state maximized=%d fullscreen=%d\n",
          data->hwnd, data->maximized, data->fullscreen);

    if (data->wayland_surface->xdg_toplevel)
    {
        int wayland_width, wayland_height;
        BOOL compat_with_current = FALSE;
        BOOL compat_with_pending = FALSE;

        wayland_surface_coords_rounded_from_wine(data->wayland_surface, width, height,
                                                 &wayland_width, &wayland_height);

        if (data->wayland_surface->current.serial &&
            wayland_surface_configure_is_compatible(&data->wayland_surface->current,
                                                    wayland_width, wayland_height,
                                                    conf_flags))
        {
            compat_with_current = TRUE;
        }

        if (data->wayland_surface->pending.serial &&
            wayland_surface_configure_is_compatible(&data->wayland_surface->pending,
                                                    wayland_width, wayland_height,
                                                    conf_flags))
        {
            compat_with_pending = TRUE;
        }

        TRACE("current conf serial=%d size=%dx%d flags=%#x\n compat=%d\n",
              data->wayland_surface->current.serial,
              data->wayland_surface->current.width,
              data->wayland_surface->current.height,
              data->wayland_surface->current.configure_flags,
              compat_with_current);
        TRACE("pending conf serial=%d size=%dx%d flags=%#x compat=%d\n",
              data->wayland_surface->pending.serial,
              data->wayland_surface->pending.width,
              data->wayland_surface->pending.height,
              data->wayland_surface->pending.configure_flags,
              compat_with_pending);

        /* Only update the wayland surface state to match the window
         * configuration if the surface can accept the new config, in order to
         * avoid causing a protocol error. */
        if (!compat_with_pending && !compat_with_current)
        {
            TRACE("hwnd=%p window state not compatible with current or "
                  "pending wayland surface configuration\n", data->hwnd);
            wayland_surface_set_drawing_allowed(data->wayland_surface, FALSE);
            return;
        }

        if (compat_with_pending)
            wayland_surface_ack_configure(data->wayland_surface);
    }

    /* We manage some top level, popup window with subsurfaces (see create_win_data),
     * which use coordinates relative to their parent surface. In order to properly
     * handle the positioning of such windows, we treat the effective parent window
     * as the "parent" below. */
    parent_data = get_win_data(data->effective_parent);

    if (parent_data)
    {
        TRACE("positioning relative to parent hwnd=%p window %s client %s\n",
              parent_data->hwnd,
              wine_dbgstr_rect(&parent_data->window_rect),
              wine_dbgstr_rect(&parent_data->client_rect));
        offset_rect = parent_data->window_rect;
        offset_rect.left = -offset_rect.left;
        offset_rect.top = -offset_rect.top;
    }

    wayland_surface_reconfigure(data->wayland_surface,
                                offset_rect.left + data->window_rect.left,
                                offset_rect.top + data->window_rect.top,
                                width, height);
    /* The GL/VK subsurface (if any), is positioned over the client area of the
     * window. The position of the GL/VK subsurface is relative to the window
     * top-left. */
    wayland_surface_reconfigure_glvk(data->wayland_surface,
                                     data->client_rect.left - data->window_rect.left,
                                     data->client_rect.top - data->window_rect.top,
                                     data->client_rect.right - data->client_rect.left,
                                     data->client_rect.bottom - data->client_rect.top);

    wayland_surface_reconfigure_apply(data->wayland_surface);

    wayland_surface_update_pointer_confinement(data->wayland_surface);

    wayland_surface_set_drawing_allowed(data->wayland_surface, TRUE);

    release_win_data(parent_data);
}

static void update_wayland_state(struct wayland_win_data *data)
{
    if (win_data_wayland_surface_needs_update(data))
        win_data_update_wayland_surface(data);

    if (data->surface)
        win_data_update_window_surface(data);

    if (data->wayland_surface)
        win_data_update_wayland_surface_state(data);

    if (data->surface)
    {
        struct wayland_window_surface *wws = get_wayland_window_surface(data->surface);
        /* If the last flush was not performed due to not having a wayland surface or
         * buffer queue, flush now. */
        if (wws->last_flush_missing_wayland)
            data->surface->funcs->flush(data->surface);
    }
}

/***********************************************************************
 *           WAYLAND_WindowPosChanged
 */
void CDECL WAYLAND_WindowPosChanged(HWND hwnd, HWND insert_after, UINT swp_flags,
                                    const RECT *window_rect, const RECT *client_rect,
                                    const RECT *visible_rect, const RECT *valid_rects,
                                    struct window_surface *surface)
{
    struct wayland_win_data *data;

    if (!(data = get_win_data(hwnd))) return;

    TRACE("hwnd %p window %s client %s visible %s style %08x effective_parent %p after %p flags %08x\n",
          hwnd, wine_dbgstr_rect(window_rect), wine_dbgstr_rect(client_rect),
          wine_dbgstr_rect(visible_rect), GetWindowLongW(hwnd, GWL_STYLE),
          data->effective_parent, insert_after, swp_flags);

    if (surface) window_surface_add_ref(surface);
    if (data->surface) window_surface_release(data->surface);
    data->surface = surface;

    /* TODO: Try to handle z-order */

    /* In some cases, notably when the app calls UpdateLayeredWindow, position
     * and size changes may be emitted from a thread other than the window
     * thread. Since in the current implementation updating the wayland state
     * needs to happen in the context of the window thread to avoid racy
     * interactions, post a message to update the state in the right thread. */
    if (GetCurrentThreadId() == GetWindowThreadProcessId(hwnd, NULL))
    {
        update_wayland_state(data);
    }
    else if (!data->pending_state_update_message)
    {
        PostMessageW(hwnd, WM_WAYLAND_STATE_UPDATE, 0, 0);
        data->pending_state_update_message = TRUE;
    }

    release_win_data(data);
}

/***********************************************************************
 *           WAYLAND_ShowWindow
 */
UINT CDECL WAYLAND_ShowWindow(HWND hwnd, INT cmd, RECT *rect, UINT swp)
{
    struct wayland_surface *wsurface;

    TRACE("hwnd=%p cmd=%d\n", hwnd, cmd);

    if (IsRectEmpty(rect)) return swp;
    if (!IsIconic(hwnd)) return swp;
    /* always hide icons off-screen */
    if (rect->left != -32000 || rect->top != -32000)
    {
        OffsetRect(rect, -32000 - rect->left, -32000 - rect->top);
        swp &= ~(SWP_NOMOVE | SWP_NOCLIENTMOVE);
    }

    if ((wsurface = wayland_surface_for_hwnd_lock(hwnd)) && wsurface->xdg_toplevel)
        xdg_toplevel_set_minimized(wsurface->xdg_toplevel);

    wayland_surface_for_hwnd_unlock(wsurface);

    return swp;
}

/*****************************************************************
 *	     WAYLAND_SetParent
 */
void CDECL WAYLAND_SetParent(HWND hwnd, HWND parent, HWND old_parent)
{
    TRACE("hwnd=%p old=%p new=%p\n", hwnd, old_parent, parent);
    /* We handle reparenting in the next WAYLAND_WindowPosChanging call */
}

/***********************************************************************
 *           WAYLAND_SetCapture
 */
void CDECL WAYLAND_SetCapture(HWND hwnd, UINT flags)
{
    TRACE("hwnd=%p\n", hwnd);

    /* TODO: wayland */
}


/***********************************************************************
 *           wayland_init_set_cursor
 *
 *  Initalize internal information, so that we can track the last set
 *  cursor properly.
 */
BOOL wayland_init_set_cursor(void)
{
    /* Allocate a handle that we are going to treat as invalid. */
    SERVER_START_REQ(alloc_user_handle)
    {
        if (!wine_server_call_err(req))
            invalid_cursor = wine_server_ptr_handle(reply->handle);
    }
    SERVER_END_REQ;

    TRACE("invalid_cursor=%p\n", invalid_cursor);

    last_cursor = invalid_cursor;

    return invalid_cursor != NULL;
}

/***********************************************************************
 *           wayland_invalidate_set_cursor
 *
 *  Invalidate the cursor we consider to be set, effectively forcing
 *  the application of next SetCursor call.
 */
void wayland_invalidate_set_cursor(void)
{
    InterlockedExchangePointer((void **)&last_cursor, invalid_cursor);
}

/***********************************************************************
 *           WAYLAND_SetCursor
 */
void CDECL WAYLAND_SetCursor(HCURSOR handle)
{
    TRACE("hcursor=%p last_cursor=%p\n", handle, last_cursor);

    if (InterlockedExchangePointer((void **)&last_cursor, handle) != handle)
    {
        HWND foreground = GetForegroundWindow();
        struct wayland *wayland = thread_wayland();

        /* If a non GUI thread calls SetCursor, just ignore it, since it doesn't
         * have any wayland surfaces and thus changing the cursor will not have
         * any effect. */
        if (wayland)
            wayland_pointer_update_cursor_from_win32(&wayland->pointer, handle);
        else
            wayland_invalidate_set_cursor();

        /* Cursor visibility affects pointer confinement mode. */
        SendMessageW(foreground, WM_WAYLAND_POINTER_CONFINEMENT_UPDATE,
                     WAYLAND_POINTER_CONFINEMENT_RETAIN_CLIP, 0);
    }
}

/***********************************************************************
 *           WAYLAND_ClipCursor
 */
void CDECL WAYLAND_ClipCursor(const RECT *clip)
{
    HWND foreground = GetForegroundWindow();
    WPARAM confine = clip ? WAYLAND_POINTER_CONFINEMENT_SYSTEM_CLIP :
                            WAYLAND_POINTER_CONFINEMENT_UNSET_CLIP;

    SendMessageW(foreground, WM_WAYLAND_POINTER_CONFINEMENT_UPDATE, confine, 0);
}

/***********************************************************************
 *           WAYLAND_SetWindowStyle
 */
void CDECL WAYLAND_SetWindowStyle(HWND hwnd, INT offset, STYLESTRUCT *style)
{
    struct wayland_win_data *data;
    DWORD changed = style->styleNew ^ style->styleOld;

    TRACE("hwnd=%p offset=%d changed=%#x\n", hwnd, offset, changed);

    if (hwnd == GetDesktopWindow()) return;
    if (!(data = get_win_data(hwnd))) return;

    /* changing WS_EX_LAYERED resets attributes */
    if (offset == GWL_EXSTYLE && (changed & WS_EX_LAYERED))
    {
        TRACE("hwnd=%p changed layered\n", hwnd);
        if (is_wayland_layered_window_surface(data->surface))
        {
            if (data->surface) window_surface_release(data->surface);
            data->surface = NULL;
        }
        else if (data->surface)
        {
            set_surface_layered(data->surface, 255, CLR_INVALID);
        }
    }
    release_win_data(data);
}

/*****************************************************************
 *		WAYLAND_SetWindowText
 */
void CDECL WAYLAND_SetWindowText(HWND hwnd, LPCWSTR text)
{
    struct wayland_surface *wsurface = wayland_surface_for_hwnd_lock(hwnd);

    TRACE("hwnd=%p text=%s\n", hwnd, wine_dbgstr_w(text));

    if (wsurface && wsurface->xdg_toplevel)
        wayland_surface_set_title(wsurface, text);

    wayland_surface_for_hwnd_unlock(wsurface);
}

/***********************************************************************
 *           WAYLAND_SetWindowRgn
 */
void CDECL WAYLAND_SetWindowRgn(HWND hwnd, HRGN hrgn, BOOL redraw)
{
    struct wayland_win_data *data;

    TRACE("hwnd=%p\n", hwnd);

    if ((data = get_win_data(hwnd)))
    {
        if (data->surface) set_surface_region(data->surface, hrgn);
        release_win_data(data);
    }
    else
    {
        FIXME("not supported on other process window %p\n", hwnd);
    }
}

/***********************************************************************
 *	     WAYLAND_SetLayeredWindowAttributes
 */
void CDECL WAYLAND_SetLayeredWindowAttributes(HWND hwnd, COLORREF key, BYTE alpha, DWORD flags)
{
    struct wayland_win_data *data;

    TRACE("hwnd=%p\n", hwnd);

    if (!(flags & LWA_ALPHA)) alpha = 255;
    if (!(flags & LWA_COLORKEY)) key = CLR_INVALID;

    if ((data = get_win_data(hwnd)))
    {
        if (data->surface) set_surface_layered(data->surface, alpha, key);
        release_win_data(data);
    }
}

static enum xdg_toplevel_resize_edge hittest_to_resize_edge(WPARAM hittest)
{
    switch (hittest) {
    case WMSZ_LEFT:        return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    case WMSZ_RIGHT:       return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    case WMSZ_TOP:         return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    case WMSZ_TOPLEFT:     return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    case WMSZ_TOPRIGHT:    return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    case WMSZ_BOTTOM:      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    case WMSZ_BOTTOMLEFT:  return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    case WMSZ_BOTTOMRIGHT: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    default:               return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    }
}

/***********************************************************************
 *          WAYLAND_SysCommand
 */
LRESULT CDECL WAYLAND_SysCommand(HWND hwnd, WPARAM wparam, LPARAM lparam)
{
    struct wayland_win_data *data;
    LRESULT ret = -1;
    WPARAM command = wparam & 0xfff0;
    WPARAM hittest = wparam & 0x0f;
    struct wayland_surface *surface;

    TRACE("cmd=%lx hwnd=%p, %x, %lx,\n", command, hwnd, (unsigned)wparam, lparam);

    if (!(data = get_win_data(hwnd)) || !data->wayland_surface) goto done;

    surface = data->wayland_surface;

    if (command == SC_MOVE)
    {
        if (surface->wayland->last_button_serial)
        {
            xdg_toplevel_move(surface->xdg_toplevel, surface->wayland->wl_seat,
                              surface->wayland->last_button_serial);
        }
        release_win_data(data);
        return 0;
    }
    else if (command == SC_SIZE)
    {
        if (surface->wayland->last_button_serial)
        {
            xdg_toplevel_resize(surface->xdg_toplevel, surface->wayland->wl_seat,
                                surface->wayland->last_button_serial,
                                hittest_to_resize_edge(hittest));
        }
        release_win_data(data);
        return 0;
    }
    else if (command == SC_MAXIMIZE)
    {
        xdg_toplevel_set_maximized(data->wayland_surface->xdg_toplevel);
        release_win_data(data);
        return 0;
    }
    else if (command == SC_RESTORE)
    {
        xdg_toplevel_unset_maximized(data->wayland_surface->xdg_toplevel);
        release_win_data(data);
        return 0;
    }

done:
    release_win_data(data);
    return ret;
}

/*****************************************************************************
 *           WAYLAND_UpdateLayeredWindow
 */
BOOL CDECL WAYLAND_UpdateLayeredWindow(HWND hwnd, const UPDATELAYEREDWINDOWINFO *info,
                                       const RECT *window_rect)
{
    struct window_surface *surface;
    struct wayland_win_data *data;
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, 0 };
    COLORREF color_key = (info->dwFlags & ULW_COLORKEY) ? info->crKey : CLR_INVALID;
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *bmi = (BITMAPINFO *)buffer;
    void *src_bits, *dst_bits;
    RECT rect, src_rect;
    HDC hdc = 0;
    HBITMAP dib;
    BOOL ret = FALSE;

    if (!(data = get_win_data(hwnd))) return FALSE;

    TRACE("hwnd %p colorkey %x dirty %s flags %x src_alpha %d alpha_format %d\n",
          hwnd, info->crKey, wine_dbgstr_rect(info->prcDirty), info->dwFlags,
          info->pblend->SourceConstantAlpha, info->pblend->AlphaFormat == AC_SRC_ALPHA);

    rect = *window_rect;
    OffsetRect(&rect, -window_rect->left, -window_rect->top);

    surface = data->surface;
    if (!is_wayland_layered_window_surface(surface))
    {
        if (surface) window_surface_release(surface);
        surface = NULL;
    }

    if (!surface || !EqualRect(&surface->rect, &rect))
    {
        data->surface = create_surface(data->hwnd, &rect, 255, color_key, TRUE);
        if (surface) window_surface_release(surface);
        surface = data->surface;
        win_data_update_window_surface(data);
    }
    else
    {
        set_surface_layered(surface, 255, color_key);
    }

    if (surface) window_surface_add_ref(surface);
    release_win_data(data);

    if (!surface) return FALSE;
    if (!info->hdcSrc)
    {
        window_surface_release(surface);
        return TRUE;
    }

    dst_bits = surface->funcs->get_info(surface, bmi);

    if (!(dib = CreateDIBSection(info->hdcDst, bmi, DIB_RGB_COLORS, &src_bits, NULL, 0))) goto done;
    if (!(hdc = CreateCompatibleDC(0))) goto done;

    SelectObject(hdc, dib);

    surface->funcs->lock(surface);

    if (info->prcDirty)
    {
        IntersectRect(&rect, &rect, info->prcDirty);
        memcpy(src_bits, dst_bits, bmi->bmiHeader.biSizeImage);
        PatBlt(hdc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, BLACKNESS);
    }
    src_rect = rect;
    if (info->pptSrc) OffsetRect(&src_rect, info->pptSrc->x, info->pptSrc->y);
    DPtoLP(info->hdcSrc, (POINT *)&src_rect, 2);

    ret = GdiAlphaBlend(hdc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                         info->hdcSrc, src_rect.left, src_rect.top,
                         src_rect.right - src_rect.left, src_rect.bottom - src_rect.top,
                         (info->dwFlags & ULW_ALPHA) ? *info->pblend : blend);
    if (ret)
    {
        memcpy(dst_bits, src_bits, bmi->bmiHeader.biSizeImage);
        add_bounds_rect(surface->funcs->get_bounds(surface), &rect);
    }

    surface->funcs->unlock(surface);
    surface->funcs->flush(surface);

done:
    window_surface_release(surface);
    if (hdc) DeleteDC(hdc);
    if (dib) DeleteObject(dib);
    return ret;
}

static LRESULT handle_wm_wayland_configure(HWND hwnd)
{
    struct wayland_win_data *data;
    struct wayland_surface *wsurface;
    DWORD flags;
    int width, height, wine_width, wine_height;
    BOOL needs_move_to_origin;
    int origin_x, origin_y;
    UINT swp_flags;
    BOOL needs_restore = FALSE;
    BOOL needs_enter_size_move = FALSE;
    BOOL needs_exit_size_move = FALSE;

    if (!(data = get_win_data(hwnd))) return 0;
    if (!data->wayland_surface || !data->wayland_surface->xdg_toplevel)
    {
        TRACE("no suitable wayland surface, returning\n");
        release_win_data(data);
        return 0;
    }

    wsurface = data->wayland_surface;

    TRACE("serial=%d size=%dx%d flags=%#x restore_rect=%s\n",
          wsurface->pending.serial, wsurface->pending.width,
          wsurface->pending.height, wsurface->pending.configure_flags,
          wine_dbgstr_rect(&data->restore_rect));

    if (wsurface->pending.serial == 0)
    {
        TRACE("pending configure event already handled, returning\n");
        release_win_data(data);
        return 0;
    }

    if (IsIconic(hwnd) &&
        !(wsurface->pending.configure_flags & WAYLAND_CONFIGURE_FLAG_ACTIVATED))
    {
        data->deactivated_after_minimization = TRUE;
    }

    /* Restore a minimized window if it is activated. We only do this after we
     * have seen a deactivation event for the surface or if the flags indicate
     * an explicit non-minimized state, to ensure we don't restore prematurely,
     * e.g., while minimization is still in progress. */
    if (IsIconic(hwnd) &&
        (wsurface->pending.configure_flags & WAYLAND_CONFIGURE_FLAG_ACTIVATED) &&
        (data->deactivated_after_minimization ||
         (wsurface->pending.configure_flags &
          (WAYLAND_CONFIGURE_FLAG_MAXIMIZED|WAYLAND_CONFIGURE_FLAG_FULLSCREEN))))
    {
        needs_restore = TRUE;
        data->deactivated_after_minimization = FALSE;
        /* If allowed, ignore the restoration size provided by wayland, since
         * it may cause wayland_win_data.restore_rect to be set and mess up our
         * own tracking.
         */
        if (!(wsurface->pending.configure_flags &
              (WAYLAND_CONFIGURE_FLAG_MAXIMIZED|WAYLAND_CONFIGURE_FLAG_FULLSCREEN)))
        {
            wsurface->pending.width = 0;
            wsurface->pending.height = 0;
        }
    }

    wsurface->pending.processed = TRUE;

    data->wayland_configure_event_flags = wsurface->pending.configure_flags;

    width = wsurface->pending.width;
    height = wsurface->pending.height;
    flags = wsurface->pending.configure_flags;

    /* If we are free to set our size, first try the restore size, then
     * the current size. */
    if (width == 0)
    {
        int ignore;
        if (!IsIconic(hwnd) || needs_restore)
            width = data->restore_rect.right - data->restore_rect.left;
        if (width == 0)
            width = data->window_rect.right - data->window_rect.left;
        wayland_surface_coords_rounded_from_wine(wsurface, width, 0,
                                                 &width, &ignore);
        wsurface->pending.width = width;
    }
    if (height == 0)
    {
        int ignore;
        if (!IsIconic(hwnd) || needs_restore)
            height = data->restore_rect.bottom - data->restore_rect.top;
        if (height == 0)
            height = data->window_rect.bottom - data->window_rect.top;
        wayland_surface_coords_rounded_from_wine(wsurface, 0, height,
                                                 &ignore, &height);
        wsurface->pending.height = height;
    }

    if (flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN)
        wayland_surface_find_wine_fullscreen_fit(wsurface, width, height,
                                                 &wine_width, &wine_height);
    else
        wayland_surface_coords_to_wine(wsurface, width, height,
                                       &wine_width, &wine_height);

    TRACE("hwnd=%p effective_size=%dx%d wine_size=%dx%d\n",
          data->hwnd, width, height, wine_width, wine_height);

    if ((flags & WAYLAND_CONFIGURE_FLAG_RESIZING) && !data->resizing)
    {
        data->resizing = TRUE;
        needs_enter_size_move = TRUE;
    }

    if (!(flags & WAYLAND_CONFIGURE_FLAG_RESIZING) && data->resizing)
    {
        data->resizing = FALSE;
        needs_exit_size_move = TRUE;
    }

    /* Parts of the window that are outside the win32 display are not
     * accessible to mouse events, although they may be visible and accessible
     * to the user from a wayland compositor pespective. To mitigate this, we
     * place all top-level windows at 0,0, to maximize the area that can reside
     * within the win32 display. */
    if (data->wayland_surface->main_output)
    {
        origin_x = data->wayland_surface->main_output->x;
        origin_y = data->wayland_surface->main_output->y;
        needs_move_to_origin = data->window_rect.top != origin_x ||
                               data->window_rect.left != origin_y;
        TRACE("current=%d,%d origin=%d,%d\n",
              data->window_rect.left, data->window_rect.top,
              origin_x, origin_y);
    }
    else
    {
        origin_x = 0;
        origin_y = 0;
        needs_move_to_origin = FALSE;
    }

    release_win_data(data);

    if (needs_restore)
    {
        ShowOwnedPopups(hwnd, TRUE);
        ShowWindow(hwnd, SW_RESTORE);
    }

    if (needs_enter_size_move)
        SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);

    if (needs_exit_size_move)
        SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);

    if ((data = get_win_data(hwnd)))
    {
        data->handling_wayland_configure_event = TRUE;
        release_win_data(data);
    }

    if (flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED)
        SetWindowLongW(hwnd, GWL_STYLE, GetWindowLongW(hwnd, GWL_STYLE) | WS_MAXIMIZE);
    else
        SetWindowLongW(hwnd, GWL_STYLE, GetWindowLongW(hwnd, GWL_STYLE) & ~WS_MAXIMIZE);

    swp_flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER;

    if (!needs_move_to_origin) swp_flags |= SWP_NOMOVE;
    if (wine_width > 0 && wine_height > 0)
        swp_flags |= SWP_FRAMECHANGED;
    else
        swp_flags |= SWP_NOSIZE | SWP_NOREDRAW;
    /* When we are maximized or fullscreen, wayland is particular about the
     * surface size it accepts, so don't allow the app to change it. */
    if (flags & (WAYLAND_CONFIGURE_FLAG_MAXIMIZED|WAYLAND_CONFIGURE_FLAG_FULLSCREEN))
        swp_flags |= SWP_NOSENDCHANGING;

    SetWindowPos(hwnd, 0, origin_x, origin_y, wine_width, wine_height, swp_flags);

    if ((data = get_win_data(hwnd)))
    {
        data->handling_wayland_configure_event = FALSE;
        release_win_data(data);
    }

    return 0;
}

static void handle_wm_wayland_surface_output_change(HWND hwnd)
{
    struct wayland_surface *wsurface;
    int x, y, w, h;
    UINT swp_flags;

    TRACE("hwnd=%p\n", hwnd);

    wsurface = wayland_surface_for_hwnd_lock(hwnd);
    if (!wsurface || !wsurface->xdg_toplevel)
    {
        TRACE("no suitable wayland surface, returning\n");
        goto out;
    }

    /* When becoming fullscreen (particularly on a different output), we may
     * get some confusing enter/leave events from the compositor. Ignore these
     * events and rely on update_wayland_state() to set the output, based on our
     * wine coordinates.
     */
    if (wsurface->pending.serial &&
        (wsurface->pending.configure_flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN))
    {
        TRACE("in the middle of a fullscreen configuration, "
              "ignoring output change notification\n");
        goto out;
    }

    swp_flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                SWP_FRAMECHANGED | SWP_NOSENDCHANGING;

    if (wsurface->main_output)
    {
        x = wsurface->main_output->x;
        y = wsurface->main_output->y;
        TRACE("moving window to %d,%d\n", x, y);
    }
    else
    {
        x = y = 0;
        swp_flags |= SWP_NOMOVE;
    }

    /* If we are fullscreen or maximized we need to provide a particular buffer
     * size to the wayland compositor on the new output (hence swp_flags
     * includes SWP_NOSENDCHANGING). */
    if (wsurface->current.serial &&
        (wsurface->current.configure_flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED))
    {
        wayland_surface_coords_to_wine(wsurface, wsurface->current.width,
                                       wsurface->current.height,
                                       &w, &h);
        TRACE("resizing window to maximized %dx%d\n", w, h);
    }
    else if (wsurface->current.serial &&
             (wsurface->current.configure_flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN))
    {
        wayland_surface_find_wine_fullscreen_fit(wsurface, wsurface->current.width,
                                                 wsurface->current.height,
                                                 &w, &h);
        TRACE("resizing window to fullscreen %dx%d\n", w, h);
    }
    else
    {
        w = h = 0;
        swp_flags |= SWP_NOSIZE;
    }

    SetWindowPos(hwnd, 0, x, y, w, h, swp_flags);

out:
    wayland_surface_for_hwnd_unlock(wsurface);
}

static void CALLBACK post_configure(HWND hwnd, UINT msg, UINT_PTR timer_id, DWORD elapsed)
{
    TRACE("hwnd=%p\n", hwnd);
    KillTimer(hwnd, timer_id);
    handle_wm_wayland_configure(hwnd);
}

/**********************************************************************
 *           WAYLAND_WindowMessage
 */
LRESULT CDECL WAYLAND_WindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TRACE("msg %x hwnd %p wp %lx lp %lx\n", msg, hwnd, wp, lp);

    switch (msg)
    {
    case WM_WAYLAND_CONFIGURE:
        /* While resizing, configure events can come continuously and due to the
         * amount of other message their handling produces (e.g., paints), have
         * the potential to keep the message loop busy for some time. This may
         * lead Wine core to think that the app never goes idle (see
         * win.c:flush_window_surfaces), and thus start flushing at unfortunate
         * times (e.g., in between partial window paints), causing visual
         * artifacts.
         *
         * To mitigate this we handle the configure message only if the message
         * queue is empty, ensuring that the loop has had time to become idle.
         * If the queue is not currently empty, we schedule a timer message,
         * which due to having the lowest priority is guaranteed to be triggered
         * only on otherwise empty queues.
         */
        if (!GetQueueStatus(QS_ALLINPUT))
        {
            return handle_wm_wayland_configure(hwnd);
        }
        else
        {
            struct wayland_surface *wayland_surface = wayland_surface_for_hwnd_lock(hwnd);
            if (wayland_surface && wayland_surface->xdg_toplevel)
            {
                SetTimer(hwnd, (UINT_PTR)wayland_surface->wl_surface, 10,
                         post_configure);
            }
            wayland_surface_for_hwnd_unlock(wayland_surface);
        }
        break;
    case WM_WAYLAND_MODE_CHANGE:
        wayland_change_wine_mode(thread_wayland(), wp, LOWORD(lp), HIWORD(lp));
        break;
    case WM_WAYLAND_POINTER_CONFINEMENT_UPDATE:
        {
            struct wayland_surface *wayland_surface = wayland_surface_for_hwnd_lock(hwnd);
            if (wayland_surface)
            {
                if (wp == WAYLAND_POINTER_CONFINEMENT_SYSTEM_CLIP)
                {
                    GetClipCursor(&wayland_surface->wayland->cursor_clip);
                }
                else if (wp == WAYLAND_POINTER_CONFINEMENT_UNSET_CLIP)
                {
                    SetRect(&wayland_surface->wayland->cursor_clip,
                            INT_MIN, INT_MIN, INT_MAX, INT_MAX);
                }
                wayland_surface_update_pointer_confinement(wayland_surface);
            }
            wayland_surface_for_hwnd_unlock(wayland_surface);
        }
        break;
    case WM_WAYLAND_SURFACE_OUTPUT_CHANGE:
        handle_wm_wayland_surface_output_change(hwnd);
        break;
    case WM_WAYLAND_CLIPBOARD_WINDOW_CREATE:
        wayland_data_device_ensure_clipboard_window(thread_wayland());
        break;
    case WM_WAYLAND_STATE_UPDATE:
        {
            struct wayland_win_data *data = get_win_data(hwnd);
            if (data)
            {
                data->pending_state_update_message = FALSE;
                update_wayland_state(data);
                release_win_data(data);
            }
        }
        break;
    default:
        FIXME("got window msg %x hwnd %p wp %lx lp %lx\n", msg, hwnd, wp, lp);
    }

    return 0;
}
