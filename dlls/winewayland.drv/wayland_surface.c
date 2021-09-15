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

#include "winuser.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static void wayland_surface_set_main_output(struct wayland_surface *surface,
                                            struct wayland_output *output);

static void handle_xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                         uint32_t serial)
{
    struct wayland_surface *surface = data;
    uint32_t last_serial = surface->pending.serial;
    BOOL last_processed = surface->pending.processed;

    TRACE("hwnd=%p serial=%u\n", surface->hwnd, serial);

    surface->pending.serial = serial;
    surface->pending.processed = FALSE;

    /* If we have an unprocessed WM_WAYLAND_CONFIGURE message, no need to
     * repost. Note that checking only for a valid serial is not enough to
     * guarantee that there is a pending WM_WAYLAND_CONFIGURE message: we may
     * have processed the message but not acked the configure request due to
     * surface size incompatibilities (see window.c:
     * wayland_win_data_update_wayland_surface_state()). */
    if (last_serial && !last_processed)
    {
        TRACE("not reposting, last_serial=%u\n", last_serial);
        return;
    }

    if (surface->hwnd)
        PostMessageW(surface->hwnd, WM_WAYLAND_CONFIGURE, 0, 0);
    else
        wayland_surface_ack_pending_configure(surface);
}

/**********************************************************************
 *          wayland_surface_ack_pending_configure
 *
 * Acks the pending configure event, making it current.
 */
void wayland_surface_ack_pending_configure(struct wayland_surface *surface)
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

static struct wayland_output *wayland_surface_get_origin_output(
        struct wayland_surface *surface)
{
    struct wayland_output_ref *ref;
    struct wayland_output *leftmost = NULL;

    /* The leftmost entered output is the origin.
     * TODO: Consider Right-To-Left setups. */
    wl_list_for_each(ref, &surface->output_ref_list, link)
    {
        if (!leftmost || ref->output->x < leftmost->x)
            leftmost = ref->output;
    }

    return leftmost;
}

static void handle_wl_surface_enter(void *data,
                                    struct wl_surface *wl_surface,
                                    struct wl_output *wl_output)
{
    struct wayland_surface *surface = data;
    struct wayland_output *output =
        wl_output ? wl_output_get_user_data(wl_output) : NULL;
    struct wayland_output_ref *ref;
    struct wayland_output *origin;

    if (!output || output->wayland != surface->wayland) return;

    TRACE("hwnd=%p output->name=%s output->id=0x%x\n",
          surface->hwnd, output->name, output->id);

    ref = heap_alloc_zero(sizeof(*ref));
    if (!ref) { ERR("memory allocation failed"); return; }
    ref->output = output;
    wl_list_insert(&surface->output_ref_list, &ref->link);

    origin = wayland_surface_get_origin_output(surface);
    wayland_surface_set_main_output(surface, origin);
}

static void handle_wl_surface_leave(void *data,
                                    struct wl_surface *wl_surface,
                                    struct wl_output *wl_output)
{
    struct wayland_surface *surface = data;
    struct wayland_output *output =
        wl_output ? wl_output_get_user_data(wl_output) : NULL;

    if (!output || output->wayland != surface->wayland) return;

    TRACE("hwnd=%p output->name=%s output->id=0x%x\n",
          surface->hwnd, output->name, output->id);

    wayland_surface_leave_output(surface, output);
}

static const struct wl_surface_listener wl_surface_listener = {
    handle_wl_surface_enter,
    handle_wl_surface_leave,
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

    wl_list_init(&surface->output_ref_list);
    wl_list_init(&surface->link);
    wl_list_init(&surface->child_list);
    wl_surface_set_user_data(surface->wl_surface, surface);
    surface->drawing_allowed = TRUE;

    surface->ref = 1;

    return surface;

err:
    if (surface)
        wayland_surface_destroy(surface);
    return NULL;
}

/**********************************************************************
 *          wayland_surface_create_plain
 *
 * Creates a plain, role-less wayland surface.
 */
struct wayland_surface *wayland_surface_create_plain(struct wayland *wayland)
{
    struct wayland_surface *surface;

    TRACE("\n");

    surface = wayland_surface_create_common(wayland);
    if (!surface)
        goto err;

    /* Plain surfaces are not visible, so don't allow drawing. */
    surface->drawing_allowed = FALSE;

    /* Although not technically toplevel, plain surfaces have no parent, so
     * track them in the toplevel list. */
    wl_list_insert(&wayland->toplevel_list, &surface->link);

    wl_surface_commit(surface->wl_surface);

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

    /* We want enter/leave events only for toplevels */
    wl_surface_add_listener(surface->wl_surface, &wl_surface_listener, surface);

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

    wl_list_insert(&wayland->toplevel_list, &surface->link);

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

    EnterCriticalSection(&parent->crit);
    wl_list_insert(&parent->child_list, &surface->link);
    LeaveCriticalSection(&parent->crit);

    surface->wl_subsurface =
        wl_subcompositor_get_subsurface(wayland->wl_subcompositor,
                                        surface->wl_surface,
                                        parent->wl_surface);
    if (!surface->wl_subsurface)
        goto err;
    wl_subsurface_set_desync(surface->wl_subsurface);

    surface->main_output = parent->main_output;
    surface->wine_output = parent->wine_output;
    wl_surface_set_buffer_scale(surface->wl_surface,
                                wayland_surface_get_buffer_scale(parent));

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
 *
 * This function sets up but doesn't actually apply any new configuration.
 * The wayland_surface_reconfigure_apply() needs to be called for changes
 * to take effect.
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
        wl_subsurface_set_position(surface->wl_subsurface, x, y);

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

static RGNDATA *get_region_data(HRGN region)
{
    RGNDATA *data = NULL;
    DWORD size;

    if (!(size = GetRegionData(region, 0, NULL))) goto err;
    if (!(data = heap_alloc_zero(size))) goto err;

    if (!GetRegionData(region, size, data)) goto err;

    return data;

err:
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
    int wayland_width, wayland_height;

    /* Since multiple threads can commit a buffer to a wayland surface
     * (e.g., child windows in different threads), we guard this function
     * to ensure we don't commit buffers that are not acceptable by the
     * compositor (see below, and also wayland_surface_ack_configure()). */
    EnterCriticalSection(&surface->crit);

    TRACE("surface=%p (%dx%d) flags=%#x buffer=%p (%dx%d)\n", surface,
            surface->current.width, surface->current.height,
            surface->current.configure_flags,
            shm_buffer, shm_buffer->width, shm_buffer->height);

    wayland_surface_coords_rounded_from_wine(surface,
                                             shm_buffer->width, shm_buffer->height,
                                             &wayland_width, &wayland_height);

    /* Maximized surfaces are very strict about the dimensions of buffers
     * they accept. To avoid wayland protocol errors, drop buffers not matching
     * the expected dimensions of maximized surfaces. This typically happens
     * transiently during resizing operations. */
    if (!wayland_surface_configure_is_compatible(&surface->current,
                                                 wayland_width,
                                                 wayland_height,
                                                 surface->current.configure_flags))
    {
        LeaveCriticalSection(&surface->crit);
        TRACE("surface=%p buffer=%p dropping buffer\n", surface, shm_buffer);
        shm_buffer->busy = FALSE;
        return;
    }

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
    struct wayland_pointer *pointer = &surface->wayland->pointer;
    struct wayland_keyboard *keyboard = &surface->wayland->keyboard;
    struct wayland_surface *child, *child_tmp;
    struct wayland_output_ref *ref, *ref_tmp;

    TRACE("surface=%p hwnd=%p\n", surface, surface->hwnd);

    if (pointer->focused_surface == surface)
        pointer->focused_surface = NULL;

    if (keyboard->focused_surface == surface)
        keyboard->focused_surface = NULL;

    /* There are children left only when we force a destruction during
     * thread deinitialization, otherwise the children hold a reference
     * to the parent and won't let it be destroyed. */
    EnterCriticalSection(&surface->crit);
    wl_list_for_each_safe(child, child_tmp, &surface->child_list, link)
    {
        /* Since the current surface (the parent) is being destroyed,
         * disassociate from the child to avoid the child trying to
         * destroy the parent. */
        child->parent = NULL;
        wayland_surface_destroy(child);
    }
    LeaveCriticalSection(&surface->crit);

    wl_list_for_each_safe(ref, ref_tmp, &surface->output_ref_list, link)
    {
        wl_list_remove(&ref->link);
        heap_free(ref);
    }

    if (surface->wl_egl_window)
    {
        wl_egl_window_destroy(surface->wl_egl_window);
        surface->wl_egl_window = NULL;
    }

    if (surface->wp_viewport)
    {
        wp_viewport_destroy(surface->wp_viewport);
        surface->wp_viewport = NULL;
    }

    if (surface->xdg_toplevel)
    {
        xdg_toplevel_destroy(surface->xdg_toplevel);
        surface->xdg_toplevel = NULL;
    }

    if (surface->xdg_surface)
    {
        xdg_surface_destroy(surface->xdg_surface);
        surface->xdg_surface = NULL;
    }

    if (surface->wl_subsurface)
    {
        wl_subsurface_destroy(surface->wl_subsurface);
        surface->wl_subsurface = NULL;
    }

    if (surface->wl_surface)
    {
        wl_surface_destroy(surface->wl_surface);
        surface->wl_surface = NULL;
    }

    if (surface->parent)
    {
        EnterCriticalSection(&surface->parent->crit);
        wl_list_remove(&surface->link);
        LeaveCriticalSection(&surface->parent->crit);

        wayland_surface_unref(surface->parent);
        surface->parent = NULL;
    }
    else
    {
        wl_list_remove(&surface->link);
    }

    surface->crit.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&surface->crit);

    wl_display_flush(surface->wayland->wl_display);

    heap_free(surface);
}

static struct wayland_surface *wayland_surface_create_glvk_common(struct wayland_surface *surface)
{
    struct wayland_surface *glvk;

    TRACE("surface=%p hwnd=%p\n", surface, surface->hwnd);

    glvk = wayland_surface_create_common(surface->wayland);
    if (!glvk)
        goto err;

    glvk->parent = wayland_surface_ref(surface);

    EnterCriticalSection(&glvk->parent->crit);
    wl_list_insert(&glvk->parent->child_list, &glvk->link);
    LeaveCriticalSection(&glvk->parent->crit);

    glvk->wl_subsurface =
        wl_subcompositor_get_subsurface(glvk->wayland->wl_subcompositor,
                                        glvk->wl_surface,
                                        surface->wl_surface);
    if (!glvk->wl_subsurface)
        goto err;
    wl_subsurface_set_desync(glvk->wl_subsurface);
    /* Place the glvk subsurface just above the parent surface, so that it
     * doesn't end up obscuring any other subsurfaces. */
    wl_subsurface_place_above(glvk->wl_subsurface, surface->wl_surface);

    glvk->hwnd = surface->hwnd;
    glvk->main_output = surface->main_output;
    wl_surface_set_buffer_scale(glvk->wl_surface, wayland_surface_get_buffer_scale(surface));

    return glvk;

err:
    if (glvk)
        wayland_surface_destroy(glvk);

    return NULL;

}

static struct wayland_surface *wayland_surface_ref_glvk(struct wayland_surface *surface)
{
    struct wayland_surface *glvk = NULL;
    EnterCriticalSection(&surface->crit);
    if (surface->glvk)
        glvk = wayland_surface_ref(surface->glvk);
    LeaveCriticalSection(&surface->crit);
    return glvk;
}

/**********************************************************************
 *          wayland_surface_create_gl
 *
 * Creates a GL subsurface for this wayland surface.
 */
BOOL wayland_surface_create_or_ref_gl(struct wayland_surface *surface)
{
    struct wayland_surface *glvk;
    RECT client_rect;

    TRACE("surface=%p hwnd=%p\n", surface, surface->hwnd);

    if (wayland_surface_ref_glvk(surface))
        return TRUE;

    glvk = wayland_surface_create_glvk_common(surface);
    if (!glvk)
        goto err;

    glvk->wl_egl_window = wl_egl_window_create(glvk->wl_surface, 1, 1);
    if (!glvk->wl_egl_window)
        goto err;

    EnterCriticalSection(&surface->crit);
    surface->glvk = glvk;
    LeaveCriticalSection(&surface->crit);

    /* Set initial position in the client area. */
    wayland_get_client_rect_in_win_coords(surface->hwnd, &client_rect);

    wayland_surface_reconfigure_glvk(surface,
                                     client_rect.left, client_rect.top,
                                     client_rect.right - client_rect.left,
                                     client_rect.bottom - client_rect.top);

    wayland_surface_reconfigure_apply(surface);

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
    struct wayland_surface *glvk_to_destroy = NULL;
    LONG ref = -12345;

    EnterCriticalSection(&surface->crit);
    if (surface->glvk && (ref = InterlockedDecrement(&surface->glvk->ref)) == 0)
    {
        glvk_to_destroy = surface->glvk;
        surface->glvk = NULL;
    }
    TRACE("surface=%p glvk=%p ref=%d->%d\n",
          surface, glvk_to_destroy ? glvk_to_destroy : surface->glvk, ref + 1, ref);
    LeaveCriticalSection(&surface->crit);

    if (glvk_to_destroy)
        wayland_surface_destroy(glvk_to_destroy);
}

/**********************************************************************
 *          wayland_surface_reconfigure_glvk
 *
 * Configures the position and size of the GL/VK subsurface associated with
 * a wayland surface.
 *
 * The coordinates and sizes should be given in wine's coordinate space.
 *
 * This function sets up but doesn't actually apply any new configuration.
 * The wayland_surface_reconfigure_apply() needs to be called for changes
 * to take effect.
 */
void wayland_surface_reconfigure_glvk(struct wayland_surface *surface,
                                      int wine_x, int wine_y,
                                      int wine_width, int wine_height)
{
    int x, y, width, height;
    struct wayland_surface *glvk = wayland_surface_ref_glvk(surface);

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

    /* Use a viewport, if supported, to ensure GL surfaces remain inside their
     * parent's boundaries when resizing and also to handle display mode
     * changes. If the size is invalid use a 1x1 destination (instead of
     * unsetting with -1x-1) since many apps don't respect a GL/VK 0x0 size
     * which can happen, e.g., when an app is minimized. */
    if (glvk->wp_viewport)
    {
        if (width != 0 && height != 0)
            wp_viewport_set_destination(glvk->wp_viewport, width, height);
        else
            wp_viewport_set_destination(glvk->wp_viewport, 1, 1);
    }

    wayland_surface_unref_glvk(surface);
}

/**********************************************************************
 *          wayland_surface_reconfigure_apply
 *
 * Applies the configuration set by previous calls to the
 * wayland_surface_reconfigure{_glvk}() functions.
 */
void wayland_surface_reconfigure_apply(struct wayland_surface *surface)
{
    struct wayland_surface *glvk = wayland_surface_ref_glvk(surface);

    if (glvk)
    {
        wl_surface_commit(glvk->wl_surface);
        wayland_surface_unref_glvk(surface);
    }

    wl_surface_commit(surface->wl_surface);

    /* Commit the parent so any subsurface repositioning takes effect. */
    if (surface->parent)
        wl_surface_commit(surface->parent->wl_surface);
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

/* If the surface has a main output set, use that, otherwise use the output
 * which Wine considers to contain the associated window. */
static struct wayland_output *wayland_surface_get_main_output(
        struct wayland_surface *surface)
{
    if (surface->main_output)
        return surface->main_output;

    return surface->wine_output;
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

/**********************************************************************
 *          wayland_surface_coords_from_wine
 *
 * Converts the window-local wine coordinates to wayland surface-local coordinates.
 */
void wayland_surface_coords_from_wine(struct wayland_surface *surface,
                                      int wine_x, int wine_y,
                                      double *wayland_x, double *wayland_y)
{
    struct wayland_output *output = wayland_surface_get_main_output(surface);
    int scale = wayland_surface_get_buffer_scale(surface);

    if (output)
    {
        *wayland_x = wine_x * output->wine_scale / scale;
        *wayland_y = wine_y * output->wine_scale / scale;
    }
    else
    {
        *wayland_x = wine_x / (double)scale;
        *wayland_y = wine_y / (double)scale;
    }

    TRACE("hwnd=%p wine_scale=%f wine=%d,%d => wayland=%.2f,%.2f\n",
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
    struct wayland_output *output = wayland_surface_get_main_output(surface);
    int scale = wayland_surface_get_buffer_scale(surface);

    if (output)
    {
        *wine_x = round(wayland_x * scale / output->wine_scale);
        *wine_y = round(wayland_y * scale / output->wine_scale);
    }
    else
    {
        *wine_x = wayland_x * scale;
        *wine_y = wayland_y * scale;
    }

    TRACE("hwnd=%p wine_scale=%f wayland=%.2f,%.2f => wine=%d,%d\n",
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
    struct wayland_output *output = wayland_surface_get_main_output(surface);
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
        int flags = surface->current.configure_flags;
        int wine_width, wine_height;
        struct wayland_shm_buffer *dummy_shm_buffer;

        /* Use a large enough width/height, so even when the target
         * surface is scaled by the compositor, this will not end up
         * being 0x0. */
        if (width == 0) width = 32;
        if (height == 0) height = 32;

        if ((flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN) &&
            !(flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED))
        {
            wayland_surface_find_wine_fullscreen_fit(surface, width, height,
                                                     &wine_width, &wine_height);
        }
        else
        {
            wayland_surface_coords_to_wine(surface, width, height,
                                           &wine_width, &wine_height);
        }

        dummy_shm_buffer = wayland_shm_buffer_create(surface->wayland,
                                                     wine_width, wine_height,
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
    LONG ref = InterlockedIncrement(&surface->ref);
    TRACE("surface=%p ref=%d->%d\n", surface, ref - 1, ref);
    return surface;
}

/**********************************************************************
 *          wayland_surface_unref
 *
 * Remove a reference to wayland_surface, potentially destroying it.
 */
void wayland_surface_unref(struct wayland_surface *surface)
{
    LONG ref = InterlockedDecrement(&surface->ref);

    TRACE("surface=%p ref=%d->%d\n", surface, ref + 1, ref);

    if (ref == 0)
        wayland_surface_destroy(surface);
}

static void wayland_surface_tree_set_main_output_and_scale(struct wayland_surface *surface,
                                                           struct wayland_output *output,
                                                           int scale)
{
    struct wayland_surface *child;

    surface->main_output = output;
    wl_surface_set_buffer_scale(surface->wl_surface, scale);

    EnterCriticalSection(&surface->crit);

    wl_list_for_each(child, &surface->child_list, link)
        wayland_surface_tree_set_main_output_and_scale(child, output, scale);

    LeaveCriticalSection(&surface->crit);
}

/**********************************************************************
 *          wayland_surface_set_main_output
 *
 * Sets the main output for a surface, i.e., the output whose scale will be
 * used for surface scaling.
 */
static void wayland_surface_set_main_output(struct wayland_surface *surface,
                                            struct wayland_output *output)
{
    /* Don't update non-toplevels. */
    if (surface->parent) return;

    TRACE("surface=%p output->name=%s (id=0x%x) => output->name=%s (id=0x%x)\n",
          surface, surface->main_output ? surface->main_output->name : NULL,
          surface->main_output ? surface->main_output->id : 0,
          output ? output->name : NULL, output ? output->id : 0);

    if (surface->main_output != output)
    {
        wayland_surface_tree_set_main_output_and_scale(surface, output,
                                                       output ? output->scale : 1);
        if (surface->hwnd)
            SendMessageW(surface->hwnd, WM_WAYLAND_SURFACE_OUTPUT_CHANGE, 0, 0);
    }
}

/**********************************************************************
 *          wayland_surface_leave_output
 *
 * Removes an output from the set of outputs a surface is presented on.
 *
 * It is OK to call this function even if the surface is not presented
 * on the specified output, in which case this function is a NOP.
 */
void wayland_surface_leave_output(struct wayland_surface *surface,
                                  struct wayland_output *output)
{
    struct wayland_output_ref *ref, *tmp;

    wl_list_for_each_safe(ref, tmp, &surface->output_ref_list, link)
    {
        if (ref->output == output)
        {
            wl_list_remove(&ref->link);
            heap_free(ref);
            break;
        }
    }

    if (surface->main_output == output)
    {
        struct wayland_output *origin =
            wayland_surface_get_origin_output(surface);

        wayland_surface_set_main_output(surface, origin);
    }
}

static void wayland_surface_tree_set_wine_output(struct wayland_surface *surface,
                                                 struct wayland_output *output)
{
    struct wayland_surface *child;

    surface->wine_output = output;

    EnterCriticalSection(&surface->crit);

    wl_list_for_each(child, &surface->child_list, link)
        wayland_surface_tree_set_wine_output(child, output);

    LeaveCriticalSection(&surface->crit);
}

/**********************************************************************
 *          wayland_surface_set_wine_output
 *
 * Sets the output which Wine considers to contain the window backed by this
 * surface. Transiently, this may be different from the output we consider to
 * be the "main" one for this surface.
 */
void wayland_surface_set_wine_output(struct wayland_surface *surface,
                                     struct wayland_output *output)
{
    /* Don't update non-toplevels. */
    if (surface->parent) return;

    TRACE("surface=%p output->name=%s => output->name=%s\n",
          surface,
          surface->wine_output ? surface->wine_output->name : NULL,
          output ? output->name : NULL);

    if (surface->wine_output != output)
        wayland_surface_tree_set_wine_output(surface, output);
}

/**********************************************************************
 *          wayland_surface_get_buffer_scale
 *
 */
int wayland_surface_get_buffer_scale(struct wayland_surface *surface)
{
    /* Use the toplevel surface to get the scale */
    struct wayland_surface *toplevel = surface;
    struct wayland_output *output;
    int scale = 1;

    while (toplevel->parent) toplevel = toplevel->parent;

    output = wayland_surface_get_main_output(surface);
    if (output) scale = output->scale;

    TRACE("hwnd=%p (toplevel=%p) => scale=%d\n", surface->hwnd, toplevel->hwnd, scale);
    return scale;
}

/**********************************************************************
 *          wayland_surface_set_drawing_allowed
 */
void wayland_surface_set_drawing_allowed(struct wayland_surface *surface, BOOL allowed)
{
    __atomic_store_n(&surface->drawing_allowed, allowed, __ATOMIC_SEQ_CST);
}

/**********************************************************************
 *          wayland_surface_is_drawing_allowed
 */
BOOL wayland_surface_is_drawing_allowed(struct wayland_surface *surface)
{
    return __atomic_load_n(&surface->drawing_allowed, __ATOMIC_SEQ_CST);
}
