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

/* NOTE: If making changes here, consider whether they should be reflected in
 * the other drivers. */

#include "config.h"
#include "wine/port.h"

#include <stdarg.h>
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"

#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/list.h"
#include "waylanddrv.h"

#define VK_NO_PROTOTYPES
#define WINE_VK_HOST

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

#ifdef SONAME_LIBVULKAN
WINE_DECLARE_DEBUG_CHANNEL(fps);

#define VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR 1000006000

typedef struct VkWaylandSurfaceCreateInfoKHR
{
    VkStructureType sType;
    const void *pNext;
    VkWaylandSurfaceCreateFlagsKHR flags;
    struct wl_display *display;
    struct wl_surface *surface;
} VkWaylandSurfaceCreateInfoKHR;

static VkResult (*pvkCreateInstance)(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
static VkResult (*pvkCreateSwapchainKHR)(VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *, VkSwapchainKHR *);
static VkResult (*pvkCreateWaylandSurfaceKHR)(VkInstance, const VkWaylandSurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *);
static void (*pvkDestroyInstance)(VkInstance, const VkAllocationCallbacks *);
static void (*pvkDestroySurfaceKHR)(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *);
static void (*pvkDestroySwapchainKHR)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *);
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

static void *wayland_get_vk_device_proc_addr(const char *name);
static void *wayland_get_vk_instance_proc_addr(VkInstance instance, const char *name);

static CRITICAL_SECTION wine_vk_object_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &wine_vk_object_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": wine_vk_object_section") }
};
static CRITICAL_SECTION wine_vk_object_section = { &critsect_debug, -1, 0, 0, 0, 0 };

static struct list wine_vk_surface_list = LIST_INIT(wine_vk_surface_list);
static struct list wine_vk_swapchain_list = LIST_INIT(wine_vk_swapchain_list);

struct wine_vk_surface
{
    struct list entry;
    HWND hwnd;
    struct wayland_surface *wayland_surface;
    VkSurfaceKHR vk_surface; /* native surface */
    BOOL valid;
};

struct wine_vk_swapchain
{
    struct list entry;
    HWND hwnd;
    struct wayland_surface *wayland_surface;
    VkSwapchainKHR vk_swapchain; /* native swapchain */
    BOOL valid;
    VkExtent2D extent;
};

static inline void wine_vk_list_add(struct list *list, struct list *entry)
{
    EnterCriticalSection(&wine_vk_object_section);
    list_add_tail(list, entry);
    LeaveCriticalSection(&wine_vk_object_section);
}

static inline void wine_vk_list_remove(struct list *entry)
{
    EnterCriticalSection(&wine_vk_object_section);
    list_remove(entry);
    LeaveCriticalSection(&wine_vk_object_section);
}

static struct wine_vk_surface *wine_vk_surface_from_handle(VkSurfaceKHR handle)
{
    struct wine_vk_surface *surf;

    EnterCriticalSection(&wine_vk_object_section);

    LIST_FOR_EACH_ENTRY(surf, &wine_vk_surface_list, struct wine_vk_surface, entry)
    {
        if (surf->vk_surface == handle) goto out;
    }

    surf = NULL;

out:
    LeaveCriticalSection(&wine_vk_object_section);
    return surf;
}

static BOOL wine_vk_surface_handle_is_valid(VkSurfaceKHR handle)
{
    struct wine_vk_surface *wine_vk_surface = wine_vk_surface_from_handle(handle);
    return wine_vk_surface && __atomic_load_n(&wine_vk_surface->valid, __ATOMIC_SEQ_CST);
}

static void wine_vk_surface_destroy(struct wine_vk_surface *wine_vk_surface)
{
    wine_vk_list_remove(&wine_vk_surface->entry);

    if (wine_vk_surface->wayland_surface)
        wayland_surface_unref_glvk(wine_vk_surface->wayland_surface);

    heap_free(wine_vk_surface);
}

static struct wine_vk_swapchain *wine_vk_swapchain_from_handle(VkSurfaceKHR handle)
{
    struct wine_vk_swapchain *swap;

    EnterCriticalSection(&wine_vk_object_section);

    LIST_FOR_EACH_ENTRY(swap, &wine_vk_swapchain_list, struct wine_vk_swapchain, entry)
    {
        if (swap->vk_swapchain == handle) goto out;
    }

    swap = NULL;

out:
    LeaveCriticalSection(&wine_vk_object_section);
    return swap;
}

static void wine_vk_swapchain_destroy(struct wine_vk_swapchain *wine_vk_swapchain)
{
    wine_vk_list_remove(&wine_vk_swapchain->entry);

    if (wine_vk_swapchain->wayland_surface)
        wayland_surface_unref_glvk(wine_vk_swapchain->wayland_surface);

    heap_free(wine_vk_swapchain);
}

static inline VkResult vk_error_surface_lost(void)
{
    TRACE("VK_ERROR_SURFACE_LOST_KHR\n");
    return VK_ERROR_SURFACE_LOST_KHR;
}

static void *vulkan_handle;

static BOOL WINAPI wine_vk_init(INIT_ONCE *once, void *param, void **context)
{
    if (!(vulkan_handle = dlopen(SONAME_LIBVULKAN, RTLD_NOW)))
    {
        ERR("Failed to load %s.\n", SONAME_LIBVULKAN);
        return TRUE;
    }

#define LOAD_FUNCPTR(f) if (!(p##f = dlsym(vulkan_handle, #f))) goto fail
#define LOAD_OPTIONAL_FUNCPTR(f) p##f = dlsym(vulkan_handle, #f)
    LOAD_FUNCPTR(vkCreateInstance);
    LOAD_FUNCPTR(vkCreateSwapchainKHR);
    LOAD_FUNCPTR(vkCreateWaylandSurfaceKHR);
    LOAD_FUNCPTR(vkDestroyInstance);
    LOAD_FUNCPTR(vkDestroySurfaceKHR);
    LOAD_FUNCPTR(vkDestroySwapchainKHR);
    LOAD_FUNCPTR(vkEnumerateInstanceExtensionProperties);
    LOAD_FUNCPTR(vkGetDeviceProcAddr);
    LOAD_FUNCPTR(vkGetInstanceProcAddr);
    LOAD_OPTIONAL_FUNCPTR(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    LOAD_FUNCPTR(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_OPTIONAL_FUNCPTR(vkGetPhysicalDeviceSurfaceFormats2KHR);
    LOAD_FUNCPTR(vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_FUNCPTR(vkGetPhysicalDeviceSurfacePresentModesKHR);
    LOAD_FUNCPTR(vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD_FUNCPTR(vkGetPhysicalDeviceWaylandPresentationSupportKHR);
    LOAD_FUNCPTR(vkGetSwapchainImagesKHR);
    LOAD_FUNCPTR(vkQueuePresentKHR);
    LOAD_OPTIONAL_FUNCPTR(vkGetDeviceGroupSurfacePresentModesKHR);
    LOAD_OPTIONAL_FUNCPTR(vkGetPhysicalDevicePresentRectanglesKHR);
#undef LOAD_FUNCPTR
#undef LOAD_OPTIONAL_FUNCPTR

    return TRUE;

fail:
    dlclose(vulkan_handle);
    vulkan_handle = NULL;
    return TRUE;
}

/* Helper function for converting between win32 and X11 compatible VkInstanceCreateInfo.
 * Caller is responsible for allocation and cleanup of 'dst'.
 */
static VkResult wine_vk_instance_convert_create_info(const VkInstanceCreateInfo *src,
        VkInstanceCreateInfo *dst)
{
    unsigned int i;
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
        enabled_extensions = heap_calloc(src->enabledExtensionCount, sizeof(*src->ppEnabledExtensionNames));
        if (!enabled_extensions)
        {
            ERR("Failed to allocate memory for enabled extensions\n");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        for (i = 0; i < src->enabledExtensionCount; i++)
        {
            /* Substitute extension with X11 ones else copy. Long-term, when we
             * support more extensions, we should store these in a list.
             */
            if (!strcmp(src->ppEnabledExtensionNames[i], "VK_KHR_win32_surface"))
            {
                enabled_extensions[i] = "VK_KHR_wayland_surface";
            }
            else
            {
                enabled_extensions[i] = src->ppEnabledExtensionNames[i];
            }
        }
        dst->ppEnabledExtensionNames = enabled_extensions;
        dst->enabledExtensionCount = src->enabledExtensionCount;
    }

    return VK_SUCCESS;
}

static VkResult wayland_vkCreateInstance(const VkInstanceCreateInfo *create_info,
        const VkAllocationCallbacks *allocator, VkInstance *instance)
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

    heap_free((void *)create_info_host.ppEnabledExtensionNames);
    return res;
}

static VkResult wayland_vkCreateSwapchainKHR(VkDevice device,
        const VkSwapchainCreateInfoKHR *create_info,
        const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain)
{
    VkResult res;
    struct wine_vk_surface *wine_vk_surface;
    struct wine_vk_swapchain *wine_vk_swapchain;

    TRACE("%p %p %p %p\n", device, create_info, allocator, swapchain);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    wine_vk_surface = wine_vk_surface_from_handle(create_info->surface);
    if (!wine_vk_surface || !__atomic_load_n(&wine_vk_surface->valid, __ATOMIC_SEQ_CST))
        return vk_error_surface_lost();

    wine_vk_swapchain = heap_alloc_zero(sizeof(*wine_vk_swapchain));
    if (!wine_vk_swapchain)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    list_init(&wine_vk_swapchain->entry);

    res = pvkCreateSwapchainKHR(device, create_info, NULL /* allocator */, swapchain);
    if (res != VK_SUCCESS)
        goto err;

    wine_vk_swapchain->hwnd = wine_vk_surface->hwnd;
    if (wine_vk_surface->wayland_surface)
    {
        wayland_surface_create_or_ref_vk(wine_vk_surface->wayland_surface);
        wine_vk_swapchain->wayland_surface = wine_vk_surface->wayland_surface;
    }
    wine_vk_swapchain->vk_swapchain = *swapchain;
    wine_vk_swapchain->valid = TRUE;
    wine_vk_swapchain->extent = create_info->imageExtent;

    wine_vk_list_add(&wine_vk_swapchain_list, &wine_vk_swapchain->entry);

    return res;

err:
    wine_vk_swapchain_destroy(wine_vk_swapchain);
    return res;
}

static VkResult wayland_vkCreateWin32SurfaceKHR(VkInstance instance,
        const VkWin32SurfaceCreateInfoKHR *create_info,
        const VkAllocationCallbacks *allocator, VkSurfaceKHR *vk_surface)
{
    VkResult res;
    VkWaylandSurfaceCreateInfoKHR create_info_host;
    struct wine_vk_surface *wine_vk_surface; //, *prev;
    struct wayland_surface *wayland_surface;

    TRACE("%p %p %p %p\n", instance, create_info, allocator, vk_surface);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    /* TODO: support child window rendering. */
    if (GetAncestor(create_info->hwnd, GA_PARENT) != GetDesktopWindow())
    {
        FIXME("Application requires child window rendering, which is not implemented yet!\n");
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }

    wine_vk_surface = heap_alloc_zero(sizeof(*wine_vk_surface));
    if (!wine_vk_surface)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    list_init(&wine_vk_surface->entry);

    wayland_surface = wayland_surface_for_hwnd(create_info->hwnd);
    if (wayland_surface && !wayland_surface_create_or_ref_vk(wayland_surface))
    {
        ERR("Failed to allocate vulkan surface for hwnd=%p\n", create_info->hwnd);

        /* VK_KHR_win32_surface only allows out of host and device memory as errors. */
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto err;
    }
    wine_vk_surface->wayland_surface = wayland_surface;

    create_info_host.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    create_info_host.pNext = NULL;
    create_info_host.flags = 0; /* reserved */
    create_info_host.display = process_wl_display;
    create_info_host.surface = wayland_surface->glvk->wl_surface;

    res = pvkCreateWaylandSurfaceKHR(instance, &create_info_host, NULL /* allocator */, vk_surface);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to create vulkan wayland surface, res=%d\n", res);
        goto err;
    }

    wine_vk_surface->hwnd = create_info->hwnd;
    wine_vk_surface->vk_surface = *vk_surface;
    wine_vk_surface->valid = TRUE;

    wine_vk_list_add(&wine_vk_surface_list, &wine_vk_surface->entry);

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

    pvkDestroySurfaceKHR(instance, surface, NULL /* allocator */);

    if (wine_vk_surface)
        wine_vk_surface_destroy(wine_vk_surface);
}

static void wayland_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
         const VkAllocationCallbacks *allocator)
{
    struct wine_vk_swapchain *wine_vk_swapchain = wine_vk_swapchain_from_handle(swapchain);

    TRACE("%p, 0x%s %p\n", device, wine_dbgstr_longlong(swapchain), allocator);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    pvkDestroySwapchainKHR(device, swapchain, NULL /* allocator */);

    if (wine_vk_swapchain)
        wine_vk_swapchain_destroy(wine_vk_swapchain);
}

static VkResult wayland_vkEnumerateInstanceExtensionProperties(const char *layer_name,
        uint32_t *count, VkExtensionProperties* properties)
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
        VkSurfaceKHR surface, VkDeviceGroupPresentModeFlagsKHR *flags)
{
    TRACE("%p, 0x%s, %p\n", device, wine_dbgstr_longlong(surface), flags);

    if (!wine_vk_surface_handle_is_valid(surface))
        return vk_error_surface_lost();

    return pvkGetDeviceGroupSurfacePresentModesKHR(device, surface, flags);
}

static void *wayland_vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    void *proc_addr;

    TRACE("%p, %s\n", device, debugstr_a(name));

    if ((proc_addr = wayland_get_vk_device_proc_addr(name)))
        return proc_addr;

    return pvkGetDeviceProcAddr(device, name);
}

static void *wayland_vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    void *proc_addr;

    TRACE("%p, %s\n", instance, debugstr_a(name));

    if ((proc_addr = wayland_get_vk_instance_proc_addr(instance, name)))
        return proc_addr;

    return pvkGetInstanceProcAddr(instance, name);
}

static VkResult wayland_vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice phys_dev,
        VkSurfaceKHR surface, uint32_t *count, VkRect2D *rects)
{
    TRACE("%p, 0x%s, %p, %p\n", phys_dev, wine_dbgstr_longlong(surface), count, rects);

    return pvkGetPhysicalDevicePresentRectanglesKHR(phys_dev, surface, count, rects);
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice phys_dev,
        const VkPhysicalDeviceSurfaceInfo2KHR *surface_info, VkSurfaceCapabilities2KHR *capabilities)
{
    TRACE("%p, %p, %p\n", phys_dev, surface_info, capabilities);

    if (!wine_vk_surface_handle_is_valid(surface_info->surface))
        return vk_error_surface_lost();

    if (pvkGetPhysicalDeviceSurfaceCapabilities2KHR)
        return pvkGetPhysicalDeviceSurfaceCapabilities2KHR(phys_dev, surface_info, capabilities);

    /* Until the loader version exporting this function is common, emulate it using the older non-2 version. */
    if (surface_info->pNext || capabilities->pNext)
        FIXME("Emulating vkGetPhysicalDeviceSurfaceCapabilities2KHR with vkGetPhysicalDeviceSurfaceCapabilitiesKHR, pNext is ignored.\n");

    return pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_dev, surface_info->surface, &capabilities->surfaceCapabilities);
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice phys_dev,
        VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *capabilities)
{
    TRACE("%p, 0x%s, %p\n", phys_dev, wine_dbgstr_longlong(surface), capabilities);

    return pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_dev, surface, capabilities);
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice phys_dev,
        const VkPhysicalDeviceSurfaceInfo2KHR *surface_info, uint32_t *count, VkSurfaceFormat2KHR *formats)
{
    VkSurfaceFormatKHR *formats_host;
    uint32_t i;
    VkResult result;
    TRACE("%p, %p, %p, %p\n", phys_dev, surface_info, count, formats);

    if (!wine_vk_surface_handle_is_valid(surface_info->surface))
        return vk_error_surface_lost();

    if (pvkGetPhysicalDeviceSurfaceFormats2KHR)
        return pvkGetPhysicalDeviceSurfaceFormats2KHR(phys_dev, surface_info, count, formats);

    /* Until the loader version exporting this function is common, emulate it using the older non-2 version. */
    if (surface_info->pNext)
        FIXME("Emulating vkGetPhysicalDeviceSurfaceFormats2KHR with vkGetPhysicalDeviceSurfaceFormatsKHR, pNext is ignored.\n");

    if (!formats)
        return pvkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface_info->surface, count, NULL);

    formats_host = heap_calloc(*count, sizeof(*formats_host));
    if (!formats_host) return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = pvkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface_info->surface, count, formats_host);
    if (result == VK_SUCCESS || result == VK_INCOMPLETE)
    {
        for (i = 0; i < *count; i++)
            formats[i].surfaceFormat = formats_host[i];
    }

    heap_free(formats_host);
    return result;
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice phys_dev,
        VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *formats)
{
    TRACE("%p, 0x%s, %p, %p\n", phys_dev, wine_dbgstr_longlong(surface), count, formats);

    if (!wine_vk_surface_handle_is_valid(surface))
        return vk_error_surface_lost();

    return pvkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface, count, formats);
}

static VkResult wayland_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice phys_dev,
        VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *modes)
{
    TRACE("%p, 0x%s, %p, %p\n", phys_dev, wine_dbgstr_longlong(surface), count, modes);

    if (!wine_vk_surface_handle_is_valid(surface))
        return vk_error_surface_lost();

    return pvkGetPhysicalDeviceSurfacePresentModesKHR(phys_dev, surface, count, modes);
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice phys_dev,
        uint32_t index, VkSurfaceKHR surface, VkBool32 *supported)
{
    TRACE("%p, %u, 0x%s, %p\n", phys_dev, index, wine_dbgstr_longlong(surface), supported);

    if (!wine_vk_surface_handle_is_valid(surface))
        return vk_error_surface_lost();

    return pvkGetPhysicalDeviceSurfaceSupportKHR(phys_dev, index, surface, supported);
}

static VkBool32 wayland_vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice phys_dev,
        uint32_t index)
{
    TRACE("%p %u\n", phys_dev, index);

    return pvkGetPhysicalDeviceWaylandPresentationSupportKHR(phys_dev, index, process_wl_display);
}

static VkResult wayland_vkGetSwapchainImagesKHR(VkDevice device,
        VkSwapchainKHR swapchain, uint32_t *count, VkImage *images)
{
    TRACE("%p, 0x%s %p %p\n", device, wine_dbgstr_longlong(swapchain), count, images);

    return pvkGetSwapchainImagesKHR(device, swapchain, count, images);
}

static VkResult validate_present_info(const VkPresentInfoKHR *present_info)
{
    uint32_t i;
    VkResult res = VK_SUCCESS;

    for (i = 0; i < present_info->swapchainCount && res == VK_SUCCESS; ++i)
    {
        const VkSwapchainKHR vk_swapchain = present_info->pSwapchains[i];
        struct wine_vk_swapchain *wine_vk_swapchain = wine_vk_swapchain_from_handle(vk_swapchain);
        RECT client;

        TRACE("swapchain[%d] vk=0x%s wine=%p extent=%ux%u wayland_surface=%p\n",
                i, wine_dbgstr_longlong(vk_swapchain), wine_vk_swapchain,
                wine_vk_swapchain->extent.width, wine_vk_swapchain->extent.height,
                wine_vk_swapchain ? wine_vk_swapchain->wayland_surface : NULL);

        if (!wine_vk_swapchain ||
            !__atomic_load_n(&wine_vk_swapchain->valid, __ATOMIC_SEQ_CST) ||
            !GetClientRect(wine_vk_swapchain->hwnd, &client))
        {
            res = VK_ERROR_SURFACE_LOST_KHR;
        }
        else if (client.right != wine_vk_swapchain->extent.width ||
                 client.bottom != wine_vk_swapchain->extent.height)
        {
            res = VK_ERROR_OUT_OF_DATE_KHR;
        }
        else if (wine_vk_swapchain->wayland_surface)
        {
            wayland_surface_ensure_mapped(wine_vk_swapchain->wayland_surface);
        }
    }

    return res;
}

static VkResult wayland_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *present_info)
{
    VkResult res;

    TRACE("%p, %p\n", queue, present_info);

    res = validate_present_info(present_info);
    if (res != VK_SUCCESS)
        return res;

    res = pvkQueuePresentKHR(queue, present_info);

    if (TRACE_ON(fps))
    {
        static unsigned long frames, frames_total;
        static long prev_time, start_time;
        DWORD time;

        time = GetTickCount();
        frames++;
        frames_total++;
        if (time - prev_time > 1500)
        {
            TRACE_(fps)("%p @ approx %.2ffps, total %.2ffps\n",
                    queue, 1000.0 * frames / (time - prev_time),
                    1000.0 * frames_total / (time - start_time));
            prev_time = time;
            frames = 0;
            if (!start_time)
                start_time = time;
        }
    }

    return res;
}

static VkSurfaceKHR wayland_wine_get_native_surface(VkSurfaceKHR surface)
{
    struct wine_vk_surface *wine_vk_surface = wine_vk_surface_from_handle(surface);

    TRACE("0x%s\n", wine_dbgstr_longlong(surface));

    return wine_vk_surface->vk_surface;
}

static const struct vulkan_funcs vulkan_funcs =
{
    wayland_vkCreateInstance,
    wayland_vkCreateSwapchainKHR,
    wayland_vkCreateWin32SurfaceKHR,
    wayland_vkDestroyInstance,
    wayland_vkDestroySurfaceKHR,
    wayland_vkDestroySwapchainKHR,
    wayland_vkEnumerateInstanceExtensionProperties,
    wayland_vkGetDeviceGroupSurfacePresentModesKHR,
    wayland_vkGetDeviceProcAddr,
    wayland_vkGetInstanceProcAddr,
    wayland_vkGetPhysicalDevicePresentRectanglesKHR,
    wayland_vkGetPhysicalDeviceSurfaceCapabilities2KHR,
    wayland_vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
    wayland_vkGetPhysicalDeviceSurfaceFormats2KHR,
    wayland_vkGetPhysicalDeviceSurfaceFormatsKHR,
    wayland_vkGetPhysicalDeviceSurfacePresentModesKHR,
    wayland_vkGetPhysicalDeviceSurfaceSupportKHR,
    wayland_vkGetPhysicalDeviceWin32PresentationSupportKHR,
    wayland_vkGetSwapchainImagesKHR,
    wayland_vkQueuePresentKHR,

    wayland_wine_get_native_surface,
};

static void *wayland_get_vk_device_proc_addr(const char *name)
{
    return get_vulkan_driver_device_proc_addr(&vulkan_funcs, name);
}

static void *wayland_get_vk_instance_proc_addr(VkInstance instance, const char *name)
{
    return get_vulkan_driver_instance_proc_addr(&vulkan_funcs, instance, name);
}

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

void wayland_invalidate_vulkan_objects(HWND hwnd)
{
    struct wine_vk_swapchain *swap;
    struct wine_vk_surface *surf;

    TRACE("hwnd=%p\n", hwnd);

    EnterCriticalSection(&wine_vk_object_section);

    LIST_FOR_EACH_ENTRY(swap, &wine_vk_swapchain_list,
                        struct wine_vk_swapchain, entry)
    {
        if (swap->hwnd == hwnd)
            __atomic_store_n(&swap->valid, FALSE, __ATOMIC_SEQ_CST);
    }

    LIST_FOR_EACH_ENTRY(surf, &wine_vk_surface_list,
                        struct wine_vk_surface, entry)
    {
        if (surf->hwnd == hwnd)
            __atomic_store_n(&surf->valid, FALSE, __ATOMIC_SEQ_CST);
    }

    LeaveCriticalSection(&wine_vk_object_section);
}

#else /* No vulkan */

const struct vulkan_funcs *wayland_get_vulkan_driver(UINT version)
{
    ERR("Wine was built without Vulkan support.\n");
    return NULL;
}

void wayland_invalidate_vulkan_objects(HWND hwnd)
{
}

#endif /* SONAME_LIBVULKAN */
