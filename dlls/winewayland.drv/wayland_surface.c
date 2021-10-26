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

    surface->current = surface->pending;
    xdg_surface_ack_configure(surface->xdg_surface, surface->current.serial);

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

    /* We want enter/leave events only for toplevels */
    if (!surface->xdg_toplevel) return;

    if (!output || output->wayland != surface->wayland) return;

    TRACE("hwnd=%p output->name=%s\n", surface->hwnd, output->name);

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

    /* We want enter/leave events only for toplevels */
    if (!surface->xdg_toplevel) return;

    if (!output || output->wayland != surface->wayland) return;

    TRACE("hwnd=%p output->name=%s\n", surface->hwnd, output->name);

    wayland_surface_leave_output(surface, output);
}

static const struct wl_surface_listener wl_surface_listener = {
    handle_wl_surface_enter,
    handle_wl_surface_leave,
};

/**********************************************************************
 *          wayland_surface_create_plain
 *
 * Creates a plain, role-less wayland surface.
 */
struct wayland_surface *wayland_surface_create_plain(struct wayland *wayland)
{
    struct wayland_surface *surface;

    surface = heap_alloc_zero(sizeof(*surface));
    if (!surface)
        goto err;

    TRACE("surface=%p\n", surface);

    InitializeCriticalSection(&surface->crit);
    surface->crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": wayland_surface");

    surface->wayland = wayland;

    surface->wl_surface = wl_compositor_create_surface(wayland->wl_compositor);
    if (!surface->wl_surface)
        goto err;

    wl_list_init(&surface->output_ref_list);
    wl_list_init(&surface->link);
    wl_list_init(&surface->child_list);
    wl_surface_add_listener(surface->wl_surface, &wl_surface_listener, surface);
    /* Plain surfaces are unmappable, so don't draw on them. */
    surface->drawing_allowed = FALSE;

    surface->ref = 1;
    surface->role = WAYLAND_SURFACE_ROLE_NONE;

    /* Although not technically toplevel, plain surfaces have no parent, so
     * track them in the toplevel list. */
    wl_list_insert(&wayland->toplevel_list, &surface->link);

    return surface;

err:
    if (surface)
        wayland_surface_destroy(surface);
    return NULL;
}

/**********************************************************************
 *          wayland_surface_make_toplevel
 *
 * Gives the toplevel role to a plain wayland surface, optionally associated
 * with a parent surface.
 */
void wayland_surface_make_toplevel(struct wayland_surface *surface,
                                   struct wayland_surface *parent)
{
    struct wayland *wayland = surface->wayland;

    TRACE("surface=%p parent=%p\n", surface, parent);

    surface->drawing_allowed = TRUE;

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

    /* Plain surfaces (which are the only kind can become toplevel) are
     * already tracked in the toplevel_list, there is no need to readd. */

    wl_surface_commit(surface->wl_surface);

    surface->role = WAYLAND_SURFACE_ROLE_TOPLEVEL;

    /* Wait for the first configure event. */
    while (!surface->current.serial)
        wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);

    return;

err:
    if (surface->xdg_surface)
    {
        xdg_surface_destroy(surface->xdg_surface);
        surface->xdg_surface = NULL;
    }
    ERR("Failed to assign toplevel role to wayland surface\n");
}

/**********************************************************************
 *          wayland_surface_create_subsurface
 *
 * Assigns the subsurface role to a plain wayland surface, with the specified
 * parent.
 */
void wayland_surface_make_subsurface(struct wayland_surface *surface,
                                     struct wayland_surface *parent)
{
    struct wayland *wayland = surface->wayland;

    TRACE("surface=%p parent=%p\n", surface, parent);

    surface->drawing_allowed = TRUE;

    surface->parent = wayland_surface_ref(parent);

    /* Remove from toplevel_list (added as a plain surface) and add to parent
     * child list. */
    wl_list_remove(&surface->link);
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

    wl_surface_commit(surface->wl_surface);

    surface->role = WAYLAND_SURFACE_ROLE_SUBSURFACE;

    return;

err:
    wayland_surface_unref(surface->parent);
    surface->parent = NULL;
    ERR("Failed to assign subsurface role to wayland surface\n");
}

/**********************************************************************
 *          wayland_surface_clear_role
 *
 * Clears the role related Wayland objects of a Wayland surface, making it a
 * plain surface again. We can later assign the same role (but not a
 * different one!) to the surface.
 */
void wayland_surface_clear_role(struct wayland_surface *surface)
{
    TRACE("surface=%p hwnd=%p\n", surface, surface->hwnd);

    surface->drawing_allowed = FALSE;

    if (surface->parent)
    {
        /* As a plain surface, this should now tracked in the toplevel_list. */
        EnterCriticalSection(&surface->parent->crit);
        wl_list_remove(&surface->link);
        LeaveCriticalSection(&surface->parent->crit);
        wl_list_insert(&surface->wayland->toplevel_list, &surface->link);

        wayland_surface_unref(surface->parent);
        surface->parent = NULL;
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

    memset(&surface->pending, 0, sizeof(surface->pending));
    memset(&surface->current, 0, sizeof(surface->current));

    /* We need to unmap, otherwise future role assignments may fail. */
    wayland_surface_unmap(surface);
}

/**********************************************************************
 *          wayland_surface_reconfigure_position
 *
 * Configures the position of a wayland surface relative to its parent.
 * This only applies to surfaces having the subsurface role.
 *
 * The coordinates should be given in wine's coordinate space.
 *
 * This function sets up but doesn't actually apply any new configuration.
 * The wayland_surface_reconfigure_apply() needs to be called for changes
 * to take effect.
 */
void wayland_surface_reconfigure_position(struct wayland_surface *surface,
                                          int wine_x, int wine_y)
{
    int x, y;

    wayland_surface_coords_rounded_from_wine(surface, wine_x, wine_y, &x, &y);

    TRACE("surface=%p hwnd=%p wine=%d,%d wayland=%d,%d\n",
          surface, surface->hwnd, wine_x, wine_y, x, y);

    if (surface->wl_subsurface)
        wl_subsurface_set_position(surface->wl_subsurface, x, y);
}

/**********************************************************************
 *          wayland_surface_reconfigure_geometry
 *
 * Configures the geometry of a wayland surface, i.e., the rectangle
 * within that surface that contains the surface's visible bounds.
 *
 * The coordinates and sizes should be given in wine's coordinate space.
 *
 * This function sets up but doesn't actually apply any new configuration.
 * The wayland_surface_reconfigure_apply() needs to be called for changes
 * to take effect.
 */
void wayland_surface_reconfigure_geometry(struct wayland_surface *surface,
                                          int wine_x, int wine_y,
                                          int wine_width, int wine_height)
{
    int x, y, width, height;

    wayland_surface_coords_rounded_from_wine(surface, wine_x, wine_y, &x, &y);
    wayland_surface_coords_rounded_from_wine(surface, wine_width, wine_height,
                                             &width, &height);

    TRACE("surface=%p hwnd=%p wine=%d,%d+%dx%d wayland=%d,%d+%dx%d\n",
          surface, surface->hwnd,
          wine_x, wine_y, wine_width, wine_height,
          x, y, width, height);

    if (surface->xdg_surface && width != 0 && height != 0)
    {
        enum wayland_configure_flags flags = surface->current.configure_flags;

        /* Sometimes rounding errors in our coordinate space transformations
         * can lead to invalid geometry values, so enforce acceptable geometry
         * values to avoid causing a protocol error. */
        if (flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED)
        {
            width = surface->current.width;
            height = surface->current.height;
        }
        else if (flags & WAYLAND_CONFIGURE_FLAG_FULLSCREEN)
        {
            if (width > surface->current.width)
                width = surface->current.width;
            if (height > surface->current.height)
                height = surface->current.height;
        }

        xdg_surface_set_window_geometry(surface->xdg_surface, x, y, width, height);
    }
}

/**********************************************************************
 *          wayland_surface_reconfigure_apply
 *
 * Applies the configuration set by previous calls to the
 * wayland_surface_reconfigure{_glvk}() functions.
 */
void wayland_surface_reconfigure_apply(struct wayland_surface *surface)
{
    wl_surface_commit(surface->wl_surface);

    /* Commit the parent so any subsurface repositioning takes effect. */
    if (surface->parent)
        wl_surface_commit(surface->parent->wl_surface);
}

/**********************************************************************
 *          wayland_surface_configure_is_compatible
 *
 * Checks whether a wayland_surface_configure object is compatible with the
 * the provided arguments.
 */
BOOL wayland_surface_configure_is_compatible(struct wayland_surface_configure *conf,
                                             int width, int height,
                                             enum wayland_configure_flags flags)
{
    static int mask = WAYLAND_CONFIGURE_FLAG_MAXIMIZED |
                      WAYLAND_CONFIGURE_FLAG_FULLSCREEN;

    /* We require the same state. */
    if ((flags & mask) != (conf->configure_flags & mask))
        return FALSE;

    /* The maximized state requires the configured size. During surface
     * reconfiguration we can use surface geometry to provide smaller areas
     * from larger sizes, so only smaller sizes are incompatible. */
    if ((conf->configure_flags & WAYLAND_CONFIGURE_FLAG_MAXIMIZED) &&
        (width < conf->width || height < conf->height))
    {
        return FALSE;
    }

    /* The fullscreen state requires sizes smaller or equal to the configured
     * size. We can provide this during surface reconfiguration using surface
     * geometry, so we are always compatible with a fullscreen state. */

    return TRUE;
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
 *          wayland_surface_commit_buffer
 *
 * Commits a SHM buffer on a wayland surface. Returns whether the
 * buffer was actually committed.
 */
BOOL wayland_surface_commit_buffer(struct wayland_surface *surface,
                                   struct wayland_shm_buffer *shm_buffer,
                                   HRGN surface_damage_region)
{
    RGNDATA *surface_damage;
    int wayland_width, wayland_height;

    /* Since multiple threads can commit a buffer to a wayland surface
     * (e.g., child windows in different threads), we guard this function
     * to ensure we get complete and atomic buffer commits. */
    EnterCriticalSection(&surface->crit);

    TRACE("surface=%p (%dx%d) flags=%#x buffer=%p (%dx%d)\n",
          surface, surface->current.width, surface->current.height,
          surface->current.configure_flags, shm_buffer,
          shm_buffer->width, shm_buffer->height);

    wayland_surface_coords_rounded_from_wine(surface,
                                             shm_buffer->width, shm_buffer->height,
                                             &wayland_width, &wayland_height);

    /* Certain surface states are very strict about the dimensions of buffers
     * they accept. To avoid wayland protocol errors, drop buffers not matching
     * the expected dimensions of such surfaces. This typically happens
     * transiently during resizing operations. */
    if (!surface->drawing_allowed ||
        !wayland_surface_configure_is_compatible(&surface->current,
                                             wayland_width,
                                             wayland_height,
                                             surface->current.configure_flags))
    {
        LeaveCriticalSection(&surface->crit);
        TRACE("surface=%p buffer=%p dropping buffer\n", surface, shm_buffer);
        shm_buffer->busy = FALSE;
        return FALSE;
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

    return TRUE;
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

    *screen_x = wine_x + window_rect.left;
    *screen_y = wine_y + window_rect.top;

    TRACE("hwnd=%p wayland=%.2f,%.2f rect=%s => screen=%d,%d\n",
          surface->hwnd, wayland_x, wayland_y, wine_dbgstr_rect(&window_rect),
          *screen_x, *screen_y);
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
    *wayland_x = wine_x;
    *wayland_y = wine_y;
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
    *wine_x = round(wayland_x);
    *wine_y = round(wayland_y);
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

static void wayland_surface_tree_set_main_output(struct wayland_surface *surface,
                                                 struct wayland_output *output)
{
    struct wayland_surface *child;

    surface->main_output = output;

    EnterCriticalSection(&surface->crit);

    wl_list_for_each(child, &surface->child_list, link)
        wayland_surface_tree_set_main_output(child, output);

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

    TRACE("surface=%p output->name=%s => output->name=%s\n",
          surface,
          surface->main_output ? surface->main_output->name : NULL,
          output ? output->name : NULL);

    if (surface->main_output != output)
    {
        wayland_surface_tree_set_main_output(surface, output);
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
