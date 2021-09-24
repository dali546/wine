/*
 * Wayland output handling
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

#include <stdlib.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

struct default_mode { int32_t width; int32_t height; };
struct default_mode default_modes[] = {
    /* 4:3 */
    { 320,  240},
    { 400,  300},
    { 512,  384},
    { 640,  480},
    { 768,  576},
    { 800,  600},
    {1024,  768},
    {1152,  864},
    {1280,  960},
    {1400, 1050},
    {1600, 1200},
    {2048, 1536},
    /* 5:4 */
    {1280, 1024},
    {2560, 2048},
    /* 16:9 */
    {1280,  720},
    {1366,  768},
    {1600,  900},
    {1920, 1080},
    {2560, 1440},
    {3200, 1800},
    {3840, 2160},
    /* 16:10 */
    { 320,  200},
    { 640,  400},
    {1280,  800},
    {1440,  900},
    {1680, 1050},
    {1920, 1200},
    {2560, 1600},
    {3840, 2400}
};

static CRITICAL_SECTION output_names_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &output_names_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": output_names_section") }
};
static CRITICAL_SECTION output_names_section = { &critsect_debug, -1, 0, 0, 0, 0 };

static struct wl_array output_names = { 0, 0, 0 };

/**********************************************************************
 *          Output handling
 */

static void wayland_output_add_mode(struct wayland_output *output,
                                    int32_t width, int32_t height,
                                    int32_t refresh, int bpp,
                                    BOOL current, BOOL native)
{
    struct wayland_output_mode *mode;

    /* Update mode if already in list */
    wl_list_for_each(mode, &output->mode_list, link)
    {
        if (mode->width == width && mode->height == height &&
            mode->refresh == refresh && mode->bpp == bpp)
        {
            /* Upgrade modes from virtual to native, never the reverse. */
            if (native) mode->native = TRUE;
            if (current)
            {
                output->current_mode = mode;
                output->current_wine_mode = mode;
            }
            return;
        }
    }

    mode = heap_alloc_zero(sizeof(*mode));

    mode->width = width;
    mode->height = height;
    mode->refresh = refresh;
    mode->bpp = bpp;
    mode->native = native;

    if (current)
    {
        output->current_mode = mode;
        output->current_wine_mode = mode;
    }

    wl_list_insert(&output->mode_list, &mode->link);
}

static void wayland_output_add_mode_all_bpp(struct wayland_output *output,
                                            int32_t width, int32_t height,
                                            int32_t refresh, BOOL current,
                                            BOOL native)
{
    wayland_output_add_mode(output, width, height, refresh, 32, current, native);
    wayland_output_add_mode(output, width, height, refresh, 16, FALSE, FALSE);
    wayland_output_add_mode(output, width, height, refresh, 8, FALSE, FALSE);
}

/* The id of a name is its index in the output_names array + 1 + 0xd1d10000.
 * This peculiar id is used for easier differentiation and debugging. */
static uint32_t wayland_output_name_get_id(const char *output_name)
{
    uint32_t id = 0;
    char **name;

    if (!output_name) return 0;

    EnterCriticalSection(&output_names_section);

    wl_array_for_each(name, &output_names)
    {
        id++;
        if (*name && !strcmp(*name, output_name))
            goto out;
    }

    id++;

    name = wl_array_add(&output_names, sizeof(const char*));
    if (!name || !(*name = strdup(output_name)))
    {
        ERR("Failed to allocate memory for output name");
        id = 0;
        goto out;
    }

out:
    LeaveCriticalSection(&output_names_section);
    return id > 0 ? id + 0xd1d10000 : 0;
}

static void wayland_output_add_default_modes(struct wayland_output *output)
{
    int i;
    struct wayland_output_mode *mode, *tmp;
    int32_t max_width = 0;
    int32_t max_height = 0;

    /* Remove all existing virtual modes and get the maximum native
     * mode size. */
    wl_list_for_each_safe(mode, tmp, &output->mode_list, link)
    {
        if (!mode->native)
        {
            wl_list_remove(&mode->link);
            heap_free(mode);
        }
        else
        {
            max_width = mode->width > max_width ? mode->width : max_width;
            max_height = mode->height > max_height ? mode->height : max_height;
        }
    }

    for (i = 0; i < ARRAY_SIZE(default_modes); i++)
    {
        int32_t width = default_modes[i].width;
        int32_t height = default_modes[i].height;

        /* Skip if this mode is larger than the largest native mode. */
        if (width > max_width || height > max_height)
        {
            TRACE("Skipping mode %dx%d (max: %dx%d)\n",
                    width, height, max_width, max_height);
            continue;
        }

        wayland_output_add_mode_all_bpp(output, width, height, 60000, FALSE, FALSE);
    }
}

static struct wayland_output **
wayland_output_array_append(struct wayland_output **array, int size,
                            struct wayland_output *output)
{
    struct wayland_output **realloc_array;

    realloc_array = heap_realloc(array, sizeof(*array) * size);
    if (!realloc_array)
    {
        heap_free(array);
        return NULL;
    }

    realloc_array[size - 1] = output;

    return realloc_array;
}

static void wayland_output_update_physical_coords(struct wayland_output *output)
{
    struct wayland_output *o;
    struct wayland_output **changed = NULL;
    int changed_size = 0;
    int changed_i = 0;

    /* Set some default values. */
    output->x = output->logical_x;
    output->y = output->logical_y;

    /* When compositor scaling is used, we treat logical coordinates as
     * physical. */
    if (output->wayland->hidpi_scaling == WAYLAND_HIDPI_SCALING_COMPOSITOR)
        return;

    /* Update output->x,y based on other outputs that are to
     * to the left or above. */
    wl_list_for_each(o, &output->wayland->output_list, link)
    {
        if (o == output || o->logical_w == 0 || o->logical_h == 0) continue;
        if (output->logical_x == o->logical_x + o->logical_w)
            output->x = o->x + o->current_mode->width;
        if (output->logical_y == o->logical_y + o->logical_h)
            output->y = o->y + o->current_mode->height;
    }

    changed = wayland_output_array_append(changed, ++changed_size, output);
    if (!changed) { ERR("memory allocation failed"); return; }

    /* Update the x,y of other outputs that are to the right or below and are
     * directly or indirectly affected by the change output->x,y.
     */
    for (changed_i = 0; changed_i < changed_size; changed_i++)
    {
        struct wayland_output *cur = changed[changed_i];
        wl_list_for_each(o, &output->wayland->output_list, link)
        {
            if (o == cur || o->logical_w == 0 || o->logical_h == 0) continue;
            if (o->logical_x == cur->logical_x + cur->logical_w)
            {
                o->x = cur->x + cur->current_mode->width;
                changed = wayland_output_array_append(changed, ++changed_size, o);
                if (!changed) { ERR("memory allocation failed"); return; }
            }
            if (o->logical_y == cur->logical_y + cur->logical_h)
            {
                o->y = cur->y + cur->current_mode->height;
                changed = wayland_output_array_append(changed, ++changed_size, o);
                if (!changed) { ERR("memory allocation failed"); return; }
            }
        }
    }

    heap_free(changed);
}

static void wayland_output_clear_modes(struct wayland_output *output)
{
    struct wayland_output_mode *mode, *tmp;

    wl_list_for_each_safe(mode, tmp, &output->mode_list, link)
    {
        wl_list_remove(&mode->link);
        heap_free(mode);
    }
}

static void wayland_output_done(struct wayland_output *output)
{
    struct wayland_output_mode *mode;
    struct wayland_output *o;

    TRACE("output->name=%s\n", output->name);

    /* When compositor scaling is used, the current and only native mode
     * corresponds to the logical width and height. */
    if (output->wayland->hidpi_scaling == WAYLAND_HIDPI_SCALING_COMPOSITOR)
    {
        wayland_output_clear_modes(output);
        wayland_output_add_mode_all_bpp(output, output->logical_w, output->logical_h,
                                        60000, TRUE, TRUE);
    }

    wayland_output_add_default_modes(output);
    wayland_output_update_physical_coords(output);

    wl_list_for_each(mode, &output->mode_list, link)
    {
        TRACE("mode %dx%d @ %d %s\n",
              mode->width, mode->height, mode->refresh,
              output->current_mode == mode ? "*" : "");
    }

    wl_list_for_each(o, &output->wayland->output_list, link)
    {
        if (!o->current_mode) continue;
        TRACE("output->name=%s logical=%d,%d+%dx%d physical=%d,%d+%dx%d\n",
              o->name,
              o->logical_x, output->logical_y, o->logical_w, o->logical_h,
              o->x, o->y, o->current_mode->width, o->current_mode->height);
    }

    wayland_init_display_devices(output->wayland);
}

static void output_handle_geometry(void *data, struct wl_output *wl_output,
                                   int32_t x, int32_t y,
                                   int32_t physical_width, int32_t physical_height,
                                   int32_t subpixel,
                                   const char *make, const char *model,
                                   int32_t output_transform)
{
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
                               uint32_t flags, int32_t width, int32_t height,
                               int32_t refresh)
{
    struct wayland_output *output = data;

    /* When compositor scaling is used, we don't use physical width/height
     * for modes and the current mode will be set based on logical width
     * and height (see wayland_output_handle()). */
    if (output->wayland->hidpi_scaling == WAYLAND_HIDPI_SCALING_COMPOSITOR)
        return;

    wayland_output_add_mode_all_bpp(output, width, height, refresh,
                                    (flags & WL_OUTPUT_MODE_CURRENT),
                                    TRUE);
}

static void output_handle_done(void *data, struct wl_output *wl_output)
{
    struct wayland_output *output = data;
    if (!output->zxdg_output_v1 ||
        zxdg_output_v1_get_version(output->zxdg_output_v1) >= 3)
    {
        wayland_output_done(output);
    }
}

static void output_handle_scale(void *data, struct wl_output *wl_output,
                                int32_t scale)
{
    struct wayland_output *output = data;
    TRACE("output=%p scale=%d\n", output, scale);
    /* When compositor scaling is used, we ignore the output scale, to
     * allow the the compositor to scale us. */
    if (output->wayland->hidpi_scaling != WAYLAND_HIDPI_SCALING_COMPOSITOR)
        output->scale = scale;
}

static const struct wl_output_listener output_listener = {
    output_handle_geometry,
    output_handle_mode,
    output_handle_done,
    output_handle_scale
};

static void zxdg_output_v1_handle_logical_position(void *data,
                                                   struct zxdg_output_v1 *zxdg_output_v1,
                                                   int32_t x,
                                                   int32_t y)
{
    struct wayland_output *output = data;
    TRACE("logical_x=%d logical_y=%d\n", x, y);
    output->logical_x = x;
    output->logical_y = y;
}

static void zxdg_output_v1_handle_logical_size(void *data,
                                               struct zxdg_output_v1 *zxdg_output_v1,
                                               int32_t width,
                                               int32_t height)
{
    struct wayland_output *output = data;
    TRACE("logical_w=%d logical_h=%d\n", width, height);
    output->logical_w = width;
    output->logical_h = height;
}

static void zxdg_output_v1_handle_done(void *data,
                                       struct zxdg_output_v1 *zxdg_output_v1)
{
    if (zxdg_output_v1_get_version(zxdg_output_v1) < 3)
    {
        struct wayland_output *output = data;
        wayland_output_done(output);
    }
}

static void zxdg_output_v1_handle_name(void *data,
                                       struct zxdg_output_v1 *zxdg_output_v1,
                                       const char *name)
{
    struct wayland_output *output = data;

    free(output->name);
    output->name = strdup(name);
    output->id = wayland_output_name_get_id(output->name);
}

static void zxdg_output_v1_handle_description(void *data,
                                              struct zxdg_output_v1 *zxdg_output_v1,
                                              const char *description)
{
}

static const struct zxdg_output_v1_listener zxdg_output_v1_listener = {
    zxdg_output_v1_handle_logical_position,
    zxdg_output_v1_handle_logical_size,
    zxdg_output_v1_handle_done,
    zxdg_output_v1_handle_name,
    zxdg_output_v1_handle_description,
};

/**********************************************************************
 *          wayland_output_create
 *
 *  Creates a wayland_output and adds it to the output list.
 */
BOOL wayland_output_create(struct wayland *wayland, uint32_t id, uint32_t version)
{
    struct wayland_output *output = heap_alloc_zero(sizeof(*output));

    if (!output)
    {
        ERR("Couldn't allocate space for wayland_output\n");
        goto err;
    }

    output->wayland = wayland;
    output->wl_output = wl_registry_bind(wayland->wl_registry, id,
                                         &wl_output_interface,
                                         version < 2 ? version : 2);
    output->global_id = id;
    wl_output_add_listener(output->wl_output, &output_listener, output);

    wl_list_init(&output->mode_list);
    wl_list_init(&output->link);

    output->scale = 1;
    output->wine_scale = 1.0;

    /* Have a fallback in case xdg_output is not supported or name is not sent. */
    output->name = malloc(20);
    if (output->name)
    {
        snprintf(output->name, 20, "WaylandOutput%d",
                 wayland->next_fallback_output_id++);
        output->id = wayland_output_name_get_id(output->name);
    }
    else
    {
        ERR("Couldn't allocate space for output name\n");
        goto err;
    }

    if (wayland->zxdg_output_manager_v1)
        wayland_output_use_xdg_extension(output);

    wl_list_insert(output->wayland->output_list.prev, &output->link);

    return TRUE;

err:
    if (output) wayland_output_destroy(output);
    return FALSE;
}

/**********************************************************************
 *          wayland_output_destroy
 *
 *  Destroys a wayland_output.
 */
void wayland_output_destroy(struct wayland_output *output)
{
    wayland_output_clear_modes(output);
    wl_list_remove(&output->link);
    free(output->name);
    if (output->zxdg_output_v1)
        zxdg_output_v1_destroy(output->zxdg_output_v1);
    wl_output_destroy(output->wl_output);

    heap_free(output);
}

/**********************************************************************
 *          wayland_output_use_xdg_extension
 *
 *  Use the zxdg_output_v1 extension to get output information.
 */
void wayland_output_use_xdg_extension(struct wayland_output *output)
{
    output->zxdg_output_v1 =
        zxdg_output_manager_v1_get_xdg_output(output->wayland->zxdg_output_manager_v1,
                                              output->wl_output);
    zxdg_output_v1_add_listener(output->zxdg_output_v1, &zxdg_output_v1_listener,
                                output);
}

/**********************************************************************
 *          wayland_output_get_by_wine_name
 *
 *  Returns the wayland_output with the specified Wine name (or NULL
 *  if not present).
 */
struct wayland_output *wayland_output_get_by_wine_name(struct wayland *wayland,
                                                       LPCWSTR wine_name)
{
    struct wayland_output *output;

    wl_list_for_each(output, &wayland->output_list, link)
    {
        if (!lstrcmpiW(wine_name, output->wine_name))
            return output;
    }

    return NULL;
}

/**********************************************************************
 *          wayland_output_get_by_id
 *
 *  Returns the wayland_output with the specified id (or NULL
 *  if not present).
 */
struct wayland_output *wayland_output_get_by_id(struct wayland *wayland,
                                                uint32_t output_id)
{
    struct wayland_output *output;

    wl_list_for_each(output, &wayland->output_list, link)
    {
        if (output->id == output_id)
            return output;
    }

    return NULL;
}

/**********************************************************************
 *          wayland_output_set_wine_mode
 *
 * Set the current wine mode for the specified output.
 */
void wayland_output_set_wine_mode(struct wayland_output *output, int width, int height)
{
    struct wayland_output_mode *output_mode;

    TRACE("output->name=%s (id=0x%x) width=%d height=%d\n",
          output->name, output->id, width, height);

    /* We always use 32bpp modes since that's the only one we really
     * support. */
    wl_list_for_each(output_mode, &output->mode_list, link)
    {
        if (output_mode->width == width && output_mode->height == height &&
            output_mode->bpp == 32)
        {
            output->current_wine_mode = output_mode;
            break;
        }
    }

    if (!output->current_wine_mode || !output->current_mode)
    {
        output->wine_scale = 1.0;
    }
    else
    {
        double scale_x = ((double)output->current_mode->width) /
                         output->current_wine_mode->width;
        double scale_y = ((double)output->current_mode->height) /
                         output->current_wine_mode->height;
        /* We want to keep the aspect ratio of the target mode. */
        output->wine_scale = fmin(scale_x, scale_y);
    }
}
