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

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static struct wayland_surface *wayland_surface_create_common(struct wayland *wayland)
{
    struct wayland_surface *surface;

    surface = heap_alloc_zero(sizeof(*surface));
    if (!surface)
        goto err;

    surface->wayland = wayland;

    surface->wl_surface = wl_compositor_create_surface(wayland->wl_compositor);
    if (!surface->wl_surface)
        goto err;

    wl_surface_set_user_data(surface->wl_surface, surface);

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

    surface->xdg_surface =
        xdg_wm_base_get_xdg_surface(wayland->xdg_wm_base, surface->wl_surface);
    if (!surface->xdg_surface)
        goto err;

    surface->xdg_toplevel = xdg_surface_get_toplevel(surface->xdg_surface);
    if (!surface->xdg_toplevel)
        goto err;

    if (parent && parent->xdg_toplevel)
        xdg_toplevel_set_parent(surface->xdg_toplevel, parent->xdg_toplevel);

    wl_surface_commit(surface->wl_surface);

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
 *          wayland_surface_destroy
 *
 * Destroys a wayland surface.
 */
void wayland_surface_destroy(struct wayland_surface *surface)
{
    TRACE("surface=%p\n", surface);

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
        wayland_surface_unref(surface->parent);
        surface->parent = NULL;
    }

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

    heap_free(surface);
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
