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
    /* owner hwnd for owned windows */
    HWND           owner;
    /* whether the owner is an internal guess */
    BOOL           owner_is_guess;
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

static HWND guess_popup_owner(struct wayland *wayland)
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

    /* If we have a recent mouse event, the popup owner is likely the window
     * under the cursor, so prefer it. Otherwise prefer the window with
     * the keyboard focus. */
    if (wayland->last_event_type == INPUT_MOUSE)
        return cursor_hwnd ? cursor_hwnd : keyboard_hwnd;
    else
        return keyboard_hwnd ? keyboard_hwnd : cursor_hwnd;
}

/***********************************************************************
 *           alloc_win_data
 */
static struct wayland_win_data *alloc_win_data(HWND hwnd)
{
    struct wayland_win_data *data;
    DWORD style = GetWindowLongW(hwnd, GWL_STYLE);

    if (!(data = heap_alloc_zero(sizeof(*data))))
        return NULL;

    data->hwnd = hwnd;

    /* Don't create native wayland surfaces for child windows. Let Wine core
     * handle their drawing in their corresponding top level window surface. */
    if (!(style & WS_CHILD))
    {
        struct wayland *wayland = thread_init_wayland();
        HWND owner_hwnd = GetWindow(hwnd, GW_OWNER);
        struct wayland_surface *owner_surface = NULL;

        /* Many applications use top level, unowned popup windows for menus and
         * tooltips and depend on screen coordinates for correct positioning. Since
         * wayland can't deal with screen coordinates, try to guess the owner
         * window of such popups and manage them as wayland subsurfaces. */
        if (!owner_hwnd && (style & WS_POPUP) && !(style & WS_CAPTION))
        {
            owner_hwnd = guess_popup_owner(wayland);
            data->owner_is_guess = TRUE;
        }

        TRACE("owner=%p\n", owner_hwnd);

        if (owner_hwnd && owner_hwnd != GetDesktopWindow())
            owner_surface = wayland_surface_for_hwnd(owner_hwnd);

        data->owner = owner_hwnd;

        /* Use wayland subsurfaces for owned win32 windows that are transient (i.e., don't have
         * a titlebar). Otherwise make them wayland toplevels. */
        if (!(style & WS_CAPTION) && owner_surface)
            data->wayland_surface = wayland_surface_create_subsurface(wayland, owner_surface);
        else
            data->wayland_surface = wayland_surface_create_toplevel(wayland, owner_surface);

        data->wayland_surface->hwnd = hwnd;
    }

    EnterCriticalSection(&win_data_section);
    win_data_context[context_idx(hwnd)] = data;

    TRACE("hwnd=%p wayland_surface=%p\n", data->hwnd, data->wayland_surface);

    return data;
}

/***********************************************************************
 *           free_win_data
 */
static void free_win_data(struct wayland_win_data *data)
{
    TRACE("hwnd=%p\n", data->hwnd);
    win_data_context[context_idx(data->hwnd)] = NULL;
    LeaveCriticalSection(&win_data_section);
    if (data->wayland_surface)
        wayland_surface_destroy(data->wayland_surface);
    heap_free(data);
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
 *           wayland_surface_for_hwnd
 */
struct wayland_surface *wayland_surface_for_hwnd(HWND hwnd)
{
    struct wayland_surface *ret;
    struct wayland_win_data *data = get_win_data(hwnd);

    ret = !data ? NULL : data->wayland_surface;
    release_win_data(data);

    return ret;
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
    COLORREF              color_key;
    void                 *bits;
    CRITICAL_SECTION      crit;
    BITMAPINFO            info;   /* variable size, must be last */
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
    reset_bounds(&surface->bounds);
    if (!needs_flush) goto done;

    TRACE("flushing surface %p hwnd %p surface_rect %s bits %p alpha %02x key %08x compression %d region %p\n",
           surface, surface->hwnd, wine_dbgstr_rect(&surface->header.rect),
           surface->bits, surface->alpha, surface->color_key,
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
    buffer_damage = wayland_shm_buffer_get_damage(buffer);

    rgn_rect = (RECT *)buffer_damage->Buffer;
    rgn_rect_end = rgn_rect + buffer_damage->rdh.nCount;

    /* Flush damaged buffer region from window_surface bitmap to wayland SHM buffer. */
    for (;rgn_rect < rgn_rect_end; rgn_rect++)
    {
        unsigned int *src, *dst;
        int x, y, width, height;

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

        /* Fast path for full width rectangles. */
        if (width == buffer->width &&
            surface->alpha == 255 &&
            surface->color_key == CLR_INVALID)
        {
            memcpy(dst, src, height * buffer->stride);
            continue;
        }

        for (y = 0; y < height; y++)
        {
            if (surface->alpha == 255)
            {
                memcpy(dst, src, width * 4);
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

static BOOL is_wayland_rgba_window_surface(struct window_surface *surface)
{
    return surface && surface->funcs == &wayland_window_surface_funcs &&
        get_wayland_window_surface(surface)->wayland_buffer_queue->format ==
            WL_SHM_FORMAT_ARGB8888;
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
 *           set_surface_region
 */
static void set_surface_region(struct window_surface *window_surface, HRGN win_region)
{
    struct wayland_window_surface *surface;
    struct wayland_win_data *win_data;
    HRGN region = win_region;

    if (!is_wayland_window_surface(window_surface)) return;

    surface = get_wayland_window_surface(window_surface);

    TRACE("hwnd %p surface %p region %p\n", surface->hwnd, surface, win_region);

    if (win_region == (HRGN)1)  /* hack: win_region == 1 means retrieve region from server */
    {
        if (!(win_data = get_win_data(surface->hwnd))) return;
        region = CreateRectRgn(0, 0, win_data->window_rect.right - win_data->window_rect.left,
                               win_data->window_rect.bottom - win_data->window_rect.top);
        release_win_data(win_data);
        if (GetWindowRgn(surface->hwnd, region) == ERROR && !surface->region)
        {
            DeleteObject(region);
            region = 0;
            goto done;
        }
    }

    /* If we have a region set through the window_surface funcs, take it into account */
    if (surface->region) CombineRgn(region, region, surface->region, RGN_AND);

done:
    window_surface->funcs->lock(window_surface);
    if (surface->total_region) DeleteObject(surface->total_region);
    surface->total_region = region;
    *window_surface->funcs->get_bounds(window_surface) = surface->header.rect;
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
    surface->wayland_surface = wayland_surface_for_hwnd(hwnd);
    surface->wayland_buffer_queue =
        surface->wayland_surface ?
        wayland_buffer_queue_create(surface->wayland_surface->wayland, width, height,
                (src_alpha || alpha != 255) ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888) :
        NULL;
    surface->alpha        = alpha;
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
    set_color_key(surface, color_key);
    if (alpha != prev_alpha || surface->color_key != prev_key)  /* refresh */
        *window_surface->funcs->get_bounds(window_surface) = surface->header.rect;
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
        struct wayland_win_data *data;

        if (!(data = alloc_win_data(hwnd))) return FALSE;
        release_win_data(data);
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
    if (data->surface) window_surface_release(data->surface);
    data->surface = NULL;
    free_win_data(data);
}

/***********************************************************************
 *           create_win_data
 *
 * Create a data window structure for an existing window.
 */
static struct wayland_win_data *create_win_data(HWND hwnd, const RECT *window_rect,
                                                 const RECT *client_rect)
{
    struct wayland_win_data *data;
    HWND parent;

    /* Don't create win_data for desktop or HWND_MESSAGE windows. */
    if (!(parent = GetAncestor(hwnd, GA_PARENT))) return NULL;
    if (parent != GetDesktopWindow() && !GetAncestor(parent, GA_PARENT)) return NULL;

    if (!(data = alloc_win_data(hwnd))) return NULL;

    data->parent = (parent == GetDesktopWindow()) ? 0 : parent;
    data->window_rect = *window_rect;
    data->client_rect = *client_rect;
    return data;
}

/***********************************************************************
 *           WAYLAND_WindowPosChanging
 */
void CDECL WAYLAND_WindowPosChanging(HWND hwnd, HWND insert_after, UINT swp_flags,
                                     const RECT *window_rect, const RECT *client_rect, RECT *visible_rect,
                                     struct window_surface **surface)
{
    struct wayland_win_data *data = get_win_data(hwnd);
    RECT surface_rect;
    DWORD flags;
    COLORREF key;
    BYTE alpha;
    BOOL layered = GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED;
    HWND owner = GetWindow(hwnd, GW_OWNER);

    TRACE("win %p window %s client %s visible %s style %08x flags %08x layered %d after %p\n",
           hwnd, wine_dbgstr_rect(window_rect), wine_dbgstr_rect(client_rect),
           wine_dbgstr_rect(visible_rect),
           GetWindowLongW(hwnd, GWL_STYLE), swp_flags , layered, insert_after);

    if (!data && !(data = create_win_data(hwnd, window_rect, client_rect))) return;

    /* Change of ownership requires recreating the whole win_data to ensure
     * we have a properly owned wayland surface. If we get a null owner we
     * only apply it if the current owner is not an internal guess. */
    if ((owner || !data->owner_is_guess) && data->owner != owner)
    {
        EnterCriticalSection(&win_data_section);
        free_win_data(data);
        data = create_win_data(hwnd, window_rect, client_rect);
        LeaveCriticalSection(&win_data_section);
    }

    *visible_rect = *window_rect;

    /* create the window surface if necessary */
    if (*surface) window_surface_release(*surface);
    *surface = NULL;  /* indicate that we want to draw directly to the window */

    /* If we don't want a dedicated surface for this window... */
    if (!data->wayland_surface) goto done;

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
    if (!layered || !GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags)) flags = 0;
    if (!(flags & LWA_ALPHA)) alpha = 255;
    if (!(flags & LWA_COLORKEY)) key = CLR_INVALID;

    *surface = create_surface(data->hwnd, &surface_rect, alpha, key, FALSE);

    TRACE("new surface %p\n", *surface);
done:
    release_win_data(data);
}

static void update_wayland_state(struct wayland_win_data *data, DWORD style,
                                 const RECT *old_window_rect)
{
    RECT offset_rect = {0};
    MONITORINFO mi;
    int width = data->window_rect.right - data->window_rect.left;
    int height = data->window_rect.bottom - data->window_rect.top;
    int monitor_width;
    int monitor_height;
    struct wayland_win_data *parent_data;
    BOOL wait_for_configure = FALSE;
    enum wayland_configure_flags conf_flags = 0;

    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(data->hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    monitor_width = mi.rcMonitor.right - mi.rcMonitor.left;
    monitor_height = mi.rcMonitor.bottom - mi.rcMonitor.top;

    TRACE("hwnd=%p window=%dx%d monitor=%dx%d maximized=%d fullscreen=%d handling_event=%d\n",
          data->hwnd, width, height, monitor_width, monitor_height,
          data->maximized, data->fullscreen, data->handling_wayland_configure_event);

    if (!(style & WS_VISIBLE))
    {
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
        {
            xdg_toplevel_unset_maximized(data->wayland_surface->xdg_toplevel);
            wait_for_configure = TRUE;
        }
        data->maximized = FALSE;
    }

    if (data->fullscreen && !(conf_flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN))
    {
        if (!data->handling_wayland_configure_event)
        {
            xdg_toplevel_unset_fullscreen(data->wayland_surface->xdg_toplevel);
            wait_for_configure = TRUE;
        }
        data->fullscreen = FALSE;
    }

    if (!data->maximized && (conf_flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED))
    {
        if (!data->handling_wayland_configure_event)
        {
            xdg_toplevel_set_maximized(data->wayland_surface->xdg_toplevel);
            wait_for_configure = TRUE;
        }
        data->maximized = TRUE;
    }

   /* Set the fullscreen state after the maximized state on the wayland surface
    * to ensure compositors apply the final fullscreen state properly. */
    if (!data->fullscreen && (conf_flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN))
    {
        if (!data->handling_wayland_configure_event)
        {
            xdg_toplevel_set_fullscreen(data->wayland_surface->xdg_toplevel, NULL);
            wait_for_configure = TRUE;
        }
        data->fullscreen = TRUE;
    }

    if (!(conf_flags & (WAYLAND_CONFIGURE_FLAG_FULLSCREEN|WAYLAND_CONFIGURE_FLAG_MAXIMIZED)))
        data->restore_rect = data->window_rect;

    TRACE("hwnd=%p current state maximized=%d fullscreen=%d\n",
          data->hwnd, data->maximized, data->fullscreen);

    /* If have just requested a state change, wayland hasn't yet replied with a
     * configure for the new state (and thus we haven't acked it), so all
     * current maximize/fullscreen limitations still apply. To avoid wayland
     * compositors erroring out on us, don't reconfigure the surfaces yet. We
     * will reconfigure them when we get the configure event. */
    if (wait_for_configure)
    {
        /* Reset any pending configure serial to avoid handling events older than
         * the ones we expect to get from the new state. */
        data->wayland_surface->pending.serial = 0;
        wl_display_flush(data->wayland_surface->wayland->wl_display);
        TRACE("hwnd=%p waiting for configure for state maximized=%d fullscreen=%d\n",
              data->hwnd, data->maximized, data->fullscreen);
        return;
    }

    parent_data = get_win_data((HWND)GetWindowLongPtrW(data->hwnd, GWLP_HWNDPARENT));
    /* We manage some top level, popup window with subsurfaces (see alloc_win_data),
     * which use coordinates relative to their parent surface. In order to properly
     * handle the positioning of such windows, we treat the owner window as the
     * "parent" below. */
    if (!parent_data)
        parent_data = get_win_data(data->owner);

    if (parent_data)
    {
        if (style & WS_POPUP)
        {
            /* Popup windows are in screen coordinates */
            offset_rect = parent_data->window_rect;
            TRACE("popup parent hwnd=%p window %s client %s\n",
                  parent_data->hwnd,
                  wine_dbgstr_rect(&parent_data->window_rect),
                  wine_dbgstr_rect(&parent_data->client_rect));
            offset_rect.left = -offset_rect.left;
            offset_rect.top = -offset_rect.top;
        }
        else
        {
            /* Other windows are in client coordinates */
            TRACE("non-popup parent hwnd=%p window %s client %s\n",
                  parent_data->hwnd,
                  wine_dbgstr_rect(&parent_data->window_rect),
                  wine_dbgstr_rect(&parent_data->client_rect));
            offset_rect = parent_data->client_rect;
            offset_rect.left -= parent_data->window_rect.left;
            offset_rect.top -= parent_data->window_rect.top;
        }
    }

    wayland_surface_reconfigure(data->wayland_surface,
                                offset_rect.left + data->window_rect.left,
                                offset_rect.top + data->window_rect.top,
                                width, height);
    /* The GL subsurface (if any), is positioned over the client area of the window.
     * The position of the GL subsurface is relative to the window top-left. */
    wayland_surface_reconfigure_gl(data->wayland_surface,
                                   data->client_rect.left - data->window_rect.left,
                                   data->client_rect.top - data->window_rect.top,
                                   data->client_rect.right - data->client_rect.left,
                                   data->client_rect.bottom - data->client_rect.top);

    TRACE("conf->serial=%d conf->size=%dx%d conf->flags=%#x\n",
          data->wayland_surface->pending.serial,
          data->wayland_surface->pending.width,
          data->wayland_surface->pending.height,
          data->wayland_surface->pending.configure_flags);

    /* If we have a pending configure event, and it is compatible with the new state,
     * ack the event. */
    if (data->wayland_surface->pending.serial &&
        wayland_surface_configure_is_compatible(&data->wayland_surface->pending,
                                                width, height, conf_flags))
    {
        wayland_surface_ack_configure(data->wayland_surface);
    }

    release_win_data(parent_data);
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
    DWORD new_style = GetWindowLongW(hwnd, GWL_STYLE);
    RECT old_window_rect;

    if (!(data = get_win_data(hwnd))) return;

    TRACE("hwnd %p window %s client %s visible %s style %08x owner %p after %p flags %08x\n",
          hwnd, wine_dbgstr_rect(window_rect), wine_dbgstr_rect(client_rect),
          wine_dbgstr_rect(visible_rect), new_style, data->owner, insert_after, swp_flags);

    old_window_rect = data->window_rect;

    data->window_rect = *window_rect;
    data->client_rect = *client_rect;

    if (!is_wayland_rgba_window_surface(data->surface))
    {
        if (surface) window_surface_add_ref(surface);
        if (data->surface) window_surface_release(data->surface);
        data->surface = surface;
    }

    /* TODO: Try to handle z-order */

    if (data->wayland_surface)
        update_wayland_state(data, new_style, &old_window_rect);

    release_win_data(data);
}

/***********************************************************************
 *           WAYLAND_ShowWindow
 */
UINT CDECL WAYLAND_ShowWindow(HWND hwnd, INT cmd, RECT *rect, UINT swp)
{
    TRACE("hwnd=%p cmd=%d\n", hwnd, cmd);

    if (IsRectEmpty(rect)) return swp;
    if (!IsIconic(hwnd)) return swp;
    /* always hide icons off-screen */
    if (rect->left != -32000 || rect->top != -32000)
    {
        OffsetRect(rect, -32000 - rect->left, -32000 - rect->top);
        swp &= ~(SWP_NOMOVE | SWP_NOCLIENTMOVE);
    }

    return swp;
}

/*****************************************************************
 *	     WAYLAND_SetParent
 */
void CDECL WAYLAND_SetParent(HWND hwnd, HWND parent, HWND old_parent)
{
    struct wayland_win_data *data;

    TRACE("hwnd=%p old=%p new=%p\n", hwnd, old_parent, parent);

    if (parent == old_parent) return;
    if (!(data = get_win_data(hwnd))) return;

    data->parent = (parent == GetDesktopWindow()) ? 0 : parent;

    /* TODO: handle reparenting */

    release_win_data(data);
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
 *           WAYLAND_SetCursor
 */
void CDECL WAYLAND_SetCursor(HCURSOR handle)
{
    static HCURSOR last_cursor;
    static DWORD last_cursor_change;
    struct wayland *wayland;

    if (InterlockedExchangePointer((void **)&last_cursor, handle) == handle ||
        GetTickCount() - last_cursor_change < 100)
    {
        return;
    }

    wayland = thread_wayland();

    TRACE("hcursor=%p\n", handle);

    wayland_pointer_update_cursor_from_win32(&wayland->pointer, handle);
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
        if (is_wayland_window_surface(data->surface))
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
    if (!is_wayland_rgba_window_surface(surface))
    {
        if (surface) window_surface_release(surface);
        surface = NULL;
    }

    if (!surface || !EqualRect(&surface->rect, &rect))
    {
        data->surface = create_surface(data->hwnd, &rect, 255, color_key, TRUE);
        if (surface) window_surface_release(surface);
        surface = data->surface;
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
    BOOL needs_move_to_zero;

    if (!(data = get_win_data(hwnd))) return 0;

    wsurface = data->wayland_surface;

    TRACE("serial=%d size=%dx%d flags=%#x\n",
          wsurface->pending.serial, wsurface->pending.width,
          wsurface->pending.height, wsurface->pending.configure_flags);

    if (wsurface->pending.serial == 0)
    {
        TRACE("pending configure event already handled, returning\n");
        release_win_data(data);
        return 0;
    }

    data->handling_wayland_configure_event = TRUE;
    data->wayland_configure_event_flags = wsurface->pending.configure_flags;

    width = wsurface->pending.width;
    height = wsurface->pending.height;
    flags = wsurface->pending.configure_flags;

    /* If we are free to set our size, first try the restore size, then
     * the current size. */
    if (width == 0)
    {
        width = data->restore_rect.right - data->restore_rect.left;
        if (width == 0)
            width = data->window_rect.right - data->window_rect.left;
        wsurface->pending.width = width;
    }
    if (height == 0)
    {
        height = data->restore_rect.bottom - data->restore_rect.top;
        if (height == 0)
            height = data->window_rect.bottom - data->window_rect.top;
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
        SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    }

    if (!(flags & WAYLAND_CONFIGURE_FLAG_RESIZING) && data->resizing)
    {
        data->resizing = FALSE;
        SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
    }

    /* Parts of the window that are outside the win32 display are not
     * accessible to mouse events, although they may be visible and accessible
     * to the user from a wayland compositor pespective. To mitigate this, we
     * place all top-level windows at 0,0, to maximize the area that can reside
     * within the win32 display. */
    needs_move_to_zero = data->window_rect.top != 0 || data->window_rect.left != 0;

    release_win_data(data);

    if (flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED)
        SetWindowLongW(hwnd, GWL_STYLE, GetWindowLongW(hwnd, GWL_STYLE) | WS_MAXIMIZE);
    else
        SetWindowLongW(hwnd, GWL_STYLE, GetWindowLongW(hwnd, GWL_STYLE) & ~WS_MAXIMIZE);

    if (wine_width > 0 && wine_height > 0)
    {
        UINT swp_flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                         SWP_FRAMECHANGED | SWP_NOMOVE;
        if (needs_move_to_zero) swp_flags &= ~SWP_NOMOVE;
        /* When we are maximized or fullscreen, wayland is particular about the
         * surface size it accepts, so don't allow the app to change it. */
        if (flags & (WAYLAND_CONFIGURE_FLAG_MAXIMIZED|WAYLAND_CONFIGURE_FLAG_FULLSCREEN))
            swp_flags |= SWP_NOSENDCHANGING;
        SetWindowPos(hwnd, 0, 0, 0, wine_width, wine_height, swp_flags);
    }
    else
    {
        wayland_surface_ack_configure(wsurface);
        if (needs_move_to_zero)
        {
            SetWindowPos(hwnd, 0, 0, 0, 0, 0,
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                         SWP_NOSIZE | SWP_NOREDRAW);
        }
    }

    data->handling_wayland_configure_event = FALSE;

    return 0;
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
            struct wayland_win_data *data;
            if (!(data = get_win_data(hwnd))) return 0;
            SetTimer(hwnd, (UINT_PTR)data->wayland_surface->wl_surface, 10, post_configure);
            release_win_data(data);
        }
        break;
    case WM_WAYLAND_MODE_CHANGE:
        wayland_change_wine_mode(thread_wayland(), wp, LOWORD(lp), HIWORD(lp));
        break;
    default:
        FIXME("got window msg %x hwnd %p wp %lx lp %lx\n", msg, hwnd, wp, lp);
    }

    return 0;
}
