/* WAYLANDDRV Vulkan internal swapchain implementation
 *
 * Copyright 2022 Leandro Ribeiro
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

#include <stdlib.h>
#include <assert.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

struct drm_vk_format {
    VkFormat vk_format;
    VkFormat vk_format_srgb;
    uint32_t drm_format;
    uint32_t drm_format_alpha;
};
const static struct drm_vk_format format_table[] = {
    {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888},
    {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888},
};

static BOOL is_vulkan_format_supported(VkFormat format)
{
    unsigned int i;
    uint32_t format_table_size;

    format_table_size = sizeof(format_table) / sizeof(struct drm_vk_format);

    for (i = 0; i < format_table_size; i++)
        if (format_table[i].vk_format == format)
            return TRUE;

    return FALSE;
}

static uint32_t vulkan_format_to_drm_format(VkFormat format, BOOL ignore_alpha)
{
    unsigned int i;
    uint32_t format_table_size;

    format_table_size = sizeof(format_table) / sizeof(struct drm_vk_format);

    for (i = 0; i < format_table_size; i++)
        if (format_table[i].vk_format == format ||
            format_table[i].vk_format_srgb == format) {
            if (ignore_alpha) return format_table[i].drm_format;
            return format_table[i].drm_format_alpha;
        }

    WARN("Failed to get corresponding DRM format for Vulkan format\n");
    return DRM_FORMAT_INVALID;
}

static VkResult vulkan_get_supported_formats_count(VkPhysicalDevice phys_dev,
                                                   VkSurfaceKHR surface, uint32_t *count)
{
    uint32_t count_res = 0;
    unsigned int i;
    VkSurfaceFormatKHR *surface_formats = NULL;
    VkResult res;

    res = pvkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface, count, NULL);
    if (res != VK_SUCCESS)
        goto err;

    surface_formats = malloc(*count * sizeof(VkSurfaceFormatKHR));
    if (!surface_formats)
        goto err;

    res = pvkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface,
                                                count, surface_formats);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        goto err;

    for (i = 0; i < *count; i++)
        if (is_vulkan_format_supported(surface_formats[i].format))
            count_res++;

    free(surface_formats);
    *count = count_res;

    return VK_SUCCESS;

err:
    if (surface_formats)
        free(surface_formats);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult vulkan_get_supported_formats(VkPhysicalDevice phys_dev, VkSurfaceKHR surface,
                                             uint32_t supported_formats_count,
                                             VkSurfaceFormatKHR *supported_formats)
{
    uint32_t all_formats_count;
    unsigned int i, j;
    VkSurfaceFormatKHR *all_formats = NULL;
    VkResult res;

    res = pvkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface,
                                                &all_formats_count, NULL);
    if (res != VK_SUCCESS)
        goto err;

    all_formats = malloc(all_formats_count * sizeof(VkSurfaceFormatKHR));
    if (!all_formats)
        goto err;

    res = pvkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface,
                                                &all_formats_count, all_formats);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        goto err;

    for (i = 0, j = 0; i < all_formats_count; i++) {
        if (is_vulkan_format_supported(all_formats[i].format)) {
            assert(j < supported_formats_count);
            supported_formats[j++] = all_formats[i];
        }
    }

    free(all_formats);

    return VK_SUCCESS;

err:
    if (all_formats)
        free(all_formats);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult wayland_vulkan_internal_swapchain_get_supported_formats(VkPhysicalDevice phys_dev,
                                                                 VkSurfaceKHR surface,
                                                                 uint32_t *supported_formats_count,
                                                                 VkSurfaceFormatKHR *supported_formats)
{
    if (!supported_formats)
        return vulkan_get_supported_formats_count(phys_dev, surface,
                                                  supported_formats_count);

    return vulkan_get_supported_formats(phys_dev, surface, *supported_formats_count,
                                        supported_formats);
}

static VkResult vulkan_get_supported_formats2_count(VkPhysicalDevice phys_dev,
                                                    const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                    uint32_t *count)
{
    uint32_t count_res = 0;
    unsigned int i;
    VkSurfaceFormat2KHR *surface_formats = NULL;
    VkResult res;

    res = pvkGetPhysicalDeviceSurfaceFormats2KHR(phys_dev, surface_info, count, NULL);
    if (res != VK_SUCCESS)
        goto err;

    surface_formats = malloc(*count * sizeof(VkSurfaceFormat2KHR));
    if (!surface_formats)
        goto err;

    res = pvkGetPhysicalDeviceSurfaceFormats2KHR(phys_dev, surface_info,
                                                 count, surface_formats);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        goto err;

    for (i = 0; i < *count; i++)
        if (is_vulkan_format_supported(surface_formats[i].surfaceFormat.format))
            count_res++;

    free(surface_formats);
    *count = count_res;

    return VK_SUCCESS;

err:
    if (surface_formats)
        free(surface_formats);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult vulkan_get_supported_formats2(VkPhysicalDevice phys_dev,
                                              const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                              uint32_t supported_formats_count,
                                              VkSurfaceFormat2KHR *supported_formats)
{
    uint32_t all_formats_count;
    unsigned int i, j;
    VkSurfaceFormat2KHR *all_formats = NULL;
    VkResult res;

    res = pvkGetPhysicalDeviceSurfaceFormats2KHR(phys_dev, surface_info,
                                                 &all_formats_count, NULL);
    if (res != VK_SUCCESS)
        goto err;

    all_formats = malloc(all_formats_count * sizeof(VkSurfaceFormat2KHR));
    if (!all_formats)
        goto err;

    res = pvkGetPhysicalDeviceSurfaceFormats2KHR(phys_dev, surface_info,
                                                 &all_formats_count, all_formats);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        goto err;

    for (i = 0, j = 0; i < all_formats_count; i++) {
        if (is_vulkan_format_supported(all_formats[i].surfaceFormat.format)) {
            assert(j < supported_formats_count);
            supported_formats[j++] = all_formats[i];
        }
    }

    free(all_formats);

    return VK_SUCCESS;

err:
    if (all_formats)
        free(all_formats);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult wayland_vulkan_internal_swapchain_get_supported_formats2(VkPhysicalDevice phys_dev,
                                                                  const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                                  uint32_t *supported_formats_count,
                                                                  VkSurfaceFormat2KHR *supported_formats)
{
    if (!supported_formats)
        return vulkan_get_supported_formats2_count(phys_dev, surface_info,
                                                   supported_formats_count);

    return vulkan_get_supported_formats2(phys_dev, surface_info, *supported_formats_count,
                                         supported_formats);
}

static uint32_t vulkan_format_modifier_get_plane_count(VkInstance instance, VkPhysicalDevice phys_dev,
                                                       VkFormat format, uint64_t modifier)
{
    uint32_t plane_count = 0;
    unsigned int i;
    PFN_vkGetPhysicalDeviceFormatProperties2KHR pfn_vkGetPhysicalDeviceFormatProperties2KHR =
        vulkan_funcs.p_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFormatProperties2KHR");
    VkDrmFormatModifierPropertiesEXT *mod_props;
    VkDrmFormatModifierPropertiesListEXT format_mod_props_list = {0};
    VkFormatProperties2 format_props = {0};

    if (!pfn_vkGetPhysicalDeviceFormatProperties2KHR) {
        WARN("vkGetPhysicalDeviceFormatProperties2KHR not present\n");
        goto out;
    }

    format_mod_props_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;

    format_props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    format_props.pNext = &format_mod_props_list;

    pfn_vkGetPhysicalDeviceFormatProperties2KHR(phys_dev, format, &format_props);

    if (format_mod_props_list.drmFormatModifierCount == 0) {
        WARN("Invalid number of modifiers for format: 0\n");
        goto out;
    }

    format_mod_props_list.pDrmFormatModifierProperties =
        malloc(format_mod_props_list.drmFormatModifierCount *
               sizeof(VkDrmFormatModifierPropertiesEXT));
    if (!format_mod_props_list.pDrmFormatModifierProperties) {
        WARN("Failed to allocate memory\n");
        goto out;
    }

    pfn_vkGetPhysicalDeviceFormatProperties2KHR(phys_dev, format, &format_props);

    for (i = 0; i < format_mod_props_list.drmFormatModifierCount; i++) {
        mod_props = &format_mod_props_list.pDrmFormatModifierProperties[i];
        if (mod_props->drmFormatModifier != modifier)
            continue;
        plane_count = mod_props->drmFormatModifierPlaneCount;
        break;
    }

    free(format_mod_props_list.pDrmFormatModifierProperties);
    format_mod_props_list.pDrmFormatModifierProperties = NULL;

out:
    if (plane_count == 0)
        WARN("Failed to get number of planes for DRM format modifier\n");
    return plane_count;
}

static int wayland_native_buffer_init_vk(VkInstance instance, VkPhysicalDevice phys_dev,
                                         VkDevice dev, VkDeviceMemory image_mem,
                                         struct wine_vk_image *image, BOOL ignore_alpha)
{
    struct wayland_native_buffer *buffer = &image->native_buffer;
    uint64_t modifier;
    unsigned int i;
    PFN_vkGetMemoryFdKHR pfn_vkGetMemoryFdKHR =
        vulkan_funcs.p_vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    PFN_vkGetImageDrmFormatModifierPropertiesEXT pfn_vkGetImageDrmFormatModifierPropertiesEXT =
        vulkan_funcs.p_vkGetDeviceProcAddr(dev, "vkGetImageDrmFormatModifierPropertiesEXT");
    PFN_vkGetImageSubresourceLayout pfn_vkGetImageSubresourceLayout =
        vulkan_funcs.p_vkGetDeviceProcAddr(dev, "vkGetImageSubresourceLayout");
    const VkImageAspectFlagBits aspect_flag_bits[] = {
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT
    };
    VkMemoryGetFdInfoKHR memory_get_fd_info = {0};
    VkImageDrmFormatModifierPropertiesEXT image_drm_format_modifier_props;
    VkSubresourceLayout layout;
    VkImageSubresource image_subresource = {0};
    VkResult res;

    if (!pfn_vkGetMemoryFdKHR || !pfn_vkGetImageDrmFormatModifierPropertiesEXT ||
        !pfn_vkGetImageSubresourceLayout) {
        WARN("vkGetMemoryFdKHR, vkGetImageDrmFormatModifierPropertiesEXT or " \
             "vkGetImageSubresourceLayout not present\n");
        goto err;
    }

    memory_get_fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    memory_get_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    memory_get_fd_info.memory = image_mem;

    res = pfn_vkGetImageDrmFormatModifierPropertiesEXT(dev, image->native_vk_image,
                                                       &image_drm_format_modifier_props);
    if (res != VK_SUCCESS) {
        WARN("Failed to get Vulkan image's DRM format modifier properties\n");
        goto err;
    }

    modifier = image_drm_format_modifier_props.drmFormatModifier;

    /* TODO: support creating multi-plane formats with
     * VK_IMAGE_CREATE_DISJOINT_BIT. My understanding is that the driver sets
     * VK_FORMAT_FEATURE_DISJOINT_BIT when images of multi-plane formats CAN be
     * created with this flag, but we can also create them without this flag.
     * Then we'll have the same fd for each plane, but each of them with a
     * different offset etc. Using the flag we'd have different memory for each
     * plane, and so different fd's */
    buffer->plane_count =
        vulkan_format_modifier_get_plane_count(instance, phys_dev,
                                               image->format, modifier);
    if (buffer->plane_count == 0) goto err;

    for (i = 0; i < buffer->plane_count; i++) {
        res = pfn_vkGetMemoryFdKHR(dev, &memory_get_fd_info, &buffer->fds[i]);
        if (res != VK_SUCCESS) {
            WARN("Failed to query fd for buffer plane\n");
            goto err;
        }

        /* Specific flag for plane i */
        image_subresource.aspectMask = aspect_flag_bits[i];

        pfn_vkGetImageSubresourceLayout(dev, image->native_vk_image,
                                        &image_subresource, &layout);
        buffer->offsets[i] = layout.offset;
        buffer->strides[i] = layout.rowPitch;
    }

    buffer->format = vulkan_format_to_drm_format(image->format, ignore_alpha);
    if (buffer->format == DRM_FORMAT_INVALID) goto err;

    buffer->width = image->width;
    buffer->height = image->height;
    buffer->modifier = modifier;

    return 0;

err:
    return -1;
    WARN("Failed to init wayland_native_buffer for Vulkan image\n");
}

static int get_image_create_flags(VkSwapchainCreateInfoKHR chain_create_info)
{
    uint32_t flags = 0; /* TODO: what are good default flags to use here ? */

    if (chain_create_info.flags & VK_SWAPCHAIN_CREATE_FLAG_BITS_KHR_MAX_ENUM)
        flags |= VK_IMAGE_CREATE_FLAG_BITS_MAX_ENUM;

    if (chain_create_info.flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)
        flags |= VK_IMAGE_CREATE_PROTECTED_BIT;

    if (chain_create_info.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

    if (chain_create_info.flags & VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR)
        flags |= VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT;

    return flags;
}

static int create_vulkan_image(VkDevice dev, VkSwapchainCreateInfoKHR chain_create_info,
                               VkImage *image)
{
    PFN_vkCreateImage pfn_vkCreateImage =
        vulkan_funcs.p_vkGetDeviceProcAddr(dev, "vkCreateImage");
    VkExternalMemoryImageCreateInfo external_memory_create_info = {0};
    VkImageDrmFormatModifierListCreateInfoEXT drm_mode_list = {0};
    VkImageCreateInfo image_create_info = {0};
    VkResult res;

    if (!pfn_vkCreateImage) {
        WARN("vkCreateImage not present");
        return -1;
    }

    external_memory_create_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_memory_create_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    drm_mode_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    drm_mode_list.pNext = &external_memory_create_info;
    drm_mode_list.drmFormatModifierCount = 1;
    drm_mode_list.pDrmFormatModifiers = (uint64_t[]) { DRM_FORMAT_MOD_LINEAR };

    image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.pNext = &drm_mode_list;
    image_create_info.imageType = VK_IMAGE_TYPE_2D;
    image_create_info.format = chain_create_info.imageFormat;
    image_create_info.extent.width = chain_create_info.imageExtent.width;
    image_create_info.extent.height = chain_create_info.imageExtent.height;
    image_create_info.extent.depth = 1;
    image_create_info.arrayLayers = chain_create_info.imageArrayLayers;
    image_create_info.sharingMode = chain_create_info.imageSharingMode;
    image_create_info.usage = chain_create_info.imageUsage;
    image_create_info.mipLevels = 1;
    image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_create_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_create_info.flags = get_image_create_flags(chain_create_info);

    res = pfn_vkCreateImage(dev, &image_create_info, NULL, image);
    if (res != VK_SUCCESS) {
        WARN("Failed to create Vulkan image for internal swapchain");
        return -1;
    }

    return 0;
}

static int get_memory_property_flags(VkSwapchainCreateInfoKHR chain_create_info)
{
    uint32_t flags = 0; /* TODO: what are good default flags to use here ? */

    if (chain_create_info.flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)
        flags |= VK_MEMORY_PROPERTY_PROTECTED_BIT;

    if (chain_create_info.flags & VK_SWAPCHAIN_CREATE_FLAG_BITS_KHR_MAX_ENUM)
        flags |= VK_MEMORY_PROPERTY_FLAG_BITS_MAX_ENUM;

    return flags;
}

static int create_vulkan_image_memory(VkInstance instance, VkPhysicalDevice phys_dev,
                                      VkDevice dev, VkSwapchainCreateInfoKHR chain_create_info,
                                      VkImage image, VkDeviceMemory *image_mem)
{
    int32_t mem_type_index = -1;
    uint32_t flags;
    unsigned int i;
    PFN_vkGetImageMemoryRequirements pfn_vkGetImageMemoryRequirements =
        vulkan_funcs.p_vkGetDeviceProcAddr(dev, "vkGetImageMemoryRequirements");
    PFN_vkAllocateMemory pfn_vkAllocateMemory =
        vulkan_funcs.p_vkGetDeviceProcAddr(dev, "vkAllocateMemory");
    PFN_vkGetPhysicalDeviceMemoryProperties pfn_vkGetPhysicalDeviceMemoryProperties =
        vulkan_funcs.p_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties");
    VkMemoryRequirements mem_reqs;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkExportMemoryAllocateInfo export_alloc_info = {0};
    VkMemoryAllocateInfo alloc_info = {0};
    VkResult res;

    if (!pfn_vkGetImageMemoryRequirements || !pfn_vkAllocateMemory ||
        !pfn_vkGetPhysicalDeviceMemoryProperties) {
        WARN("vkGetImageMemoryRequirements, vkAllocateMemory or " \
             "vkGetPhysicalDeviceMemoryProperties not present\n");
        return -1;
    }

    export_alloc_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_alloc_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = &export_alloc_info;

    pfn_vkGetImageMemoryRequirements(dev, image, &mem_reqs);
    pfn_vkGetPhysicalDeviceMemoryProperties(phys_dev, &mem_props);

    flags = get_memory_property_flags(chain_create_info);
    for (i = 0; i < mem_props.memoryTypeCount; i++) {
        if (mem_reqs.memoryTypeBits & (1 << i))
            if ((mem_props.memoryTypes[i].propertyFlags & flags) == flags)
                mem_type_index = i;
    }
    if (mem_type_index == -1) {
        WARN("Failed to find memoryTypeIndex\n");
        return -1;
    }

    alloc_info.memoryTypeIndex = mem_type_index;
    alloc_info.allocationSize = mem_reqs.size;

    res = pfn_vkAllocateMemory(dev, &alloc_info, NULL, image_mem);
    if (res != VK_SUCCESS) return -1;

    return 0;
}

static void dmabuf_buffer_release(void *data, struct wl_buffer *buffer)
{
    struct wine_vk_image *image = (struct wine_vk_image *) data;

    image->busy = FALSE;
}

static const struct wl_buffer_listener dmabuf_buffer_listener = {
    dmabuf_buffer_release
};

VkResult wayland_vulkan_create_internal_swapchain_image(struct wine_vk_swapchain *wine_vk_swapchain,
                                                        VkPhysicalDevice phys_dev, VkDevice dev,
                                                        VkSwapchainCreateInfoKHR chain_create_info,
                                                        struct wine_vk_image *image)
{
    BOOL ignore_alpha;
    PFN_vkBindImageMemory pfn_vkBindImageMemory =
        vulkan_funcs.p_vkGetDeviceProcAddr(dev, "vkBindImageMemory");
    VkDeviceMemory image_mem;
    VkInstance instance = wine_vk_swapchain->instance;
    VkResult res;

    if (!pfn_vkBindImageMemory) {
	    WARN("vkBindImageMemory not present\n");
        goto err;
    }

    if (create_vulkan_image(dev, chain_create_info, &image->native_vk_image) < 0)
        goto err;
    if (create_vulkan_image_memory(instance, phys_dev, dev, chain_create_info,
                                   image->native_vk_image, &image_mem) < 0)
        goto err;

    res = pfn_vkBindImageMemory(dev, image->native_vk_image, image_mem, 0);
    if (res != VK_SUCCESS) goto err;

    image->busy = FALSE;
    image->format = chain_create_info.imageFormat;
    image->width = chain_create_info.imageExtent.width;
    image->height = chain_create_info.imageExtent.height;

    ignore_alpha = chain_create_info.compositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (wayland_native_buffer_init_vk(instance, phys_dev, dev, image_mem,
                                      image, ignore_alpha) < 0) goto err;

    image->dmabuf_buffer =
        wayland_dmabuf_buffer_create_from_native(wine_vk_swapchain->wayland_surface->wayland,
                                                 &image->native_buffer);
    if (!image->dmabuf_buffer) goto err;

    wayland_native_buffer_deinit(&image->native_buffer);

    wl_proxy_set_queue((struct wl_proxy *) image->dmabuf_buffer->wl_buffer,
                       wine_vk_swapchain->wl_event_queue);
    wl_buffer_add_listener(image->dmabuf_buffer->wl_buffer,
                           &dmabuf_buffer_listener, image);

    return VK_SUCCESS;

err:
    WARN("Internal swapchain image creation failed\n");
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult wayland_vulkan_internal_swapchain_get_images(VkDevice device, VkSwapchainKHR swapchain,
                                                      uint32_t *count, VkImage *images)
{
    struct wine_vk_swapchain *wine_vk_swapchain = wine_vk_swapchain_from_handle(swapchain);
    unsigned int i;

    if (!images) {
        *count = wine_vk_swapchain->count_internal_images;
        return VK_SUCCESS;
    }

    if (*count > wine_vk_swapchain->count_internal_images) {
        WARN("Application wants more Vulkan images than we have\n");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    for (i = 0; i < (*count); i++)
        images[i] = wine_vk_swapchain->internal_images[i].native_vk_image;

    if (*count < wine_vk_swapchain->count_internal_images)
        return VK_INCOMPLETE;

    return VK_SUCCESS;
}

VkResult wayland_vulkan_internal_swapchain_acquire_next_image(VkDevice device, VkSwapchainKHR swapchain,
                                                              uint64_t timeout, VkSemaphore semaphore,
                                                              VkFence fence, uint32_t *image_index)
{
    struct wine_vk_swapchain *wine_vk_swapchain = wine_vk_swapchain_from_handle(swapchain);
    unsigned int i;
    BOOL found_non_busy = FALSE;
    struct timespec start_time, current_time;
    PFN_vkImportSemaphoreFdKHR pfn_vkImportSemaphoreFdKHR =
        vulkan_funcs.p_vkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR");
    PFN_vkImportFenceFdKHR pfn_vkImportFenceFdKHR =
        vulkan_funcs.p_vkGetDeviceProcAddr(device, "vkImportFenceFdKHR");
    VkImportSemaphoreFdInfoKHR import_semaphore_fd_info = {0};
    VkImportFenceFdInfoKHR import_fence_fd_info = {0};
    VkResult res;

    if (!pfn_vkImportSemaphoreFdKHR && semaphore != VK_NULL_HANDLE) {
        WARN("vkImportSemaphoreFdKHR not present\n");
        goto err;
    }
    if (!pfn_vkImportFenceFdKHR && fence != VK_NULL_HANDLE) {
        WARN("vkImportFenceFdKHR not present\n");
        goto err;
    }

    import_semaphore_fd_info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    import_semaphore_fd_info.fd = -1;
    import_semaphore_fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    import_semaphore_fd_info.semaphore = semaphore;
    import_semaphore_fd_info.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;

    import_fence_fd_info.sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR;
    import_fence_fd_info.fd = -1;
    import_fence_fd_info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    import_fence_fd_info.fence = fence;
    import_fence_fd_info.flags = VK_FENCE_IMPORT_TEMPORARY_BIT;

    if (timeout > 0)
        clock_gettime(CLOCK_MONOTONIC, &start_time);

    while (!found_non_busy) {
        for (i = 0; i < wine_vk_swapchain->count_internal_images; i++)
            if (!wine_vk_swapchain->internal_images[i].busy) {
                found_non_busy = TRUE;
                break;
            }
        if (!found_non_busy &&
            wayland_dispatch_queue(wine_vk_swapchain->wl_event_queue, -1) < 0)
            goto err;

        if (timeout > 0) {
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            if (current_time.tv_nsec - start_time.tv_nsec > timeout)
                return VK_TIMEOUT;
        }
    }

    *image_index = i;

    if (semaphore != VK_NULL_HANDLE) {
        res = pfn_vkImportSemaphoreFdKHR(device, &import_semaphore_fd_info);
        if (res != VK_SUCCESS) {
            WARN("Failed to import load to Vulkan semaphore\n");
            goto err;
        }
    }
    if (fence != VK_NULL_HANDLE) {
        res = pfn_vkImportFenceFdKHR(device, &import_fence_fd_info);
        if (res != VK_SUCCESS) {
            WARN("Failed to import load to Vulkan fence\n");
            goto err;
        }
    }

    return VK_SUCCESS;

err:
    WARN("Failed to acquire image from internal Vulkan swapchain");
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void frame_callback_handle(void *data, struct wl_callback *callback, uint32_t time)
{
    struct wine_vk_swapchain *wine_vk_swapchain = (struct wine_vk_swapchain *) data;

    wine_vk_swapchain->frame_callback = NULL;
    wine_vk_swapchain->ready = TRUE;

    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_callback_listener = {
    frame_callback_handle
};

static int present_swapchain(VkSwapchainKHR swapchain, uint32_t image_index)
{
    struct wine_vk_swapchain *wine_vk_swapchain = wine_vk_swapchain_from_handle(swapchain);
    struct wine_vk_image *wine_vk_image;
    struct wl_surface *vk_wl_surface;

    vk_wl_surface = wine_vk_swapchain->wayland_surface->glvk->wl_surface;

    wine_vk_image = &wine_vk_swapchain->internal_images[image_index];
    wine_vk_image->busy = TRUE;

    if (wine_vk_swapchain->wait_for_callback)
        while (!wine_vk_swapchain->ready)
            if (wayland_dispatch_queue(wine_vk_swapchain->wl_event_queue, 500) < 0)
                goto err;

    wayland_mutex_lock(&wine_vk_swapchain->wayland_surface->mutex);
    if (wine_vk_swapchain->wayland_surface->drawing_allowed)
    {
        wayland_surface_ensure_mapped(wine_vk_swapchain->wayland_surface);

        wl_surface_attach(vk_wl_surface, wine_vk_image->dmabuf_buffer->wl_buffer, 0, 0);
        wl_surface_damage_buffer(vk_wl_surface, 0, 0, INT32_MAX, INT32_MAX);

        if (wine_vk_swapchain->wait_for_callback)
        {
            wine_vk_swapchain->frame_callback = wl_surface_frame(vk_wl_surface);
            wl_proxy_set_queue((struct wl_proxy *) wine_vk_swapchain->frame_callback,
                               wine_vk_swapchain->wl_event_queue);
            wl_callback_add_listener(wine_vk_swapchain->frame_callback,
                                     &frame_callback_listener, wine_vk_swapchain);
            wine_vk_swapchain->ready = FALSE;
        }
        wl_surface_commit(vk_wl_surface);
    }
    wayland_mutex_unlock(&wine_vk_swapchain->wayland_surface->mutex);

    return 0;

err:
    WARN("Failed to present internal Vulkan swapchain\n");
    return -1;
}

static int wait_for_semaphores(VkDevice dev, const VkPresentInfoKHR *present_info)
{
    struct pollfd pollfd;
    int semaphore_fd = -1;
    unsigned int i;
    int ret;
    PFN_vkGetSemaphoreFdKHR pfn_vkGetSemaphoreFdKHR =
        vulkan_funcs.p_vkGetDeviceProcAddr(dev, "vkGetSemaphoreFdKHR");
    VkSemaphoreGetFdInfoKHR get_fd_info = {0};
    VkResult res;

    if (present_info->waitSemaphoreCount == 0)
        return 0;

    if (!pfn_vkGetSemaphoreFdKHR) {
        WARN("vkGetSemaphoreFdKHR not present\n");
        goto err;
    }

    get_fd_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    get_fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

    for (i = 0, semaphore_fd = -1; i < present_info->waitSemaphoreCount; i++) {
        /* Current semaphore to wait for */
        get_fd_info.semaphore = present_info->pWaitSemaphores[i];

        res = pfn_vkGetSemaphoreFdKHR(dev, &get_fd_info, &semaphore_fd);
        if (res != VK_SUCCESS) {
            WARN("Failed to get semaphore fd\n");
            goto err;
        }
        if (semaphore_fd < 0) {
            WARN("Invalid semaphore fd\n");
            goto err;
        }

        pollfd.fd = semaphore_fd;
        pollfd.events = POLLIN;

        while ((ret = poll(&pollfd, 1, -1)) == -1 && errno == EINTR) continue;
        if (ret < 0) {
            WARN("Poll fd failed errno=%d\n", errno);
            goto err;
        }
        if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            WARN("Poll fd failed\n");
            goto err;
        }

        close(semaphore_fd);
    }

    return 0;

err:
    WARN("Failed to wait for semaphores before presenting queue\n");
    if (semaphore_fd >= 0)
        close(semaphore_fd);
    return -1;
}

VkResult wayland_vulkan_internal_swapchain_queue_present(const VkPresentInfoKHR *present_info)
{
    BOOL failed = FALSE;
    int res_chain;
    unsigned int i;
    VkDevice dev;

    if (present_info->swapchainCount == 0 || !present_info->pSwapchains) {
        WARN("Invalid number of swapchains to present: 0\n");
        goto err;
    }

    dev = wine_vk_swapchain_from_handle(present_info->pSwapchains[0])->device;
    if (wait_for_semaphores(dev, present_info) < 0)
        goto err;

    for (i = 0; i < present_info->swapchainCount; i++) {
        res_chain = present_swapchain(present_info->pSwapchains[i],
                                      present_info->pImageIndices[i]);

        /* If presenting one of the chains failed, this function fails */
        if (res_chain < 0) failed = TRUE;

        if (present_info->pResults)
            present_info->pResults[i] = (res_chain < 0) ?
                                        VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
    }

    if (failed) goto err;

    return VK_SUCCESS;

err:
    WARN("Failed to present queue\n");
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}
