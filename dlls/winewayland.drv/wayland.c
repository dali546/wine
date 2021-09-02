/*
 * Wayland core handling
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

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

struct wl_display *process_wl_display = NULL;

/**********************************************************************
 *          Registry handling
 */

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t id, const char *interface,
                                   uint32_t version)
{
    struct wayland *wayland = data;

    TRACE("interface=%s version=%d\n id=%u\n", interface, version, id);

    if (strcmp(interface, "wl_compositor") == 0)
    {
        wayland->wl_compositor =
            wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    }
    else if (strcmp(interface, "wl_output") == 0)
    {
        if (!wayland_output_create(wayland, id, version))
            ERR("Failed to create wayland_output for global id=%u\n", id);
    }
    else if (strcmp(interface, "zxdg_output_manager_v1") == 0)
    {
        struct wayland_output *output;

        wayland->zxdg_output_manager_v1 =
            wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface,
                             version < 3 ? version : 3);

        /* Add zxdg_output_v1 to existing outputs. */
        wl_list_for_each(output, &wayland->output_list, link)
            wayland_output_use_xdg_extension(output);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
                                          uint32_t id)
{
    struct wayland *wayland = data;
    struct wayland_output *output, *tmp;

    TRACE("id=%d\n", id);

    wl_list_for_each_safe(output, tmp, &wayland->output_list, link)
    {
        if (output->global_id == id)
        {
            TRACE("removing output->name=%s\n", output->name);
            wayland_output_destroy(output);
            wayland_init_display_devices(wayland);
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

/**********************************************************************
 *          wayland_init
 *
 *  Initialise a wayland instance.
 */
BOOL wayland_init(struct wayland *wayland)
{
    TRACE("wayland=%p wl_display=%p\n", wayland, process_wl_display);

    wayland->process_id = GetCurrentProcessId();
    wayland->thread_id = GetCurrentThreadId();
    wayland->wl_display = process_wl_display;

    if (!wayland->wl_display)
    {
        ERR("Failed to connect to wayland compositor\n");
        return FALSE;
    }

    if (!(wayland->wl_event_queue = wl_display_create_queue(wayland->wl_display)))
    {
        ERR("Failed to create event queue\n");
        return FALSE;
    }

    if (!(wayland->wl_registry = wl_display_get_registry(wayland->wl_display)))
    {
        ERR("Failed to get to wayland registry\n");
        return FALSE;
    }
    wl_proxy_set_queue((struct wl_proxy *) wayland->wl_registry, wayland->wl_event_queue);

    wl_list_init(&wayland->output_list);

    /* Populate registry */
    wl_registry_add_listener(wayland->wl_registry, &registry_listener, wayland);

    /* We need three roundtrips. One to get and bind globals, one to handle all
     * initial events produced from registering the globals and one more to
     * handle potential third-order registrations. */
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);

    wayland->initialized = TRUE;

    return TRUE;
}

/**********************************************************************
 *          wayland_deinit
 *
 *  Deinitialise a wayland instance, releasing all associated resources.
 */
void wayland_deinit(struct wayland *wayland)
{
    struct wayland_output *output, *output_tmp;

    TRACE("%p\n", wayland);

    wl_list_for_each_safe(output, output_tmp, &wayland->output_list, link)
        wayland_output_destroy(output);

    if (wayland->zxdg_output_manager_v1)
        zxdg_output_manager_v1_destroy(wayland->zxdg_output_manager_v1);

    if (wayland->wl_compositor)
        wl_compositor_destroy(wayland->wl_compositor);

    if (wayland->wl_registry)
        wl_registry_destroy(wayland->wl_registry);

    if (wayland->wl_event_queue)
        wl_event_queue_destroy(wayland->wl_event_queue);

    wl_display_flush(wayland->wl_display);

    memset(wayland, 0, sizeof(*wayland));
}

/**********************************************************************
 *          wayland_process_init
 *
 *  Initialise the per process wayland objects.
 *
 */
BOOL wayland_process_init(void)
{
    process_wl_display = wl_display_connect(NULL);
    return process_wl_display != NULL;
}
