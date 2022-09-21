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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "waylanddrv.h"

#include "wine/debug.h"

#include "ntuser.h"

#define VK_NO_PROTOTYPES
#define WINE_VK_HOST

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"
#include "vulkan_remote.h"

#include <dlfcn.h>
#include <stdlib.h>

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

#ifdef SONAME_LIBVULKAN

#define VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR 1000006000

/* This is temporary until we fully support remote (cross-process) Vulkan
 * rendering, as we are progressively adding commits that add the support. */
static const BOOL remote_rendering_supported = FALSE;

typedef struct VkWaylandSurfaceCreateInfoKHR
{
    VkStructureType sType;
    const void *pNext;
    VkWaylandSurfaceCreateFlagsKHR flags;
    struct wl_display *display;
    struct wl_surface *surface;
} VkWaylandSurfaceCreateInfoKHR;

static VkResult (*pvkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
static VkResult (*pvkCreateInstance)(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
static VkResult (*pvkCreateSwapchainKHR)(VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *, VkSwapchainKHR *);
static VkResult (*pvkCreateWaylandSurfaceKHR)(VkInstance, const VkWaylandSurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *);
static void(*pvkDestroyDevice)(VkDevice, const VkAllocationCallbacks *);
static void (*pvkDestroyInstance)(VkInstance, const VkAllocationCallbacks *);
static void (*pvkDestroySurfaceKHR)(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *);
static void (*pvkDestroySwapchainKHR)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *);
static VkResult (*pvkEnumerateDeviceExtensionProperties)(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
static VkResult (*pvkEnumerateInstanceExtensionProperties)(const char *, uint32_t *, VkExtensionProperties *);
static VkResult (*pvkGetDeviceGroupSurfacePresentModesKHR)(VkDevice, VkSurfaceKHR, VkDeviceGroupPresentModeFlagsKHR *);
static void * (*pvkGetDeviceProcAddr)(VkDevice, const char *);
static void * (*pvkGetInstanceProcAddr)(VkInstance, const char *);
static VkResult (*pvkGetPhysicalDevicePresentRectanglesKHR)(VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkRect2D *);
static VkResult (*pvkGetPhysicalDeviceSurfaceCapabilities2KHR)(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR *, VkSurfaceCapabilities2KHR *);
static VkResult (*pvkGetPhysicalDeviceSurfaceCapabilitiesKHR)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *);
static VkResult (*pvkGetPhysicalDeviceSurfaceFormats2KHR)(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR *, uint32_t *, VkSurfaceFormat2KHR *);
static VkResult (*pvkGetPhysicalDeviceSurfaceFormatsKHR)(VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *);
static VkResult (*pvkGetPhysicalDeviceSurfacePresentModesKHR)(VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkPresentModeKHR *);
static VkResult (*pvkGetPhysicalDeviceSurfaceSupportKHR)(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *);
static VkBool32 (*pvkGetPhysicalDeviceWaylandPresentationSupportKHR)(VkPhysicalDevice, uint32_t, struct wl_display *);
static VkResult (*pvkGetSwapchainImagesKHR)(VkDevice, VkSwapchainKHR, uint32_t *, VkImage *);
static VkResult (*pvkQueuePresentKHR)(VkQueue, const VkPresentInfoKHR *);

static void *vulkan_handle;

static struct wayland_mutex wine_vk_object_mutex =
{
    PTHREAD_MUTEX_INITIALIZER, 0, 0, __FILE__ ": wine_vk_object_mutex"
};

static struct wl_list wine_vk_surface_list = { &wine_vk_surface_list, &wine_vk_surface_list };
static struct wl_list wine_vk_swapchain_list = { &wine_vk_swapchain_list, &wine_vk_swapchain_list };
static struct wl_list wine_vk_device_list = { &wine_vk_device_list, &wine_vk_device_list };

static const struct vulkan_funcs vulkan_funcs;

/* These instance extensions are required to support Vulkan remote. Some of them
 * might not be supported by the device, so we must check. */
const static char *instance_extensions_remote_vulkan[] =
{
};

/* These device extensions are required to support Vulkan remote. Some of them
 * might not be supported by the device, so we must check. */
const static char *device_extensions_remote_vulkan[] =
{
};

struct wine_vk_device
{
    struct wl_list link;
    VkDevice dev;
    VkPhysicalDevice phys_dev;
    BOOL supports_remote_vulkan;
};

struct wine_vk_surface
{
    struct wl_list link;
    HWND hwnd;
    VkInstance instance;
    struct wayland_surface *wayland_surface;
    /* Used when we are rendering cross-process and we don't have the real
     * wayland surface available. */
    struct wl_surface *dummy_wl_surface;
    VkSurfaceKHR native_vk_surface;
    BOOL valid;
};

struct wine_vk_swapchain
{
    struct wl_list link;
    HWND hwnd;
    struct wine_vk_device *wine_vk_device;
    struct wayland_surface *wayland_surface;
    VkSwapchainKHR native_vk_swapchain;
    VkExtent2D extent;
    BOOL valid;
    /* Only used for cross-process Vulkan rendering apps. */
    struct wayland_remote_vk_swapchain *remote_vk_swapchain;
};

static inline void wine_vk_list_add(struct wl_list *list, struct wl_list *link)
{
    wayland_mutex_lock(&wine_vk_object_mutex);
    wl_list_insert(list, link);
    wayland_mutex_unlock(&wine_vk_object_mutex);
}

static inline void wine_vk_list_remove(struct wl_list *link)
{
    wayland_mutex_lock(&wine_vk_object_mutex);
    wl_list_remove(link);
    wayland_mutex_unlock(&wine_vk_object_mutex);
}

static void wine_vk_surface_destroy(struct wine_vk_surface *wine_vk_surface)
{
    wine_vk_list_remove(&wine_vk_surface->link);

    if (wine_vk_surface->wayland_surface)
        wayland_surface_unref_glvk(wine_vk_surface->wayland_surface);
    if (wine_vk_surface->dummy_wl_surface)
        wl_surface_destroy(wine_vk_surface->dummy_wl_surface);

    free(wine_vk_surface);
}

static struct wine_vk_surface *wine_vk_surface_from_handle(VkSurfaceKHR handle)
{
    struct wine_vk_surface *surf;

    wayland_mutex_lock(&wine_vk_object_mutex);

    wl_list_for_each(surf, &wine_vk_surface_list, link)
        if (surf->native_vk_surface == handle) goto out;

    surf = NULL;

out:
    wayland_mutex_unlock(&wine_vk_object_mutex);
    return surf;
}

static BOOL wine_vk_surface_handle_is_valid(VkSurfaceKHR handle)
{
    struct wine_vk_surface *wine_vk_surface = wine_vk_surface_from_handle(handle);
    return wine_vk_surface && __atomic_load_n(&wine_vk_surface->valid, __ATOMIC_SEQ_CST);
}

static void wine_vk_swapchain_destroy(struct wine_vk_swapchain *wine_vk_swapchain)
{
    wine_vk_list_remove(&wine_vk_swapchain->link);

    if (wine_vk_swapchain->wayland_surface)
        wayland_surface_unref_glvk(wine_vk_swapchain->wayland_surface);

    if (wine_vk_swapchain->remote_vk_swapchain)
        wayland_remote_vk_swapchain_destroy(wine_vk_swapchain->remote_vk_swapchain,
                                            wine_vk_swapchain->wine_vk_device->dev);

    free(wine_vk_swapchain);
}

static struct wine_vk_swapchain *wine_vk_swapchain_from_handle(VkSurfaceKHR handle)
{
    struct wine_vk_swapchain *swap;

    wayland_mutex_lock(&wine_vk_object_mutex);

    wl_list_for_each(swap, &wine_vk_swapchain_list, link)
        if (swap->native_vk_swapchain == handle) goto out;

    swap = NULL;

out:
    wayland_mutex_unlock(&wine_vk_object_mutex);
    return swap;
}

static BOOL wine_vk_swapchain_is_remote(struct wine_vk_swapchain *wine_vk_swapchain)
{
    return wine_vk_swapchain && wine_vk_swapchain->remote_vk_swapchain;
}

static BOOL vk_extension_props_contain_all(uint32_t count_props,
                                           VkExtensionProperties *props,
                                           uint32_t count_required,
                                           const char * const *required)
{
    BOOL supported;
    unsigned int i, j;

    for (i = 0; i < count_required; i++)
    {
        supported = FALSE;
        for (j = 0; j < count_props; j++)
        {
            if (strcmp(props[j].extensionName, required[i]) == 0)
            {
                supported = TRUE;
                break;
            }
        }
        if (!supported)
            return FALSE;
    }

    return TRUE;
}

static BOOL vulkan_instance_supports(size_t num_exts, const char **exts)
{
    VkExtensionProperties *props = NULL;
    uint32_t count_props;
    VkResult vk_res;
    BOOL res = TRUE;

    vk_res = pvkEnumerateInstanceExtensionProperties(NULL, &count_props, NULL);
    if (vk_res != VK_SUCCESS)
    {
        ERR("pvkEnumerateInstanceExtensionProperties failed, res=%d\n", vk_res);
        res = FALSE;
        goto out;
    }
    props = calloc(count_props, sizeof(*props));
    if (!props)
    {
        ERR("Failed to allocate memory\n");
        res = FALSE;
        goto out;
    }
    vk_res = pvkEnumerateInstanceExtensionProperties(NULL, &count_props, props);
    if (vk_res != VK_SUCCESS)
    {
        ERR("pvkEnumerateInstanceExtensionProperties failed, res=%d\n", vk_res);
        res = FALSE;
        goto out;
    }

    /* These extensions are required to support the remote Vulkan, but may
     * not be present. */
    res = vk_extension_props_contain_all(count_props, props, num_exts, exts);

out:
    free(props);
    return res;
}

/* Helper function for converting between win32 and Wayland compatible VkInstanceCreateInfo.
 * Caller is responsible for allocation and cleanup of 'dst'.
 */
static VkResult wine_vk_instance_convert_create_info(const VkInstanceCreateInfo *src,
                                                     VkInstanceCreateInfo *dst)
{
    BOOL supports_remote_vulkan =
        vulkan_instance_supports(ARRAY_SIZE(instance_extensions_remote_vulkan),
                                            instance_extensions_remote_vulkan);
    unsigned int i, j;
    uint32_t enabled_extensions_count;
    const char **enabled_extensions = NULL;

    dst->sType = src->sType;
    dst->flags = src->flags;
    dst->pApplicationInfo = src->pApplicationInfo;
    dst->pNext = src->pNext;
    dst->enabledLayerCount = 0;
    dst->ppEnabledLayerNames = NULL;
    dst->enabledExtensionCount = 0;
    dst->ppEnabledExtensionNames = NULL;

    if (src->enabledExtensionCount > 0)
    {
        enabled_extensions_count = src->enabledExtensionCount;
        if (supports_remote_vulkan)
            enabled_extensions_count += ARRAY_SIZE(instance_extensions_remote_vulkan);

        enabled_extensions = calloc(enabled_extensions_count, sizeof(*src->ppEnabledExtensionNames));
        if (!enabled_extensions)
        {
            ERR("Failed to allocate memory for enabled extensions\n");
            goto err;
        }

        for (i = 0; i < src->enabledExtensionCount; i++)
        {
            /* Substitute extension with Wayland ones else copy. Long-term, when we
             * support more extensions, we should store these in a list.
             */
            if (!strcmp(src->ppEnabledExtensionNames[i], "VK_KHR_win32_surface"))
                enabled_extensions[i] = "VK_KHR_wayland_surface";
            else
                enabled_extensions[i] = src->ppEnabledExtensionNames[i];
        }

        if (supports_remote_vulkan)
        {
            /* Add the extensions required to support remote Vulkan */
            for (j = 0; j < ARRAY_SIZE(instance_extensions_remote_vulkan); j++, i++)
                enabled_extensions[i] = instance_extensions_remote_vulkan[j];
        }

         dst->ppEnabledExtensionNames = enabled_extensions;
         dst->enabledExtensionCount = enabled_extensions_count;
    }

    return VK_SUCCESS;

err:
    ERR("Failed to convert instance create info\n");
    free(enabled_extensions);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

#define RETURN_VK_ERROR_SURFACE_LOST_KHR { \
    TRACE("VK_ERROR_SURFACE_LOST_KHR\n"); \
    return VK_ERROR_SURFACE_LOST_KHR; \
}

static VkResult wayland_vkCreateInstance(const VkInstanceCreateInfo *create_info,
                                         const VkAllocationCallbacks *allocator,
                                         VkInstance *instance)
{
    VkInstanceCreateInfo create_info_host;
    VkResult res;
    TRACE("create_info %p, allocator %p, instance %p\n", create_info, allocator, instance);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    /* Perform a second pass on converting VkInstanceCreateInfo. Winevulkan
     * performed a first pass in which it handles everything except for WSI
     * functionality such as VK_KHR_win32_surface. Handle this now.
     */
    res = wine_vk_instance_convert_create_info(create_info, &create_info_host);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to convert instance create info, res=%d\n", res);
        return res;
    }

    res = pvkCreateInstance(&create_info_host, NULL /* allocator */, instance);

    free((void *)create_info_host.ppEnabledExtensionNames);
    return res;
}

static VkResult wine_vk_device_convert_create_info(struct wine_vk_device *wine_vk_device,
                                                   const VkDeviceCreateInfo *src,
                                                   VkDeviceCreateInfo *dst)
{
    unsigned int i, j;
    uint32_t enabled_extensions_count;
    const char **enabled_extensions = NULL;

    dst->sType = src->sType;
    dst->flags = src->flags;
    dst->pNext = src->pNext;
    dst->enabledLayerCount = 0;
    dst->ppEnabledLayerNames = NULL;
    dst->enabledExtensionCount = 0;
    dst->ppEnabledExtensionNames = NULL;
    dst->pEnabledFeatures = src->pEnabledFeatures;
    dst->pQueueCreateInfos = src->pQueueCreateInfos;
    dst->queueCreateInfoCount = src->queueCreateInfoCount;

    if (src->enabledExtensionCount > 0)
    {
        enabled_extensions_count = src->enabledExtensionCount;
        if (wine_vk_device->supports_remote_vulkan)
            enabled_extensions_count += ARRAY_SIZE(device_extensions_remote_vulkan);

        enabled_extensions = calloc(enabled_extensions_count, sizeof(*src->ppEnabledExtensionNames));
        if (!enabled_extensions)
        {
            ERR("Failed to allocate memory for enabled extensions\n");
            goto err;
        }

        for (i = 0; i < src->enabledExtensionCount; i++)
            enabled_extensions[i] = src->ppEnabledExtensionNames[i];

        if (wine_vk_device->supports_remote_vulkan)
        {
            /* Add the extensions required to support remote Vulkan */
            for (j = 0; j < ARRAY_SIZE(device_extensions_remote_vulkan); j++, i++)
                enabled_extensions[i] = device_extensions_remote_vulkan[j];
        }

        dst->ppEnabledExtensionNames = enabled_extensions;
        dst->enabledExtensionCount = enabled_extensions_count;
    }

    return VK_SUCCESS;

err:
    ERR("Failed to convert device create info\n");
    free(enabled_extensions);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static struct wine_vk_device *wine_vk_device_from_handle(VkDevice handle)
{
    struct wine_vk_device *wine_vk_device;

    wayland_mutex_lock(&wine_vk_object_mutex);

    wl_list_for_each(wine_vk_device, &wine_vk_device_list, link)
        if (wine_vk_device->dev == handle) goto out;

    wine_vk_device = NULL;

out:
    wayland_mutex_unlock(&wine_vk_object_mutex);
    return wine_vk_device;
}

static void wine_vk_device_destroy(struct wine_vk_device *wine_vk_device)
{
    wine_vk_list_remove(&wine_vk_device->link);
    free(wine_vk_device);
}

static VkResult wayland_vkCreateDevice(VkPhysicalDevice physical_device,
                                       const VkDeviceCreateInfo *create_info,
                                       const VkAllocationCallbacks *allocator,
                                       VkDevice *device)
{
    VkDeviceCreateInfo create_info_host = {0};
    VkResult res;
    struct wine_vk_device *wine_vk_device;
    VkExtensionProperties *props = NULL;
    uint32_t count_props;

    TRACE("%p %p %p %p\n", physical_device, create_info, allocator, device);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    wine_vk_device = calloc(1, sizeof(*wine_vk_device));
    if (!wine_vk_device)
    {
        ERR("Failed to allocate memory\n");
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto err;
    }

    res = pvkEnumerateDeviceExtensionProperties(physical_device, NULL, &count_props, NULL);
    if (res != VK_SUCCESS)
    {
        ERR("pvkEnumerateDeviceExtensionProperties failed, res=%d\n", res);
        goto err;
    }
    props = calloc(count_props, sizeof(*props));
    if (!props)
    {
        ERR("Failed to allocate memory\n");
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto err;
    }
    res = pvkEnumerateDeviceExtensionProperties(physical_device, NULL, &count_props, props);
    if (res != VK_SUCCESS)
    {
        ERR("pvkEnumerateDeviceExtensionProperties failed, res=%d\n", res);
        goto err;
    }

    wine_vk_device->supports_remote_vulkan =
        vk_extension_props_contain_all(count_props, props,
                                       ARRAY_SIZE(device_extensions_remote_vulkan),
                                       device_extensions_remote_vulkan) &&
        vulkan_instance_supports(ARRAY_SIZE(instance_extensions_remote_vulkan),
                                 instance_extensions_remote_vulkan);

    free(props);
    props = NULL;

    res = wine_vk_device_convert_create_info(wine_vk_device, create_info, &create_info_host);
    if (res != VK_SUCCESS)
        goto err;

    res = pvkCreateDevice(physical_device, &create_info_host, NULL /* allocator */, device);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to create VkDevice, res=%d\n", res);
        goto err;
    }

    wine_vk_device->dev = *device;
    wine_vk_device->phys_dev = physical_device;

    wl_list_init(&wine_vk_device->link);

    wine_vk_list_add(&wine_vk_device_list, &wine_vk_device->link);

    free((void *)create_info_host.ppEnabledExtensionNames);
    return res;

err:
    ERR("Failed to create VkDevice\n");
    free(wine_vk_device);
    free((void *)create_info_host.ppEnabledExtensionNames);
    return res;
}

static VkResult wayland_vkCreateSwapchainKHR(VkDevice device,
                                             const VkSwapchainCreateInfoKHR *create_info,
                                             const VkAllocationCallbacks *allocator,
                                             VkSwapchainKHR *swapchain)
{
    VkResult res;
    struct wine_vk_device *wine_vk_device;
    struct wine_vk_surface *wine_vk_surface;
    struct wine_vk_swapchain *wine_vk_swapchain;
    VkSwapchainCreateInfoKHR info = *create_info;

    TRACE("%p %p %p %p\n", device, create_info, allocator, swapchain);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    /* Wayland can't deal with 0x0 swapchains, use the minimum 1x1. */
    if (info.imageExtent.width == 0)
        info.imageExtent.width = 1;
    if (info.imageExtent.height == 0)
        info.imageExtent.height = 1;

    wine_vk_device = wine_vk_device_from_handle(device);
    if (!wine_vk_device)
        return VK_ERROR_DEVICE_LOST;

    wine_vk_surface = wine_vk_surface_from_handle(info.surface);
    if (!wine_vk_surface || !__atomic_load_n(&wine_vk_surface->valid, __ATOMIC_SEQ_CST))
        RETURN_VK_ERROR_SURFACE_LOST_KHR;

    wine_vk_swapchain = calloc(1, sizeof(*wine_vk_swapchain));
    if (!wine_vk_swapchain)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    wl_list_init(&wine_vk_swapchain->link);

    res = pvkCreateSwapchainKHR(device, &info, NULL /* allocator */, swapchain);
    if (res != VK_SUCCESS)
        goto err;

    wine_vk_swapchain->hwnd = wine_vk_surface->hwnd;
    if (wine_vk_surface->wayland_surface)
    {
        if (!wayland_surface_create_or_ref_glvk(wine_vk_surface->wayland_surface))
        {
            ERR("Failed to create or ref vulkan surface owned by " \
                "wine_vk_surface=%p\n", wine_vk_surface);
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto err;
        }
        wine_vk_swapchain->wayland_surface = wine_vk_surface->wayland_surface;
    }
    else
    {
        if (!wine_vk_device->supports_remote_vulkan)
        {
            ERR("Failed to create remote Vulkan swapchain, required extensions " \
                "not supported by VkDevice %p\n", wine_vk_device->dev);
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto err;
        }
        wine_vk_swapchain->remote_vk_swapchain =
            wayland_remote_vk_swapchain_create(wine_vk_swapchain->hwnd,
                                               wine_vk_surface->instance,
                                               wine_vk_device->phys_dev,
                                               wine_vk_device->dev,
                                               &vulkan_funcs,
                                               &info);
        if (!wine_vk_swapchain->remote_vk_swapchain)
        {
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto err;
        }
    }

    wine_vk_swapchain->wine_vk_device = wine_vk_device;
    wine_vk_swapchain->native_vk_swapchain = *swapchain;
    wine_vk_swapchain->extent = info.imageExtent;
    wine_vk_swapchain->valid = TRUE;

    wine_vk_list_add(&wine_vk_swapchain_list, &wine_vk_swapchain->link);

    return res;

err:
    wine_vk_swapchain_destroy(wine_vk_swapchain);
    return res;
}

static VkResult wayland_vkCreateWin32SurfaceKHR(VkInstance instance,
                                                const VkWin32SurfaceCreateInfoKHR *create_info,
                                                const VkAllocationCallbacks *allocator,
                                                VkSurfaceKHR *vk_surface)
{
    VkResult res;
    VkWaylandSurfaceCreateInfoKHR create_info_host;
    struct wine_vk_surface *wine_vk_surface;
    struct wayland_surface *wayland_surface;
    BOOL ref_vk;

    TRACE("%p %p %p %p\n", instance, create_info, allocator, vk_surface);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    wine_vk_surface = calloc(1, sizeof(*wine_vk_surface));
    if (!wine_vk_surface)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    wl_list_init(&wine_vk_surface->link);

    wayland_surface = wayland_surface_for_hwnd_lock(create_info->hwnd);
    if (wayland_surface)
    {
        ref_vk = wayland_surface_create_or_ref_glvk(wayland_surface);
        wayland_surface_for_hwnd_unlock(wayland_surface);
        if (!ref_vk)
        {
            ERR("Failed to create or ref vulkan surface for hwnd=%p\n", create_info->hwnd);
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto err;
        }
        wine_vk_surface->wayland_surface = wayland_surface;
    }
    else if (remote_rendering_supported)
    {
        struct wayland *wayland;
        if (!vulkan_instance_supports(ARRAY_SIZE(instance_extensions_remote_vulkan),
                                      instance_extensions_remote_vulkan))
        {
            ERR("Failed to create remote Vulkan surface, required extensions " \
                "not supported by VkInstance %p\n", instance);
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto err;
        }
        wayland = wayland_process_acquire();
        wine_vk_surface->dummy_wl_surface =
            wl_compositor_create_surface(wayland->wl_compositor);
        wayland_process_release();
        if (!wine_vk_surface->dummy_wl_surface)
        {
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto err;
        }
    }
    else
    {
        ERR("Failed to create surface, cross-process Vulkan rendering not supported yet\n");
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto err;
    }

    create_info_host.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    create_info_host.pNext = NULL;
    create_info_host.flags = 0; /* reserved */
    create_info_host.display = process_wl_display;
    if (wine_vk_surface->wayland_surface)
        create_info_host.surface = wine_vk_surface->wayland_surface->glvk->wl_surface;
    else
        create_info_host.surface = wine_vk_surface->dummy_wl_surface;

    res = pvkCreateWaylandSurfaceKHR(instance, &create_info_host, NULL /* allocator */, vk_surface);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to create vulkan wayland surface, res=%d\n", res);
        goto err;
    }

    wine_vk_surface->hwnd = create_info->hwnd;
    wine_vk_surface->instance = instance;
    wine_vk_surface->native_vk_surface = *vk_surface;
    wine_vk_surface->valid = TRUE;

    wine_vk_list_add(&wine_vk_surface_list, &wine_vk_surface->link);

    TRACE("Created surface=0x%s\n", wine_dbgstr_longlong(*vk_surface));
    return VK_SUCCESS;

err:
    wine_vk_surface_destroy(wine_vk_surface);
    return res;
}

static void wayland_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator)
{
    TRACE("%p %p\n", instance, allocator);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    pvkDestroyInstance(instance, NULL /* allocator */);
}

static void wayland_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                        const VkAllocationCallbacks *allocator)
{
    struct wine_vk_surface *wine_vk_surface = wine_vk_surface_from_handle(surface);

    TRACE("%p 0x%s %p\n", instance, wine_dbgstr_longlong(surface), allocator);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    if (wine_vk_surface)
    {
        pvkDestroySurfaceKHR(instance, wine_vk_surface->native_vk_surface,
                             NULL /* allocator */);
        wine_vk_surface_destroy(wine_vk_surface);
    }
}

static void wayland_vkDestroySwapchainKHR(VkDevice device,
                                          VkSwapchainKHR swapchain,
                                          const VkAllocationCallbacks *allocator)
{
    struct wine_vk_swapchain *wine_vk_swapchain = wine_vk_swapchain_from_handle(swapchain);

    TRACE("%p, 0x%s %p\n", device, wine_dbgstr_longlong(swapchain), allocator);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    if (wine_vk_swapchain)
    {
        pvkDestroySwapchainKHR(device, wine_vk_swapchain->native_vk_swapchain,
                               NULL /* allocator */);
        wine_vk_swapchain_destroy(wine_vk_swapchain);
    }
}

static void wayland_vkDestroyDevice(VkDevice device,
                                    const VkAllocationCallbacks *allocator)
{
    struct wine_vk_device *wine_vk_device = wine_vk_device_from_handle(device);

    TRACE("%p %p\n", device, allocator);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    if (wine_vk_device)
    {
        pvkDestroyDevice(device, NULL /* allocator */);
        wine_vk_device_destroy(wine_vk_device);
    }
}

static VkResult wayland_vkEnumerateInstanceExtensionProperties(const char *layer_name,
                                                               uint32_t *count,
                                                               VkExtensionProperties* properties)
{
    unsigned int i;
    VkResult res;

    TRACE("layer_name %s, count %p, properties %p\n", debugstr_a(layer_name), count, properties);

    /* This shouldn't get called with layer_name set, the ICD loader prevents it. */
    if (layer_name)
    {
        ERR("Layer enumeration not supported from ICD.\n");
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    /* We will return the same number of instance extensions reported by the host back to
     * winevulkan. Along the way we may replace xlib extensions with their win32 equivalents.
     * Winevulkan will perform more detailed filtering as it knows whether it has thunks
     * for a particular extension.
     */
    res = pvkEnumerateInstanceExtensionProperties(layer_name, count, properties);
    if (!properties || res < 0)
        return res;

    for (i = 0; i < *count; i++)
    {
        /* For now the only wayland extension we need to fixup. Long-term we may need an array. */
        if (!strcmp(properties[i].extensionName, "VK_KHR_wayland_surface"))
        {
            TRACE("Substituting VK_KHR_wayland_surface for VK_KHR_win32_surface\n");

            snprintf(properties[i].extensionName, sizeof(properties[i].extensionName),
                    VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
            properties[i].specVersion = VK_KHR_WIN32_SURFACE_SPEC_VERSION;
        }
    }

    TRACE("Returning %u extensions.\n", *count);
    return res;
}

static VkResult wayland_vkGetDeviceGroupSurfacePresentModesKHR(VkDevice device,
                                                               VkSurfaceKHR surface,
                                                               VkDeviceGroupPresentModeFlagsKHR *flags)
{
    TRACE("%p, 0x%s, %p\n", device, wine_dbgstr_longlong(surface), flags);

    if (!wine_vk_surface_handle_is_valid(surface))
        RETURN_VK_ERROR_SURFACE_LOST_KHR;

    return pvkGetDeviceGroupSurfacePresentModesKHR(device, surface, flags);
}

static void *wayland_vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    void *proc_addr;

    TRACE("%p, %s\n", device, debugstr_a(name));

    if ((proc_addr = get_vulkan_driver_device_proc_addr(&vulkan_funcs, name)))
        return proc_addr;

    return pvkGetDeviceProcAddr(device, name);
}

static void *wayland_vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    void *proc_addr;

    TRACE("%p, %s\n", instance, debugstr_a(name));

    if ((proc_addr = get_vulkan_driver_instance_proc_addr(&vulkan_funcs, instance, name)))
        return proc_addr;

    return pvkGetInstanceProcAddr(instance, name);
}

static VkResult wayland_vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice phys_dev,
                                                                VkSurfaceKHR surface,
                                                                uint32_t *count, VkRect2D *rects)
{
    TRACE("%p, 0x%s, %p, %p\n", phys_dev, wine_dbgstr_longlong(surface), count, rects);

    return pvkGetPhysicalDevicePresentRectanglesKHR(phys_dev, surface, count, rects);
}

/* Set the image extent in the capabilities to match what Windows expects. */
static void set_image_extent(VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps)
{
    struct wine_vk_surface *wine_vk_surface = wine_vk_surface_from_handle(surface);
    BOOL zero_extents = FALSE;

    if (!wine_vk_surface || !wine_vk_surface->wayland_surface)
        return;

    wayland_mutex_lock(&wine_vk_surface->wayland_surface->mutex);
    if (!wine_vk_surface->wayland_surface->drawing_allowed)
        zero_extents = TRUE;
    wayland_mutex_unlock(&wine_vk_surface->wayland_surface->mutex);

    if (NtUserGetWindowLongW(wine_vk_surface->hwnd, GWL_STYLE) & WS_MINIMIZE)
        zero_extents = TRUE;

    if (zero_extents)
    {
        caps->minImageExtent.width = 0;
        caps->minImageExtent.height = 0;
        caps->maxImageExtent.width = 0;
        caps->maxImageExtent.height = 0;
        caps->currentExtent.width = 0;
        caps->currentExtent.height = 0;
    }
    else
    {
        RECT client;
        NtUserGetClientRect(wine_vk_surface->hwnd, &client);

        caps->minImageExtent.width = client.right;
        caps->minImageExtent.height = client.bottom;
        caps->maxImageExtent.width = client.right;
        caps->maxImageExtent.height = client.bottom;
        caps->currentExtent.width = client.right;
        caps->currentExtent.height = client.bottom;
    }

    TRACE("vk_surface=%s hwnd=%p wayland_surface=%p extent=%dx%d\n",
          wine_dbgstr_longlong(surface), wine_vk_surface->hwnd,
          wine_vk_surface->wayland_surface, caps->currentExtent.width,
          caps->currentExtent.height);
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice phys_dev,
                                                                   const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                                   VkSurfaceCapabilities2KHR *capabilities)
{
    VkResult res;

    TRACE("%p, %p, %p\n", phys_dev, surface_info, capabilities);

    if (!wine_vk_surface_handle_is_valid(surface_info->surface))
        RETURN_VK_ERROR_SURFACE_LOST_KHR;

    if (pvkGetPhysicalDeviceSurfaceCapabilities2KHR)
    {
        res = pvkGetPhysicalDeviceSurfaceCapabilities2KHR(phys_dev, surface_info,
                                                          capabilities);
        goto out;
    }

    /* Until the loader version exporting this function is common, emulate it
     * using the older non-2 version. */
    if (surface_info->pNext || capabilities->pNext)
    {
        FIXME("Emulating vkGetPhysicalDeviceSurfaceCapabilities2KHR with "
              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR, pNext is ignored.\n");
    }

    res = pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_dev, surface_info->surface,
                                                     &capabilities->surfaceCapabilities);

out:
    if (res == VK_SUCCESS)
        set_image_extent(surface_info->surface, &capabilities->surfaceCapabilities);

    return res;
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice phys_dev,
                                                                  VkSurfaceKHR surface,
                                                                  VkSurfaceCapabilitiesKHR *capabilities)
{
    VkResult res;

    TRACE("%p, 0x%s, %p\n", phys_dev, wine_dbgstr_longlong(surface), capabilities);

    if (!wine_vk_surface_handle_is_valid(surface))
        RETURN_VK_ERROR_SURFACE_LOST_KHR;

    res = pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_dev, surface, capabilities);

    if (res == VK_SUCCESS)
        set_image_extent(surface, capabilities);

    return res;
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice phys_dev,
                                                              const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                              uint32_t *count,
                                                              VkSurfaceFormat2KHR *formats)
{
    VkSurfaceFormatKHR *formats_host;
    uint32_t i;
    VkResult result;
    TRACE("%p, %p, %p, %p\n", phys_dev, surface_info, count, formats);

    if (!wine_vk_surface_handle_is_valid(surface_info->surface))
        RETURN_VK_ERROR_SURFACE_LOST_KHR;

    if (pvkGetPhysicalDeviceSurfaceFormats2KHR)
    {
        return pvkGetPhysicalDeviceSurfaceFormats2KHR(phys_dev, surface_info,
                                                      count, formats);
    }

    /* Until the loader version exporting this function is common, emulate it
     * using the older non-2 version. */
    if (surface_info->pNext)
    {
        FIXME("Emulating vkGetPhysicalDeviceSurfaceFormats2KHR with "
              "vkGetPhysicalDeviceSurfaceFormatsKHR, pNext is ignored.\n");
    }

    if (!formats)
    {
        return pvkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface_info->surface,
                                                     count, NULL);
    }

    formats_host = calloc(*count, sizeof(*formats_host));
    if (!formats_host) return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = pvkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface_info->surface,
                                                   count, formats_host);
    if (result == VK_SUCCESS || result == VK_INCOMPLETE)
    {
        for (i = 0; i < *count; i++)
            formats[i].surfaceFormat = formats_host[i];
    }

    free(formats_host);
    return result;
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice phys_dev,
                                                             VkSurfaceKHR surface,
                                                             uint32_t *count,
                                                             VkSurfaceFormatKHR *formats)
{
    TRACE("%p, 0x%s, %p, %p\n", phys_dev, wine_dbgstr_longlong(surface), count, formats);

    if (!wine_vk_surface_handle_is_valid(surface))
        RETURN_VK_ERROR_SURFACE_LOST_KHR;

    return pvkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface, count, formats);
}

static VkResult wayland_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice phys_dev,
                                                                  VkSurfaceKHR surface,
                                                                  uint32_t *count,
                                                                  VkPresentModeKHR *modes)
{
    TRACE("%p, 0x%s, %p, %p\n", phys_dev, wine_dbgstr_longlong(surface), count, modes);

    if (!wine_vk_surface_handle_is_valid(surface))
        RETURN_VK_ERROR_SURFACE_LOST_KHR;

    return pvkGetPhysicalDeviceSurfacePresentModesKHR(phys_dev, surface, count, modes);
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice phys_dev,
                                                             uint32_t index,
                                                             VkSurfaceKHR surface,
                                                             VkBool32 *supported)
{
    TRACE("%p, %u, 0x%s, %p\n", phys_dev, index, wine_dbgstr_longlong(surface), supported);

    if (!wine_vk_surface_handle_is_valid(surface))
        RETURN_VK_ERROR_SURFACE_LOST_KHR;

    return pvkGetPhysicalDeviceSurfaceSupportKHR(phys_dev, index, surface, supported);
}

static VkBool32 wayland_vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice phys_dev,
                                                                       uint32_t index)
{
    TRACE("%p %u\n", phys_dev, index);

    return pvkGetPhysicalDeviceWaylandPresentationSupportKHR(phys_dev, index,
                                                             process_wl_display);
}

static VkResult wayland_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                uint32_t *count, VkImage *images)
{
    struct wine_vk_swapchain *wine_vk_swapchain = wine_vk_swapchain_from_handle(swapchain);

    TRACE("%p, 0x%s %p %p\n", device, wine_dbgstr_longlong(swapchain), count, images);

    if (wine_vk_swapchain_is_remote(wine_vk_swapchain))
        return wayland_remote_vk_swapchain_get_images(wine_vk_swapchain->remote_vk_swapchain,
                                                      count, images);

    return pvkGetSwapchainImagesKHR(device, swapchain, count, images);
}

static VkResult validate_present_info(const VkPresentInfoKHR *present_info)
{
    uint32_t i;
    VkResult res = VK_SUCCESS;

    for (i = 0; i < present_info->swapchainCount; ++i)
    {
        const VkSwapchainKHR vk_swapchain = present_info->pSwapchains[i];
        struct wine_vk_swapchain *wine_vk_swapchain =
            wine_vk_swapchain_from_handle(vk_swapchain);
        BOOL drawing_allowed =
            (wine_vk_swapchain && wine_vk_swapchain->wayland_surface) ?
            wine_vk_swapchain->wayland_surface->drawing_allowed : TRUE;
        RECT client;

        TRACE("swapchain[%d] vk=0x%s wine=%p extent=%dx%d wayland_surface=%p "
               "drawing_allowed=%d\n",
               i, wine_dbgstr_longlong(vk_swapchain), wine_vk_swapchain,
               wine_vk_swapchain ? wine_vk_swapchain->extent.width : 0,
               wine_vk_swapchain ? wine_vk_swapchain->extent.height : 0,
               wine_vk_swapchain ? wine_vk_swapchain->wayland_surface : NULL,
               drawing_allowed);

        if (!wine_vk_swapchain ||
            !__atomic_load_n(&wine_vk_swapchain->valid, __ATOMIC_SEQ_CST) ||
            !NtUserGetClientRect(wine_vk_swapchain->hwnd, &client))
        {
            res = VK_ERROR_SURFACE_LOST_KHR;
        }
        else if (client.right != wine_vk_swapchain->extent.width ||
                 client.bottom != wine_vk_swapchain->extent.height ||
                 !drawing_allowed)
        {
            if (res == VK_SUCCESS) res = VK_ERROR_OUT_OF_DATE_KHR;
        }

        /* Since Vulkan content is presented on a Wayland subsurface, we need
         * to ensure the parent Wayland surface is mapped for the Vulkan
         * content to be visible. */
        if (wine_vk_swapchain->wayland_surface && drawing_allowed)
            wayland_surface_ensure_mapped(wine_vk_swapchain->wayland_surface);
    }

    /* In case of error in any swapchain, we are not going to present at all,
     * so mark all swapchains as failures. */
    if (res != VK_SUCCESS && present_info->pResults)
    {
        for (i = 0; i < present_info->swapchainCount; ++i)
            present_info->pResults[i] = res;
    }

    return res;
}

static void lock_swapchain_wayland_surfaces(const VkPresentInfoKHR *present_info,
                                            BOOL lock)
{
    uint32_t i;

    for (i = 0; i < present_info->swapchainCount; ++i)
    {
        const VkSwapchainKHR vk_swapchain = present_info->pSwapchains[i];
        struct wine_vk_swapchain *wine_vk_swapchain =
            wine_vk_swapchain_from_handle(vk_swapchain);

        if (wine_vk_swapchain && wine_vk_swapchain->wayland_surface)
        {
            if (lock)
                wayland_mutex_lock(&wine_vk_swapchain->wayland_surface->mutex);
            else
                wayland_mutex_unlock(&wine_vk_swapchain->wayland_surface->mutex);
        }
    }
}

static VkResult wayland_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *present_info)
{
    VkResult res;

    TRACE("%p, %p\n", queue, present_info);

    /* Lock the surfaces to ensure we don't present while reconfiguration is
     * taking place, so we don't inadvertently commit an in-progress,
     * incomplete configuration state. */
    lock_swapchain_wayland_surfaces(present_info, TRUE);

    if ((res = validate_present_info(present_info)) == VK_SUCCESS)
        res = pvkQueuePresentKHR(queue, present_info);

    lock_swapchain_wayland_surfaces(present_info, FALSE);

    return res;
}

/* The VkSurfaceKHR we return in wayland_vkCreateWin32SurfaceKHR *is* the
 * native surface. */
static VkSurfaceKHR wayland_wine_get_native_surface(VkSurfaceKHR surface)
{
    TRACE("0x%s\n", wine_dbgstr_longlong(surface));
    return surface;
}

static void wine_vk_init(void)
{
    if (!(vulkan_handle = dlopen(SONAME_LIBVULKAN, RTLD_NOW)))
    {
        ERR("Failed to load %s.\n", SONAME_LIBVULKAN);
        return;
    }

#define LOAD_FUNCPTR(f) if (!(p##f = dlsym(vulkan_handle, #f))) goto fail
#define LOAD_OPTIONAL_FUNCPTR(f) p##f = dlsym(vulkan_handle, #f)
    LOAD_FUNCPTR(vkCreateDevice);
    LOAD_FUNCPTR(vkCreateInstance);
    LOAD_FUNCPTR(vkCreateSwapchainKHR);
    LOAD_FUNCPTR(vkCreateWaylandSurfaceKHR);
    LOAD_FUNCPTR(vkDestroyDevice);
    LOAD_FUNCPTR(vkDestroyInstance);
    LOAD_FUNCPTR(vkDestroySurfaceKHR);
    LOAD_FUNCPTR(vkDestroySwapchainKHR);
    LOAD_FUNCPTR(vkEnumerateDeviceExtensionProperties);
    LOAD_FUNCPTR(vkEnumerateInstanceExtensionProperties);
    LOAD_OPTIONAL_FUNCPTR(vkGetDeviceGroupSurfacePresentModesKHR);
    LOAD_FUNCPTR(vkGetDeviceProcAddr);
    LOAD_FUNCPTR(vkGetInstanceProcAddr);
    LOAD_OPTIONAL_FUNCPTR(vkGetPhysicalDevicePresentRectanglesKHR);
    LOAD_OPTIONAL_FUNCPTR(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    LOAD_FUNCPTR(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_OPTIONAL_FUNCPTR(vkGetPhysicalDeviceSurfaceFormats2KHR);
    LOAD_FUNCPTR(vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_FUNCPTR(vkGetPhysicalDeviceSurfacePresentModesKHR);
    LOAD_FUNCPTR(vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD_FUNCPTR(vkGetPhysicalDeviceWaylandPresentationSupportKHR);
    LOAD_FUNCPTR(vkGetSwapchainImagesKHR);
    LOAD_FUNCPTR(vkQueuePresentKHR);
#undef LOAD_FUNCPTR
#undef LOAD_OPTIONAL_FUNCPTR

    return;

fail:
    dlclose(vulkan_handle);
    vulkan_handle = NULL;
}

static const struct vulkan_funcs vulkan_funcs =
{
    .p_vkCreateDevice = wayland_vkCreateDevice,
    .p_vkCreateInstance = wayland_vkCreateInstance,
    .p_vkCreateSwapchainKHR = wayland_vkCreateSwapchainKHR,
    .p_vkCreateWin32SurfaceKHR = wayland_vkCreateWin32SurfaceKHR,
    .p_vkDestroyDevice = wayland_vkDestroyDevice,
    .p_vkDestroyInstance = wayland_vkDestroyInstance,
    .p_vkDestroySurfaceKHR = wayland_vkDestroySurfaceKHR,
    .p_vkDestroySwapchainKHR = wayland_vkDestroySwapchainKHR,
    .p_vkEnumerateInstanceExtensionProperties = wayland_vkEnumerateInstanceExtensionProperties,
    .p_vkGetDeviceGroupSurfacePresentModesKHR = wayland_vkGetDeviceGroupSurfacePresentModesKHR,
    .p_vkGetDeviceProcAddr = wayland_vkGetDeviceProcAddr,
    .p_vkGetInstanceProcAddr = wayland_vkGetInstanceProcAddr,
    .p_vkGetPhysicalDevicePresentRectanglesKHR = wayland_vkGetPhysicalDevicePresentRectanglesKHR,
    .p_vkGetPhysicalDeviceSurfaceCapabilities2KHR = wayland_vkGetPhysicalDeviceSurfaceCapabilities2KHR,
    .p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = wayland_vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
    .p_vkGetPhysicalDeviceSurfaceFormats2KHR = wayland_vkGetPhysicalDeviceSurfaceFormats2KHR,
    .p_vkGetPhysicalDeviceSurfaceFormatsKHR = wayland_vkGetPhysicalDeviceSurfaceFormatsKHR,
    .p_vkGetPhysicalDeviceSurfacePresentModesKHR = wayland_vkGetPhysicalDeviceSurfacePresentModesKHR,
    .p_vkGetPhysicalDeviceSurfaceSupportKHR = wayland_vkGetPhysicalDeviceSurfaceSupportKHR,
    .p_vkGetPhysicalDeviceWin32PresentationSupportKHR = wayland_vkGetPhysicalDeviceWin32PresentationSupportKHR,
    .p_vkGetSwapchainImagesKHR = wayland_vkGetSwapchainImagesKHR,
    .p_vkQueuePresentKHR = wayland_vkQueuePresentKHR,
    .p_wine_get_native_surface = wayland_wine_get_native_surface,
};

/**********************************************************************
 *           WAYLAND_wine_get_vulkan_driver
 */
const struct vulkan_funcs *WAYLAND_wine_get_vulkan_driver(UINT version)
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;

    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR("version mismatch, vulkan wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION);
        return NULL;
    }

    pthread_once(&init_once, wine_vk_init);
    if (vulkan_handle)
        return &vulkan_funcs;

    return NULL;
}

void wayland_invalidate_vulkan_objects(HWND hwnd)
{
    struct wine_vk_swapchain *swap;
    struct wine_vk_surface *surf;

    TRACE("hwnd=%p\n", hwnd);

    wayland_mutex_lock(&wine_vk_object_mutex);

    wl_list_for_each(swap, &wine_vk_swapchain_list, link)
    {
        if (swap->hwnd == hwnd)
            __atomic_store_n(&swap->valid, FALSE, __ATOMIC_SEQ_CST);
    }

    wl_list_for_each(surf, &wine_vk_surface_list, link)
    {
        if (surf->hwnd == hwnd)
            __atomic_store_n(&surf->valid, FALSE, __ATOMIC_SEQ_CST);
    }

    wayland_mutex_unlock(&wine_vk_object_mutex);
}

#else /* No vulkan */

const struct vulkan_funcs *WAYLAND_wine_get_vulkan_driver(UINT version)
{
    ERR("Wine was built without Vulkan support.\n");
    return NULL;
}

void wayland_invalidate_vulkan_objects(HWND hwnd)
{
}

#endif /* SONAME_LIBVULKAN */
