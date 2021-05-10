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

    surface->ref = 1;

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

    surface->parent = wayland_surface_ref(parent);
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
 * Note that this doesn't configure any associated GL/VK subsurface,
 * wayland_surface_reconfigure_glvk() needs to be called separately.
 */
void wayland_surface_reconfigure(struct wayland_surface *surface,
                                 int wine_x, int wine_y,
                                 int wine_width, int wine_height)
{
    int x, y, width, height;

    wayland_surface_coords_rounded_from_wine(surface, wine_x, wine_y, &x, &y);
    wayland_surface_coords_rounded_from_wine(surface, wine_width, wine_height,
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

    if (surface->xdg_surface && width != 0 && height != 0)
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
    surface->mapped = TRUE;

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
    TRACE("surface=%p hwnd=%p\n", surface, surface->hwnd);

    surface->crit.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&surface->crit);

    if (surface->zwp_locked_pointer_v1)
    {
        zwp_locked_pointer_v1_destroy(surface->zwp_locked_pointer_v1);
        surface->zwp_locked_pointer_v1 = NULL;
    }
    if (surface->zwp_confined_pointer_v1)
    {
        zwp_confined_pointer_v1_destroy(surface->zwp_confined_pointer_v1);
        surface->zwp_confined_pointer_v1 = NULL;
    }

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

    if (surface->parent)
    {
        wayland_surface_unref(surface->parent);
        surface->parent = NULL;
    }

    heap_free(surface);

    /* Destroying the surface can lead to events that we need to handle
     * immediately to get the latest state, so force a round trip, but only if
     * we are in the same thread that handles the window (otherwise we will
     * call wayland event handlers in an arbitrary thread, a scenario which we
     * do not support). */
    if (surface->wayland->thread_id == GetCurrentThreadId())
    {
        wl_display_roundtrip_queue(surface->wayland->wl_display,
                                   surface->wayland->wl_event_queue);
    }
}

static struct wayland_surface *wayland_surface_create_glvk_common(struct wayland_surface *surface)
{
    struct wayland_surface *glvk;

    TRACE("surface=%p hwnd=%p\n", surface, surface->hwnd);

    glvk = wayland_surface_create_common(surface->wayland);
    if (!glvk)
        goto err;

    glvk->parent = wayland_surface_ref(surface);
    glvk->wl_subsurface =
        wl_subcompositor_get_subsurface(glvk->wayland->wl_subcompositor,
                                        glvk->wl_surface,
                                        surface->wl_surface);
    if (!glvk->wl_subsurface)
        goto err;
    wl_subsurface_set_desync(glvk->wl_subsurface);

    glvk->hwnd = surface->hwnd;

    return glvk;

err:
    if (glvk)
        wayland_surface_destroy(glvk);

    return NULL;

}

/**********************************************************************
 *          wayland_surface_create_gl
 *
 * Creates a GL subsurface for this wayland surface.
 */
BOOL wayland_surface_create_or_ref_gl(struct wayland_surface *surface)
{
    struct wayland_surface *glvk;

    TRACE("surface=%p hwnd=%p\n", surface, surface->hwnd);

    if (surface->glvk)
    {
        wayland_surface_ref(surface->glvk);
        return TRUE;
    }

    glvk = wayland_surface_create_glvk_common(surface);
    if (!glvk)
        goto err;

    glvk->wl_egl_window = wl_egl_window_create(glvk->wl_surface, 1, 1);
    if (!glvk->wl_egl_window)
        goto err;

    surface->glvk = glvk;

    wl_surface_commit(glvk->wl_surface);

    return TRUE;

err:
    if (glvk)
        wayland_surface_destroy(glvk);

    return FALSE;
}

/**********************************************************************
 *          wayland_surface_create_vk
 *
 * Creates a VK subsurface for this wayland surface.
 */
BOOL wayland_surface_create_or_ref_vk(struct wayland_surface *surface)
{
    struct wayland_surface *glvk;

    TRACE("surface=%p hwnd=%p\n", surface, surface->hwnd);

    if (surface->glvk)
    {
        wayland_surface_ref(surface->glvk);
        return TRUE;
    }

    glvk = wayland_surface_create_glvk_common(surface);
    if (!glvk)
        goto err;

    surface->glvk = glvk;
    wayland_surface_ref(surface);

    wl_surface_commit(glvk->wl_surface);

    return TRUE;

err:
    if (glvk)
        wayland_surface_destroy(glvk);

    return FALSE;
}

/**********************************************************************
 *          wayland_surface_unref_glvk
 *
 * Unreferences the associated GL/VK subsurface for this wayland surface.
 */
void wayland_surface_unref_glvk(struct wayland_surface *surface)
{
    if (!surface->glvk)
        return;

    TRACE("surface=%p hwnd=%p\n", surface, surface->hwnd);

    if (InterlockedDecrement(&surface->glvk->ref))
        return;

    wayland_surface_destroy(surface->glvk);
    surface->glvk = NULL;
}

/**********************************************************************
 *          wayland_surface_reconfigure_glvk
 *
 * Configures the position and size of the GL/VK subsurface associated with
 * a wayland surface.
 *
 * The coordinates and sizes should be given in wine's coordinate space.
 */
void wayland_surface_reconfigure_glvk(struct wayland_surface *surface,
                                      int wine_x, int wine_y,
                                      int wine_width, int wine_height)
{
    int x, y, width, height;
    struct wayland_surface *glvk = surface->glvk;

    if (!glvk)
        return;

    wayland_surface_coords_rounded_from_wine(surface, wine_x, wine_y, &x, &y);
    wayland_surface_coords_rounded_from_wine(surface, wine_width, wine_height,
                                             &width, &height);

    TRACE("surface=%p hwnd=%p %d,%d+%dx%d %d,%d+%dx%d\n",
          surface, surface->hwnd,
          wine_x, wine_y, wine_width, wine_height,
          x, y, width, height);

    glvk->offset_x = wine_x;
    glvk->offset_y = wine_y;

    wl_subsurface_set_position(glvk->wl_subsurface, x, y);
    /* The EGL window size needs to be in wine coords since this affects
     * the effective EGL buffer size. */
    if (glvk->wl_egl_window)
        wl_egl_window_resize(glvk->wl_egl_window, wine_width, wine_height, 0, 0);

    /* Use a viewport, if supported, to ensure GL surfaces remain inside
     * their parent's boundaries when resizing and also to handle display mode
     * changes. */
    if (glvk->wp_viewport)
    {
        if (width != 0 && height != 0)
            wp_viewport_set_destination(glvk->wp_viewport, width, height);
        else
            wp_viewport_set_destination(glvk->wp_viewport, -1, -1);
    }

    wl_surface_commit(glvk->wl_surface);
}

/**********************************************************************
 *          wayland_surface_unmap
 *
 * Unmaps (i.e., hides) this surface.
 */
void wayland_surface_unmap(struct wayland_surface *surface)
{
    EnterCriticalSection(&surface->crit);

    wl_surface_attach(surface->wl_surface, NULL, 0, 0);
    wl_surface_commit(surface->wl_surface);
    surface->mapped = FALSE;

    LeaveCriticalSection(&surface->crit);
}

/**********************************************************************
 *          wayland_surface_coords_to_screen
 *
 * Converts the surface-local coordinates to Windows screen coordinates.
 */
void wayland_surface_coords_to_screen(struct wayland_surface *surface,
                                      double wayland_x, double wayland_y,
                                      int *screen_x, int *screen_y)
{
    RECT window_rect = {0};
    int wine_x, wine_y;

    wayland_surface_coords_to_wine(surface, wayland_x, wayland_y,
                                   &wine_x, &wine_y);

    GetWindowRect(surface->hwnd, &window_rect);

    /* Some wayland surfaces are offset relative to their window rect,
     * e.g., GL subsurfaces. */
    OffsetRect(&window_rect, surface->offset_x, surface->offset_y);

    *screen_x = wine_x + window_rect.left;
    *screen_y = wine_y + window_rect.top;

    TRACE("hwnd=%p wayland=%.2f,%.2f rect=%s => screen=%d,%d\n",
          surface->hwnd, wayland_x, wayland_y, wine_dbgstr_rect(&window_rect),
          *screen_x, *screen_y);
}


/**********************************************************************
 *          wayland_surface_coords_from_screen
 *
 * Converts the Windows screen coordinates to surface-local coordinates.
 */
void wayland_surface_coords_from_screen(struct wayland_surface *surface,
                                        int screen_x, int screen_y,
                                        double *wayland_x, double *wayland_y)

{
    int wine_x, wine_y;
    RECT window_rect = {0};

    /* Screen to window */
    GetWindowRect(surface->hwnd, &window_rect);
    OffsetRect(&window_rect, surface->offset_x, surface->offset_y);

    wine_x = screen_x - window_rect.left;
    wine_y = screen_y - window_rect.top;

    /* Window to wayland surface */
    wayland_surface_coords_from_wine(surface, wine_x, wine_y,
                                     wayland_x, wayland_y);

    TRACE("hwnd=%p screen=%d,%d rect=%s => wayland=%.2f,%.2f\n",
          surface->hwnd, screen_x, screen_y, wine_dbgstr_rect(&window_rect),
          *wayland_x, *wayland_y);
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
                                      double *wayland_x, double *wayland_y)
{
    struct wayland_output *output = wayland_first_output(surface->wayland);

    if (output)
    {
        *wayland_x = wine_x * output->wine_scale;
        *wayland_y = wine_y * output->wine_scale;
    }
    else
    {
        *wayland_x = wine_x;
        *wayland_y = wine_y;
    }

    TRACE("hwnd=%p scale=%f wine=%d,%d => wayland=%.2f,%.2f\n",
          surface->hwnd, output ? output->wine_scale : -1.0, wine_x, wine_y,
          *wayland_x, *wayland_y);
}

/**********************************************************************
 *          wayland_surface_coords_rounded_from_wine
 *
 * Converts the window-local wine coordinates to wayland surface-local coordinates
 * rounding to the closest integer value.
 */
void wayland_surface_coords_rounded_from_wine(struct wayland_surface *surface,
                                              int wine_x, int wine_y,
                                              int *wayland_x, int *wayland_y)
{
    double w_x, w_y;
    wayland_surface_coords_from_wine(surface, wine_x, wine_y, &w_x, &w_y);
    *wayland_x = round(w_x);
    *wayland_y = round(w_y);
}

/**********************************************************************
 *          wayland_surface_coords_to_wine
 *
 * Converts the surface-local coordinates to wine windows-local coordinates.
 */
void wayland_surface_coords_to_wine(struct wayland_surface *surface,
                                    double wayland_x, double wayland_y,
                                    int *wine_x, int *wine_y)
{
    struct wayland_output *output = wayland_first_output(surface->wayland);

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

    TRACE("hwnd=%p scale=%f wayland=%.2f,%.2f => wine=%d,%d\n",
          surface->hwnd, output ? output->wine_scale : -1.0,
          wayland_x, wayland_y, *wine_x, *wine_y);

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
    double subarea_width, subarea_height;

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
            subarea_width = wayland_height * wine_aspect;
            subarea_height = wayland_height;
        }
        else
        {
            subarea_width = wayland_width;
            subarea_height = wayland_width / wine_aspect;
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

static void dummy_buffer_release(void *data, struct wl_buffer *buffer)
{
    struct wayland_shm_buffer *shm_buffer = data;

    TRACE("shm_buffer=%p\n", shm_buffer);

    wayland_shm_buffer_destroy(shm_buffer);
}

static const struct wl_buffer_listener dummy_buffer_listener = {
    dummy_buffer_release
};

/**********************************************************************
 *          wayland_surface_ensure_mapped
 *
 * Ensure that the wayland surface is mapped, by committing a dummy
 * buffer if necessary.
 *
 * The contents of GL or Vulkan windows are rendered on subsurfaces
 * with the parent surface used for the decorations. Such GL/VK
 * subsurfaces may want to commit their contents before the parent
 * surface has had a chance to commit. In such cases the GL/VK commit
 * will not be displayed, but, more importantly, will not get a frame
 * callback until the parent surface is also committed. Depending on
 * the presentation mode, a second GL/VK buffer swap may indefinitely
 * block waiting on the frame callback. By calling this function before a
 * GL/VK buffer swap we can avoid this situation.
 */
void wayland_surface_ensure_mapped(struct wayland_surface *surface)
{
    EnterCriticalSection(&surface->crit);

    TRACE("surface=%p hwnd=%p mapped=%d\n",
          surface, surface->hwnd, surface->mapped);

    if (!surface->mapped)
    {
        int width = surface->current.width;
        int height = surface->current.height;
        struct wayland_shm_buffer *dummy_shm_buffer;

        if (width == 0) width = 1;
        if (height == 0) height = 1;

        dummy_shm_buffer = wayland_shm_buffer_create(surface->wayland,
                                                     width, height,
                                                     WL_SHM_FORMAT_ARGB8888);
        wl_buffer_add_listener(dummy_shm_buffer->wl_buffer,
                               &dummy_buffer_listener, dummy_shm_buffer);

        wayland_surface_commit_buffer(surface, dummy_shm_buffer, NULL);
    }

    LeaveCriticalSection(&surface->crit);
}


/**********************************************************************
 *          wayland_surface_ref
 *
 * Add a reference to a wayland_surface.
 */
struct wayland_surface *wayland_surface_ref(struct wayland_surface *surface)
{
    InterlockedIncrement(&surface->ref);
    return surface;
}

/**********************************************************************
 *          wayland_surface_unref
 *
 * Remove a reference to wayland_surface, potentially destroying it.
 */
void wayland_surface_unref(struct wayland_surface *surface)
{
    if (InterlockedDecrement(&surface->ref))
        return;

    wayland_surface_destroy(surface);
}

/**********************************************************************
 *          wayland_update_pointer_confinement
 *
 * Update pointer confinement on the surface. Confinement mode depends
 * on the current Windows cursor clip and cursor visibility.
 */
void wayland_surface_update_pointer_confinement(struct wayland_surface *surface)
{
    struct wayland *wayland = surface->wayland;
    struct wl_region *region;
    RECT vscreen_rect;
    RECT clip_rect = wayland->cursor_clip;
    RECT client_rect = {0};
    RECT client_clip_rect;
    BOOL needs_lock = FALSE;
    BOOL needs_confine = FALSE;

    if (!wayland->zwp_pointer_constraints_v1 || !wayland->pointer.wl_pointer)
        return;

    GetClientRect(surface->hwnd, &client_rect);
    MapWindowPoints(surface->hwnd, NULL, (POINT*)&client_rect, 2);

    vscreen_rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vscreen_rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vscreen_rect.bottom = vscreen_rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    vscreen_rect.right = vscreen_rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);

    /* Get the effective clip area, if any. */
    IntersectRect(&clip_rect, &clip_rect, &vscreen_rect);
    IntersectRect(&client_clip_rect, &clip_rect, &client_rect);

    TRACE("wayland=%p surface=%p (glvk=%p) clip_rect=%s client_clip_rect=%s "
          "client=%s vscreen=%s hcursor=%p\n",
          wayland, surface, surface->glvk,
          wine_dbgstr_rect(&clip_rect), wine_dbgstr_rect(&client_clip_rect),
          wine_dbgstr_rect(&client_rect),
          wine_dbgstr_rect(&vscreen_rect),
          GetCursor());

    surface = surface->glvk ? surface->glvk : surface;

    /* Only confine or lock if the cursor is actually clipped within this window. */
    if (!IsRectEmpty(&client_clip_rect))
    {
        HCURSOR hcursor = GetCursor();
        if (!hcursor &&
            (!EqualRect(&clip_rect, &vscreen_rect) ||
             EqualRect(&client_rect, &vscreen_rect)))
        {
            needs_lock = TRUE;
        }
        else if (hcursor && !EqualRect(&clip_rect, &vscreen_rect))
        {
            needs_confine = TRUE;
        }
    }

    /* Destroy unneeded interface objects. */
    if (!needs_lock && surface->zwp_locked_pointer_v1)
    {
        POINT cursor_pos;

        if (GetCursorPos(&cursor_pos) && PtInRect(&client_rect, cursor_pos))
        {
            double wayland_x, wayland_y;
            wayland_surface_coords_from_screen(surface,
                                               cursor_pos.x, cursor_pos.y,
                                               &wayland_x, &wayland_y);

            zwp_locked_pointer_v1_set_cursor_position_hint(
                    surface->zwp_locked_pointer_v1,
                    wl_fixed_from_double(wayland_x),
                    wl_fixed_from_double(wayland_y));

            wl_surface_commit(surface->wl_surface);
        }

        zwp_locked_pointer_v1_destroy(surface->zwp_locked_pointer_v1);
        surface->zwp_locked_pointer_v1 = NULL;
    }

    if (!needs_confine && surface->zwp_confined_pointer_v1)
    {
        zwp_confined_pointer_v1_destroy(surface->zwp_confined_pointer_v1);
        surface->zwp_confined_pointer_v1 = NULL;
    }

    /* Set up (or update) pointer confinement or lock. */
    if (needs_confine)
    {
        double top, left, bottom, right;

        wayland_surface_coords_from_screen(surface,
                                           client_clip_rect.left,
                                           client_clip_rect.top,
                                           &left, &top);
        wayland_surface_coords_from_screen(surface,
                                           client_clip_rect.right,
                                           client_clip_rect.bottom,
                                           &right, &bottom);

        region = wl_compositor_create_region(wayland->wl_compositor);
        wl_region_add(region, round(left), round(top),
                      round(right - left), round(bottom - top));

        if (!surface->zwp_confined_pointer_v1)
        {
            surface->zwp_confined_pointer_v1 =
                zwp_pointer_constraints_v1_confine_pointer(
                    wayland->zwp_pointer_constraints_v1,
                    surface->wl_surface,
                    wayland->pointer.wl_pointer,
                    region,
                    ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        }
        else
        {
            zwp_confined_pointer_v1_set_region(surface->zwp_confined_pointer_v1,
                                               region);
        }

        wl_region_destroy(region);
    }
    else if (needs_lock)
    {
        if (!surface->zwp_locked_pointer_v1)
        {
            surface->zwp_locked_pointer_v1 =
                zwp_pointer_constraints_v1_lock_pointer(
                    wayland->zwp_pointer_constraints_v1,
                    surface->wl_surface,
                    wayland->pointer.wl_pointer,
                    NULL,
                    ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        }
        else
        {
            zwp_locked_pointer_v1_set_region(surface->zwp_locked_pointer_v1,
                                             NULL);
        }
    }

    wayland_pointer_set_relative(&wayland->pointer, needs_lock);

    if (needs_confine || needs_lock)
        wl_surface_commit(surface->wl_surface);
}
