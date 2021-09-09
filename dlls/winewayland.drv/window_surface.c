/*
 * Wayland window surface implementation
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

#include "config.h"

#include "waylanddrv.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>

#include "winuser.h"

#include "wine/debug.h"
#include "wine/heap.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

struct wayland_window_surface
{
    struct window_surface header;
    HWND                  hwnd;
    struct wayland_surface *wayland_surface; /* Not owned by us */
    struct wayland_buffer_queue *wayland_buffer_queue;
    RECT                  bounds;
    HRGN                  region; /* region set through window_surface funcs */
    HRGN                  total_region; /* Total region (surface->region AND window_region) */
    COLORREF              color_key;
    BYTE                  alpha;
    void                 *bits;
    CRITICAL_SECTION      crit;
    BOOL                  last_flush_failed;
    BITMAPINFO            info;
};

static struct wayland_window_surface *wayland_window_surface_cast(
    struct window_surface *window_surface)
{
    return (struct wayland_window_surface *)window_surface;
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

static inline void reset_bounds(RECT *bounds)
{
    bounds->left = bounds->top = INT_MAX;
    bounds->right = bounds->bottom = INT_MIN;
}

/***********************************************************************
 *           wayland_window_surface_preferred_format
 */
static int get_preferred_format(struct wayland_window_surface *wws)
{
    int format;
    HRGN window_region = CreateRectRgn(0, 0, 0, 0);

    /* Use ARGB to implement window regions (areas out of the region are
     * transparent). */
    if ((window_region && GetWindowRgn(wws->hwnd, window_region) != ERROR) ||
        wws->color_key != CLR_INVALID || wws->alpha != 255)
        format = WL_SHM_FORMAT_ARGB8888;
    else
        format = WL_SHM_FORMAT_XRGB8888;

    if (window_region) DeleteObject(window_region);

    return format;
}

/***********************************************************************
 *           recreate_wayland_buffer_queue
 */
static void recreate_wayland_buffer_queue(struct wayland_window_surface *wws)
{
    int width;
    int height;
    int format;

    if (!wws->wayland_buffer_queue || !wws->wayland_surface) return;

    width = wws->wayland_buffer_queue->width;
    height = wws->wayland_buffer_queue->height;
    format = get_preferred_format(wws);

    wayland_buffer_queue_destroy(wws->wayland_buffer_queue);

    wws->wayland_buffer_queue =
        wayland_buffer_queue_create(wws->wayland_surface->wayland,
                                    width, height, format);
}

/***********************************************************************
 *           wayland_window_surface_set_window_region
 */
void wayland_window_surface_set_window_region(struct window_surface *window_surface,
                                              HRGN win_region)
{
    struct wayland_window_surface *wws =
        wayland_window_surface_cast(window_surface);
    HRGN region = 0;

    TRACE("hwnd %p surface %p region %p\n", wws->hwnd, wws, win_region);

    if (win_region == (HRGN)1)  /* hack: win_region == 1 means retrieve region from server */
    {
        region = CreateRectRgn(0, 0, 0, 0);
        if (region && GetWindowRgn(wws->hwnd, region) == ERROR)
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

    if (wws->region)
    {
        if (region)
        {
            CombineRgn(region, region, wws->region, RGN_AND);
        }
        else
        {
            region = CreateRectRgn(0, 0, 0, 0);
            if (region) CombineRgn(region, wws->region, 0, RGN_COPY);
        }
    }

    window_surface->funcs->lock(window_surface);

    if (wws->total_region) DeleteObject(wws->total_region);
    wws->total_region = region;
    *window_surface->funcs->get_bounds(window_surface) = wws->header.rect;
    /* Unconditionally recreate the buffer queue to ensure we have clean buffers, so
     * that areas outside the region are transparent. */
    recreate_wayland_buffer_queue(wws);

    TRACE("hwnd %p bounds %s rect %s\n", wws->hwnd,
          wine_dbgstr_rect(window_surface->funcs->get_bounds(window_surface)),
          wine_dbgstr_rect(&wws->header.rect));

    window_surface->funcs->unlock(window_surface);
}

/***********************************************************************
 *           wayland_window_surface_lock
 */
static void CDECL wayland_window_surface_lock(struct window_surface *window_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);
    EnterCriticalSection(&wws->crit);
}

/***********************************************************************
 *           wayland_window_surface_unlock
 */
static void CDECL wayland_window_surface_unlock(struct window_surface *window_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);
    LeaveCriticalSection(&wws->crit);
}

/***********************************************************************
 *           wayland_window_surface_get_bitmap_info
 */
static void CDECL *wayland_window_surface_get_bitmap_info(struct window_surface *window_surface,
                                                          BITMAPINFO *info)
{
    struct wayland_window_surface *surface = wayland_window_surface_cast(window_surface);
    /* We don't store any additional information at the end of our BITMAPINFO, so
     * just copy the structure itself. */
    memcpy(info, &surface->info, sizeof(*info));
    return surface->bits;
}

/***********************************************************************
 *           wayland_window_surface_get_bounds
 */
static RECT CDECL *wayland_window_surface_get_bounds(struct window_surface *window_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);
    return &wws->bounds;
}

/***********************************************************************
 *           wayland_window_surface_set_region
 */
static void CDECL wayland_window_surface_set_region(struct window_surface *window_surface,
                                                    HRGN region)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);

    TRACE("updating hwnd=%p surface=%p region=%p\n", wws->hwnd, wws, region);

    window_surface->funcs->lock(window_surface);
    if (!region)
    {
        if (wws->region) DeleteObject(wws->region);
        wws->region = NULL;
    }
    else
    {
        if (!wws->region) wws->region = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(wws->region, region, 0, RGN_COPY);
    }
    window_surface->funcs->unlock(window_surface);
    wayland_window_surface_set_window_region(&wws->header, (HRGN)1);
}

/***********************************************************************
 *           wayland_window_surface_flush
 */
void CDECL wayland_window_surface_flush(struct window_surface *window_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);
    struct wayland_shm_buffer *buffer;
    RECT damage_rect;
    BOOL needs_flush;
    RGNDATA *buffer_damage;
    HRGN surface_damage_region = NULL;
    RECT *rgn_rect;
    RECT *rgn_rect_end;

    window_surface->funcs->lock(window_surface);

    TRACE("hwnd=%p surface_rect=%s bounds=%s\n", wws->hwnd,
          wine_dbgstr_rect(&wws->header.rect), wine_dbgstr_rect(&wws->bounds));

    needs_flush = IntersectRect(&damage_rect, &wws->header.rect, &wws->bounds);
    if (needs_flush)
    {
        RECT total_region_box;
        surface_damage_region = CreateRectRgnIndirect(&damage_rect);
        /* If the total_region is empty we are guaranteed to have empty SHM
         * buffers. In order for this empty content to take effect, we still
         * need to commit with non-empty damage, so don't AND with the
         * total_region in this case, to ensure we don't end up with an empty
         * surface_damage_region. */
        if (wws->total_region &&
            GetRgnBox(wws->total_region, &total_region_box) != NULLREGION)
        {
            needs_flush = CombineRgn(surface_damage_region, surface_damage_region,
                                     wws->total_region, RGN_AND);
        }
    }

    if (needs_flush && (!wws->wayland_surface || !wws->wayland_buffer_queue))
    {
        TRACE("missing wayland surface=%p buffer_queue=%p, returning\n",
              wws->wayland_surface, wws->wayland_buffer_queue);
        wws->last_flush_failed = TRUE;
        goto done;
    }
    wws->last_flush_failed = FALSE;

    if (!needs_flush) goto done;

    TRACE("flushing surface %p hwnd %p surface_rect %s bits %p color_key %08x "
          "alpha %02x compression %d region %p\n",
          wws, wws->hwnd, wine_dbgstr_rect(&wws->header.rect),
          wws->bits, wws->color_key, wws->alpha,
          wws->info.bmiHeader.biCompression,
          wws->total_region);

    assert(wws->wayland_buffer_queue);

    wayland_buffer_queue_add_damage(wws->wayland_buffer_queue, surface_damage_region);
    buffer = wayland_buffer_queue_acquire_buffer(wws->wayland_buffer_queue);
    if (!buffer)
    {
        WARN("failed to acquire wayland buffer, returning\n");
        wws->last_flush_failed = TRUE;
        goto done;
    }
    buffer_damage = wayland_shm_buffer_get_damage_clipped(buffer, wws->total_region);

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

        src = (unsigned int *)wws->bits +
              rgn_rect->top * wws->info.bmiHeader.biWidth +
              rgn_rect->left;
        dst = (unsigned int *)((unsigned char *)buffer->map_data +
              rgn_rect->top * buffer->stride +
              rgn_rect->left * 4);
        width = min(rgn_rect->right, buffer->width) - rgn_rect->left;
        height = min(rgn_rect->bottom, buffer->height) - rgn_rect->top;

        /* If we have an ARGB buffer we need to explicitly apply the surface
         * alpha to ensure the destination has sensible alpha values. */
        apply_surface_alpha = buffer->format == WL_SHM_FORMAT_ARGB8888;

        /* Fast path for full width rectangles. */
        if (width == buffer->width && !apply_surface_alpha &&
            wws->color_key == CLR_INVALID)
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
            else if (wws->alpha == 255)
            {
                for (x = 0; x < width; x++)
                    dst[x] = 0xff000000 | src[x];
            }
            else
            {
                for (x = 0; x < width; x++)
                {
                    dst[x] = ((wws->alpha << 24) |
                              (((BYTE)(src[x] >> 16) * wws->alpha / 255) << 16) |
                              (((BYTE)(src[x] >> 8) * wws->alpha / 255) << 8) |
                              (((BYTE)src[x] * wws->alpha / 255)));
                }
            }

            if (wws->color_key != CLR_INVALID)
                for (x = 0; x < width; x++) if ((src[x] & 0xffffff) == wws->color_key) dst[x] = 0;

            src += wws->info.bmiHeader.biWidth;
            dst = (unsigned int*)((unsigned char*)dst + buffer->stride);
        }
    }

    if (!wayland_surface_commit_buffer(wws->wayland_surface, buffer,
                                       surface_damage_region))
    {
        wws->last_flush_failed = TRUE;
    }

    wayland_shm_buffer_clear_damage(buffer);

    heap_free(buffer_damage);

done:
    if (!wws->last_flush_failed) reset_bounds(&wws->bounds);
    if (surface_damage_region) DeleteObject(surface_damage_region);
    window_surface->funcs->unlock(window_surface);
}

/***********************************************************************
 *           wayland_window_surface_destroy
 */
static void CDECL wayland_window_surface_destroy(struct window_surface *window_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);

    TRACE("surface=%p\n", wws);

    wws->crit.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&wws->crit);
    if (wws->region) DeleteObject(wws->region);
    if (wws->total_region) DeleteObject(wws->total_region);
    if (wws->wayland_surface) wayland_surface_unref(wws->wayland_surface);
    if (wws->wayland_buffer_queue)
        wayland_buffer_queue_destroy(wws->wayland_buffer_queue);
    heap_free(wws->bits);
    heap_free(wws);
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

/***********************************************************************
 *           wayland_window_surface_create
 */
struct window_surface *wayland_window_surface_create(HWND hwnd, const RECT *rect,
                                                     COLORREF color_key, BYTE alpha)
{
    struct wayland_window_surface *wws;
    int width = rect->right - rect->left, height = rect->bottom - rect->top;

    TRACE("win %p rect %s\n", hwnd, wine_dbgstr_rect(rect));
    wws = heap_alloc_zero(sizeof(*wws));
    if (!wws) return NULL;
    wws->info.bmiHeader.biSize = sizeof(wws->info.bmiHeader);
    wws->info.bmiHeader.biClrUsed = 0;
    wws->info.bmiHeader.biBitCount = 32;
    wws->info.bmiHeader.biCompression = BI_RGB;
    wws->info.bmiHeader.biWidth       = width;
    wws->info.bmiHeader.biHeight      = -height; /* top-down */
    wws->info.bmiHeader.biPlanes      = 1;
    wws->info.bmiHeader.biSizeImage   = get_dib_image_size(&wws->info);

    InitializeCriticalSection(&wws->crit);
    wws->crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": wws");

    wws->header.funcs = &wayland_window_surface_funcs;
    wws->header.rect  = *rect;
    wws->header.ref   = 1;
    wws->hwnd         = hwnd;
    wws->color_key    = color_key;
    wws->alpha        = alpha;
    wayland_window_surface_set_window_region(&wws->header, (HRGN)1);
    reset_bounds(&wws->bounds);

    if (!(wws->bits = heap_alloc(wws->info.bmiHeader.biSizeImage)))
        goto failed;

    TRACE("created %p hwnd %p %s bits %p-%p compression %d\n", wws, hwnd, wine_dbgstr_rect(rect),
           wws->bits, (char *)wws->bits + wws->info.bmiHeader.biSizeImage,
           wws->info.bmiHeader.biCompression);

    return &wws->header;

failed:
    wayland_window_surface_destroy(&wws->header);
    return NULL;
}

/***********************************************************************
 *           wayland_window_surface_needs_flush
 */
BOOL wayland_window_surface_needs_flush(struct window_surface *window_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);
    return wws->last_flush_failed;
}

/***********************************************************************
 *           wayland_window_surface_update_wayland_surface
 */
void wayland_window_surface_update_wayland_surface(struct window_surface *window_surface,
                                                   struct wayland_surface *wayland_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);

    window_surface->funcs->lock(window_surface);

    if (wayland_surface) wayland_surface_ref(wayland_surface);
    if (wws->wayland_surface) wayland_surface_unref(wws->wayland_surface);
    wws->wayland_surface = wayland_surface;

    /* We only need a buffer queue if we have a surface to commit to. */
    if (wws->wayland_surface && !wws->wayland_buffer_queue)
    {
        wws->wayland_buffer_queue =
            wayland_buffer_queue_create(wws->wayland_surface->wayland,
                    wws->info.bmiHeader.biWidth, abs(wws->info.bmiHeader.biHeight),
                    get_preferred_format(wws));
    }
    else if (!wws->wayland_surface && wws->wayland_buffer_queue)
    {
        wayland_buffer_queue_destroy(wws->wayland_buffer_queue);
        wws->wayland_buffer_queue = NULL;
    }

    window_surface->funcs->unlock(window_surface);
}

/***********************************************************************
 *           wayland_window_surface_update_layered
 */
void wayland_window_surface_update_layered(struct window_surface *window_surface,
                                           COLORREF color_key, BYTE alpha)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);

    window_surface->funcs->lock(window_surface);

    if (alpha != wws->alpha || color_key != wws->color_key)
        *window_surface->funcs->get_bounds(window_surface) = wws->header.rect;

    wws->alpha = alpha;
    wws->color_key = color_key;

    if (wws->wayland_buffer_queue &&
        wws->wayland_buffer_queue->format != get_preferred_format(wws))
    {
        recreate_wayland_buffer_queue(wws);
    }

    window_surface->funcs->unlock(window_surface);
}
