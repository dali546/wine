/* WAYLANDDRV Vulkan implementation
 *
 * Copyright 2017 Roderick Colenbrander
 * Copyright 2021 Alexandros Frantzis
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
#include "wine/port.h"

#include "waylanddrv.h"

#include "wine/debug.h"

#define VK_NO_PROTOTYPES
#define WINE_VK_HOST

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

#ifdef SONAME_LIBVULKAN

static void *vulkan_handle;

static BOOL WINAPI wine_vk_init(INIT_ONCE *once, void *param, void **context)
{
    if (!(vulkan_handle = dlopen(SONAME_LIBVULKAN, RTLD_NOW)))
    {
        ERR("Failed to load %s.\n", SONAME_LIBVULKAN);
        return TRUE;
    }

    return TRUE;
}

static const struct vulkan_funcs vulkan_funcs;

const struct vulkan_funcs *wayland_get_vulkan_driver(UINT version)
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR("version mismatch, vulkan wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION);
        return NULL;
    }

    InitOnceExecuteOnce(&init_once, wine_vk_init, NULL, NULL);
    if (vulkan_handle)
        return &vulkan_funcs;

    return NULL;
}

#else /* No vulkan */

const struct vulkan_funcs *wayland_get_vulkan_driver(UINT version)
{
    ERR("Wine was built without Vulkan support.\n");
    return NULL;
}

#endif /* SONAME_LIBVULKAN */
