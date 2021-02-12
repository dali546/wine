/*
 * Wayland surfaces
 *
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
#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/unicode.h"
#include "winuser.h"
#include <linux/input.h>

#include <errno.h>
#include <assert.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

/* Change to 1 to dump committed buffer contents to disk */
#define DEBUG_DUMP_COMMIT_BUFFER 0

static void handle_xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
			                 uint32_t serial)
{
    struct wayland_surface *surface = data;
    uint32_t last_serial = surface->pending.serial;

    TRACE("hwnd=%p serial=%u\n", surface->hwnd, serial);

    surface->pending.serial = serial;

    /* If we already have a pending configure event, no need to repost */
    if (last_serial)
    {
        TRACE("not reposting, last_serial=%u\n", last_serial);
        return;
    }

    if (surface->hwnd)
        PostMessageW(surface->hwnd, WM_WAYLAND_CONFIGURE, 0, 0);
    else
        wayland_surface_ack_configure(surface);
}

void wayland_surface_ack_configure(struct wayland_surface *surface)
{
    if (!surface->xdg_surface || !surface->pending.serial)
        return;

    TRACE("Setting current serial=%u size=%dx%d flags=%#x\n",
          surface->pending.serial, surface->pending.width,
          surface->pending.height, surface->pending.configure_flags);

    /* Guard setting current config, so that we only commit acceptable
     * buffers. Also see wayland_surface_commit_buffer(). */
    EnterCriticalSection(&surface->crit);

    surface->current = surface->pending;
    xdg_surface_ack_configure(surface->xdg_surface, surface->current.serial);

    LeaveCriticalSection(&surface->crit);

    memset(&surface->pending, 0, sizeof(surface->pending));
}

static const struct xdg_surface_listener xdg_surface_listener = {
    handle_xdg_surface_configure,
};

static void handle_xdg_toplevel_configure(void *data,
                                          struct xdg_toplevel *xdg_toplevel,
                                          int32_t width, int32_t height,
                                          struct wl_array *states)
{
    struct wayland_surface *surface = data;
    uint32_t *state;
    int flags = 0;

    wl_array_for_each(state, states)
    {
        switch(*state)
        {
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            flags |= WAYLAND_CONFIGURE_FLAG_MAXIMIZED;
            break;
        case XDG_TOPLEVEL_STATE_ACTIVATED:
            flags |= WAYLAND_CONFIGURE_FLAG_ACTIVATED;
            break;
        case XDG_TOPLEVEL_STATE_RESIZING:
            flags |= WAYLAND_CONFIGURE_FLAG_RESIZING;
            break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            flags |= WAYLAND_CONFIGURE_FLAG_FULLSCREEN;
            break;
        default:
            break;
        }
    }

    surface->pending.width = width;
    surface->pending.height = height;
    surface->pending.configure_flags = flags;

    TRACE("%dx%d flags=%#x\n", width, height, flags);
}

static void handle_xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    TRACE("\n");
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    handle_xdg_toplevel_configure,
    handle_xdg_toplevel_close,
};

static struct wayland_surface *wayland_surface_create_common(struct wayland *wayland)
{
    struct wayland_surface *surface;

    surface = heap_alloc_zero(sizeof(*surface));
    if (!surface)
        goto err;

    InitializeCriticalSection(&surface->crit);
    surface->crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": wayland_surface");

    surface->wayland = wayland;

    surface->wl_surface = wl_compositor_create_surface(wayland->wl_compositor);
    if (!surface->wl_surface)
        goto err;

    if (surface->wayland->wp_viewporter)
    {
        surface->wp_viewport =
            wp_viewporter_get_viewport(surface->wayland->wp_viewporter,
                                       surface->wl_surface);
    }

    wl_list_insert(&wayland->surface_list, &surface->link);
    wl_surface_set_user_data(surface->wl_surface, surface);

    return surface;

err:
    if (surface)
        wayland_surface_destroy(surface);
    return NULL;
}

/**********************************************************************
 *          wayland_surface_create_toplevel
 *
 * Creates a toplevel wayland surface, optionally associated with a parent
 * surface.
 */
struct wayland_surface *wayland_surface_create_toplevel(struct wayland *wayland,
                                                        struct wayland_surface *parent)
{
    struct wayland_surface *surface;

    TRACE("parent=%p\n", parent);

    surface = wayland_surface_create_common(wayland);
    if (!surface)
        goto err;

    surface->xdg_surface =
        xdg_wm_base_get_xdg_surface(wayland->xdg_wm_base, surface->wl_surface);
    if (!surface->xdg_surface)
        goto err;

    xdg_surface_add_listener(surface->xdg_surface, &xdg_surface_listener, surface);

    surface->xdg_toplevel = xdg_surface_get_toplevel(surface->xdg_surface);
    if (!surface->xdg_toplevel)
        goto err;
    xdg_toplevel_add_listener(surface->xdg_toplevel, &xdg_toplevel_listener, surface);

    if (parent && parent->xdg_toplevel)
        xdg_toplevel_set_parent(surface->xdg_toplevel, parent->xdg_toplevel);

    wl_surface_commit(surface->wl_surface);

    /* Wait for the first configure event. */
    while (!surface->current.serial)
        wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);

    return surface;

err:
    if (surface)
        wayland_surface_destroy(surface);
    return NULL;
}

/**********************************************************************
 *          wayland_surface_create_subsurface
 *
 * Creates a wayland subsurface with the specified parent.
 */
struct wayland_surface *wayland_surface_create_subsurface(struct wayland *wayland,
                                                          struct wayland_surface *parent)
{
    struct wayland_surface *surface;

    TRACE("parent=%p\n", parent);

    surface = wayland_surface_create_common(wayland);
    if (!surface)
        goto err;

    surface->wl_subsurface =
        wl_subcompositor_get_subsurface(wayland->wl_subcompositor,
                                        surface->wl_surface,
                                        parent->wl_surface);
    if (!surface->wl_subsurface)
        goto err;
    wl_subsurface_set_desync(surface->wl_subsurface);

    wl_surface_commit(surface->wl_surface);

    return surface;

err:
    if (surface)
        wayland_surface_destroy(surface);
    return NULL;
}

/**********************************************************************
 *          wayland_surface_reconfigure
 *
 * Configures the position and size of a wayland surface. Depending on the
 * surface type, either repositioning or resizing may have no effect.
 *
 * The coordinates and sizes should be given in wine's coordinate space.
 *
 * Note that this doesn't configure any associated GL subsurface,
 * wayland_surface_reconfigure_gl() needs to be called separately.
 */
void wayland_surface_reconfigure(struct wayland_surface *surface,
                                 int wine_x, int wine_y,
                                 int wine_width, int wine_height)
{
    int x, y, width, height;

    wayland_surface_coords_from_wine(surface, wine_x, wine_y, &x, &y);
    wayland_surface_coords_from_wine(surface, wine_width, wine_height,
                                     &width, &height);

    TRACE("surface=%p hwnd=%p %d,%d+%dx%d %d,%d+%dx%d\n",
          surface, surface->hwnd,
          wine_x, wine_y, wine_width, wine_height,
          x, y, width, height);

    if (surface->wl_subsurface)
    {
        wl_subsurface_set_position(surface->wl_subsurface, x, y);
        wl_surface_commit(surface->wl_surface);
    }

    /* Use a viewport, if supported, to handle display mode changes. */
    if (surface->wp_viewport)
    {
        if (width != 0 && height != 0)
            wp_viewport_set_destination(surface->wp_viewport, width, height);
        else
            wp_viewport_set_destination(surface->wp_viewport, -1, -1);
    }

    if (surface->xdg_surface)
        xdg_surface_set_window_geometry(surface->xdg_surface, 0, 0, width, height);
}

static void dump_commit_buffer(struct wayland_shm_buffer *shm_buffer)
{
    static int dbgid = 0;

    dbgid++;

    dump_pixels("/tmp/winedbg/commit-%.3d.pam", dbgid, shm_buffer->map_data,
                shm_buffer->width, shm_buffer->height,
                shm_buffer->format == WL_SHM_FORMAT_ARGB8888,
                shm_buffer->damage_region, NULL);
}

static RGNDATA *get_region_data(HRGN region)
{
    RGNDATA *data = NULL;
    DWORD size;

    if (!(size = GetRegionData(region, 0, NULL))) goto err;
    if (!(data = heap_alloc_zero(size))) goto err;

    if (!GetRegionData(region, size, data)) goto err;

    return data;

err:
    if (data)
        heap_free(data);
    return NULL;
}

/**********************************************************************
 *          wayland_surface_configure_is_compatible
 *
 * Checks whether a wayland_surface_configure object is compatible with the
 * the provided arguments.
 *
 * If flags is zero, only the width and height are checked for compatibility,
 * otherwise, the configure objects flags must also match the passed flags.
 */
BOOL wayland_surface_configure_is_compatible(struct wayland_surface_configure *conf,
                                             int width, int height,
                                             enum wayland_configure_flags flags)
{
    BOOL compat_flags = flags ? (flags & conf->configure_flags) : TRUE;
    BOOL compat_with_max =
        !(conf->configure_flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED) ||
        (width == conf->width && height == conf->height);
    BOOL compat_with_full =
        !(conf->configure_flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN) ||
        (width <= conf->width && height <= conf->height);

    return compat_flags && compat_with_max && compat_with_full;
}

/**********************************************************************
 *          wayland_surface_commit_buffer
 *
 * Commits a SHM buffer on a wayland surface.
 */
void wayland_surface_commit_buffer(struct wayland_surface *surface,
                                   struct wayland_shm_buffer *shm_buffer,
                                   HRGN surface_damage_region)
{
    RGNDATA *surface_damage;

    /* Since multiple threads can commit a buffer to a wayland surface
     * (e.g., subwindows in different threads), we guard this function
     * to ensure we don't commit buffers that are not acceptable by the
     * compositor (see below, and also wayland_surface_ack_configure()). */
    EnterCriticalSection(&surface->crit);

    TRACE("surface=%p (%dx%d) flags=%#x buffer=%p (%dx%d)\n", surface,
            surface->current.width, surface->current.height,
            surface->current.configure_flags,
            shm_buffer, shm_buffer->width, shm_buffer->height);

    /* Maximized surfaces are very strict about the dimensions of buffers
     * they accept. To avoid wayland protocol errors, drop buffers not matching
     * the expected dimensions of maximized surfaces. This typically happens
     * transiently during resizing operations. */
    if (!wayland_surface_configure_is_compatible(&surface->current,
                                                 shm_buffer->width,
                                                 shm_buffer->height,
                                                 surface->current.configure_flags))
    {
        LeaveCriticalSection(&surface->crit);
        TRACE("surface=%p buffer=%p dropping buffer\n", surface, shm_buffer);
        shm_buffer->busy = FALSE;
        return;
    }

    if (DEBUG_DUMP_COMMIT_BUFFER)
        dump_commit_buffer(shm_buffer);

    wl_surface_attach(surface->wl_surface, shm_buffer->wl_buffer, 0, 0);

    /* Add surface damage, i.e., which parts of the surface have changed since
     * the last surface commit. Note that this is different from the buffer
     * damage returned by wayland_shm_buffer_get_damage(). */
    surface_damage = get_region_data(surface_damage_region);
    if (surface_damage)
    {
        RECT *rgn_rect = (RECT *)surface_damage->Buffer;
        RECT *rgn_rect_end = rgn_rect + surface_damage->rdh.nCount;

        for (;rgn_rect < rgn_rect_end; rgn_rect++)
        {
            wl_surface_damage_buffer(surface->wl_surface,
                                     rgn_rect->left, rgn_rect->top,
                                     rgn_rect->right - rgn_rect->left,
                                     rgn_rect->bottom - rgn_rect->top);
        }
        heap_free(surface_damage);
    }

    wl_surface_commit(surface->wl_surface);

    LeaveCriticalSection(&surface->crit);

    wl_display_flush(surface->wayland->wl_display);
}

/**********************************************************************
 *          wayland_surface_destroy
 *
 * Destroys a wayland surface.
 */
void wayland_surface_destroy(struct wayland_surface *surface)
{
    struct wayland *wayland = surface->wayland;
    TRACE("surface=%p hwnd=%p\n", surface, surface->hwnd);

    surface->crit.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&surface->crit);

    wayland_surface_destroy_gl(surface);

    if (surface->wl_egl_window) {
        wl_egl_window_destroy(surface->wl_egl_window);
        surface->wl_egl_window = NULL;
    }

    if (surface->wp_viewport)
        wp_viewport_destroy(surface->wp_viewport);

    if (surface->xdg_toplevel) {
        xdg_toplevel_destroy(surface->xdg_toplevel);
        surface->xdg_toplevel = NULL;
    }
    if (surface->xdg_surface) {
        xdg_surface_destroy(surface->xdg_surface);
        surface->xdg_surface = NULL;
    }

    if (surface->wl_subsurface) {
        wl_subsurface_destroy(surface->wl_subsurface);
        surface->wl_subsurface = NULL;
    }
    if (surface->wl_surface) {
        wl_surface_destroy(surface->wl_surface);
        surface->wl_surface = NULL;
    }

    wl_list_remove(&surface->link);

    heap_free(surface);

    /* Destroying the surface can lead to events that we need to handle
     * immediately to get the latest state, so force a round trip. */
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);
}

/**********************************************************************
 *          wayland_surface_create_gl
 *
 * Creates a GL subsurface for this wayland surface.
 */
BOOL wayland_surface_create_gl(struct wayland_surface *surface)
{
    struct wayland_surface *surface_gl;

    TRACE("surface=%p hwnd=%p\n", surface, surface->hwnd);

    surface_gl = wayland_surface_create_common(surface->wayland);
    if (!surface_gl)
        goto err;

    surface_gl->wl_subsurface =
        wl_subcompositor_get_subsurface(surface_gl->wayland->wl_subcompositor,
                                        surface_gl->wl_surface,
                                        surface->wl_surface);
    if (!surface_gl->wl_subsurface)
        goto err;
    wl_subsurface_set_desync(surface_gl->wl_subsurface);

    surface_gl->wl_egl_window = wl_egl_window_create(surface_gl->wl_surface, 1, 1);
    if (!surface_gl->wl_egl_window)
        goto err;

    surface->gl = surface_gl;

    wl_surface_commit(surface_gl->wl_surface);

    surface_gl->hwnd = surface->hwnd;

    return TRUE;

err:
    if (surface_gl)
        wayland_surface_destroy(surface_gl);

    return FALSE;
}

/**********************************************************************
 *          wayland_surface_destroy_gl
 *
 * Destroys the associated GL subsurface for this wayland surface.
 */
void wayland_surface_destroy_gl(struct wayland_surface *surface)
{
    if (!surface->gl)
        return;

    TRACE("surface=%p hwnd=%p\n", surface, surface->hwnd);

    wayland_surface_destroy(surface->gl);
    surface->gl = NULL;
}

/**********************************************************************
 *          wayland_surface_reconfigure_gl
 *
 * Configures the position and size of the GL subsurface associated with
 * a wayland surface.
 *
 * The coordinates and sizes should be given in wine's coordinate space.
 */
void wayland_surface_reconfigure_gl(struct wayland_surface *surface,
                                    int wine_x, int wine_y,
                                    int wine_width, int wine_height)
{
    int x, y, width, height;

    if (!surface->gl)
        return;

    wayland_surface_coords_from_wine(surface, wine_x, wine_y, &x, &y);
    wayland_surface_coords_from_wine(surface, wine_width, wine_height,
                                     &width, &height);

    TRACE("surface=%p hwnd=%p %d,%d+%dx%d %d,%d+%dx%d\n",
          surface, surface->hwnd,
          wine_x, wine_y, wine_width, wine_height,
          x, y, width, height);

    surface->gl->offset_x = wine_x;
    surface->gl->offset_y = wine_y;

    wl_subsurface_set_position(surface->gl->wl_subsurface, x, y);
    /* The EGL window size needs to be in wine coords since this affects
     * the effective EGL buffer size. */
    wl_egl_window_resize(surface->gl->wl_egl_window, wine_width, wine_height, 0, 0);

    /* Use a viewport, if supported, to ensure GL surfaces remain inside
     * their parent's boundaries when resizing and also to handle display mode
     * changes. */
    if (surface->gl->wp_viewport)
    {
        if (width != 0 && height != 0)
            wp_viewport_set_destination(surface->gl->wp_viewport, width, height);
        else
            wp_viewport_set_destination(surface->gl->wp_viewport, -1, -1);
    }

    wl_surface_commit(surface->gl->wl_surface);
}

/**********************************************************************
 *          wayland_surface_unmap
 *
 * Unmaps (i.e., hides) this surface.
 */
void wayland_surface_unmap(struct wayland_surface *surface)
{
    wl_surface_attach(surface->wl_surface, NULL, 0, 0);
    wl_surface_commit(surface->wl_surface);
}

/**********************************************************************
 *          wayland_surface_coords_to_screen
 *
 * Converts the surface-local coordinates to Windows screen coordinates.
 */
POINT wayland_surface_coords_to_screen(struct wayland_surface *surface, int x, int y)
{
    POINT point;
    RECT window_rect = {0};
    int wine_x, wine_y;

    wayland_surface_coords_to_wine(surface, x, y, &wine_x, &wine_y);

    GetWindowRect(surface->hwnd, &window_rect);

    /* Some wayland surfaces are offset relative to their window rect,
     * e.g., GL subsurfaces. */
    OffsetRect(&window_rect, surface->offset_x, surface->offset_y);

    point.x = wine_x + window_rect.left;
    point.y = wine_y + window_rect.top;

    TRACE("hwnd=%p x=%d y=%d rect %s => %d,%d\n",
          surface->hwnd, x, y, wine_dbgstr_rect(&window_rect),
          point.x, point.y);

    return point;
}

static struct wayland_output *wayland_first_output(struct wayland *wayland)
{
    struct wayland_output *output = NULL;

    wl_list_for_each(output, &wayland->output_list, link)
        break;

    return output;
}

/**********************************************************************
 *          wayland_surface_coords_from_wine
 *
 * Converts the window-local wine coordinates to wayland surface-local coordinates.
 */
void wayland_surface_coords_from_wine(struct wayland_surface *surface,
                                      int wine_x, int wine_y,
                                      int *wayland_x, int *wayland_y)
{
    struct wayland_output *output = wayland_first_output(surface->wayland);

    TRACE("hwnd=%p scale=%f wine_x=%d wine_y=%d\n",
          surface->hwnd, output ? output->wine_scale : -1.0, wine_x, wine_y);

    if (output)
    {
        *wayland_x = round(wine_x * output->wine_scale);
        *wayland_y = round(wine_y * output->wine_scale);
    }
    else
    {
        *wayland_x = wine_x;
        *wayland_y = wine_y;
    }
}

/**********************************************************************
 *          wayland_surface_coords_to_wine
 *
 * Converts the surface-local coordinates to wine windows-local coordinates.
 */
void wayland_surface_coords_to_wine(struct wayland_surface *surface,
                                    int wayland_x, int wayland_y,
                                    int *wine_x, int *wine_y)
{
    struct wayland_output *output = wayland_first_output(surface->wayland);

    TRACE("hwnd=%p scale=%f wayland_x=%d wayland_y=%d\n",
          surface->hwnd, output ? output->wine_scale : -1.0,
          wayland_x, wayland_y);

    if (output)
    {
        *wine_x = round(wayland_x / output->wine_scale);
        *wine_y = round(wayland_y / output->wine_scale);
    }
    else
    {
        *wine_x = wayland_x;
        *wine_y = wayland_y;
    }
}

/**********************************************************************
 *          wayland_surface_find_wine_fullscreen_fit
 *
 * Finds the size of a fullscreen Wine window that when scaled best fits into a
 * wayland surface with the provided size, while maintaining the aspect
 * ratio of the current Wine display mode.
 */
void wayland_surface_find_wine_fullscreen_fit(struct wayland_surface *surface,
                                              int wayland_width, int wayland_height,
                                              int *wine_width, int *wine_height)
{
    struct wayland_output *output = wayland_first_output(surface->wayland);
    int subarea_width, subarea_height;

    TRACE("hwnd=%p wayland_width=%d wayland_height=%d\n",
          surface->hwnd, wayland_width, wayland_height);

    /* If the wine mode doesn't match the wayland mode, Find the largest subarea
     * within wayland_width x wayland_height that has an aspect ratio equal to
     * the wine display mode aspect ratio. */
    if (output)
    {
        double aspect = ((double)wayland_width) / wayland_height;
        double wine_aspect = ((double)output->current_wine_mode->width) / 
                             output->current_wine_mode->height;
        if (aspect > wine_aspect)
        {
            subarea_width = round(wayland_height * wine_aspect);
            subarea_height = wayland_height;
        }
        else
        {
            subarea_width = wayland_width;
            subarea_height = round(wayland_width / wine_aspect);
        }
    }
    else
    {
        subarea_width = wayland_width;
        subarea_height = wayland_height;
    }

    /* Transform the calculated subarea to wine coordinates. */
    wayland_surface_coords_to_wine(surface,
                                   subarea_width, subarea_height,
                                   wine_width, wine_height);
}
