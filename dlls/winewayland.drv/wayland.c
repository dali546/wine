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
#include "wine/port.h"

#include "waylanddrv.h"
#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/unicode.h"
#include "winuser.h"
#include <linux/input.h>

#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include <poll.h>
#include <sys/mman.h>
#include <limits.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

struct wl_display *process_wl_display = NULL;

static CRITICAL_SECTION thread_wayland_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &thread_wayland_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": thread_wayland_section") }
};
static CRITICAL_SECTION thread_wayland_section = { &critsect_debug, -1, 0, 0, 0, 0 };

static struct wl_list thread_wayland_list = {&thread_wayland_list, &thread_wayland_list};

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

/**********************************************************************
 *          Output handling
 */

static void wayland_output_add_mode(struct wayland_output *output,
                                    int32_t width, int32_t height,
                                    int32_t refresh, BOOL current,
                                    BOOL native)
{
    struct wayland_output_mode *mode;

    /* Update mode if already in list */
    wl_list_for_each(mode, &output->mode_list, link)
    {
        if (mode->width == width && mode->height == height &&
            mode->refresh == refresh)
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
    mode->native = native;

    if (current)
    {
        output->current_mode = mode;
        output->current_wine_mode = mode;
    }

    wl_list_insert(&output->mode_list, &mode->link);
}

/* The output id is computed using the FNV-1a hash of the name. We start
 * with the default FNV offset basis, but we update it (and recompute
 * all ids) if we find a collision. From the author's page at
 * http://www.isthe.com/chongo/tech/comp/fnv/index.html:
 *
 *   "In the general case, almost any offset_basis will serve so long as
 *    it is non-zero"
 */
static void wayland_output_recompute_id(struct wayland_output *output)
{
    static const uint32_t fnv_prime = 0x01000193;
    uint32_t hash = output->wayland->output_id_fnv_offset;
    const char *p;

    if (output->name)
    {
        for(p = output->name; *p; p++)
        {
            hash ^= *p;
            hash *= fnv_prime;
        }
    }

    output->id = hash;
}

static BOOL wayland_output_ids_conflict(struct wayland *wayland)
{
    struct wayland_output *o;
    struct wayland_output *n;

    wl_list_for_each(o, &wayland->output_list, link)
    {
        for (n = wl_container_of(o->link.next, n, link);
             &n->link != &wayland->output_list;
             n = wl_container_of(n->link.next, n, link))
        {
            if (o->id == n->id) return TRUE;
        }
    }

    return FALSE;
}

static void wayland_recompute_output_ids_until_no_conflict(struct wayland *wayland)
{
    while (wayland_output_ids_conflict(wayland))
    {
        struct wayland_output *output;

        wayland->output_id_fnv_offset += 2;
        TRACE("recomputing output ids using fnv_offset=0x%x\n",
              wayland->output_id_fnv_offset);

        wl_list_for_each(output, &wayland->output_list, link)
            wayland_output_recompute_id(output);
    }
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

        wayland_output_add_mode(output, width, height, 60000, FALSE, FALSE);
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
        wayland_output_add_mode(output, output->logical_w, output->logical_h,
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

    wayland_output_add_mode(output, width, height, refresh,
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
    wayland_output_recompute_id(output);

    wayland_recompute_output_ids_until_no_conflict(output->wayland);
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

static void wayland_add_output(struct wayland *wayland, uint32_t id, uint32_t version)
{
    struct wayland_output *output = heap_alloc_zero(sizeof(*output));

    output->wayland = wayland;
    output->wl_output = wl_registry_bind(wayland->wl_registry, id,
                                         &wl_output_interface,
                                         version < 2 ? version : 2);
    output->global_id = id;
    wl_output_add_listener(output->wl_output, &output_listener, output);

    if (wayland->zxdg_output_manager_v1)
    {
        output->zxdg_output_v1 =
            zxdg_output_manager_v1_get_xdg_output(wayland->zxdg_output_manager_v1,
                                                  output->wl_output);
        zxdg_output_v1_add_listener(output->zxdg_output_v1, &zxdg_output_v1_listener,
                                    output);
    }

    wl_list_init(&output->mode_list);

    output->scale = 1;
    output->wine_scale = 1.0;

    wl_list_insert(output->wayland->output_list.prev, &output->link);

    /* Fallbacks if xdg_output is not supported or name not sent. */
    output->name = malloc(20);
    if (output->name)
    {
        snprintf(output->name, 20, "WaylandOutput%d",
                 wayland->next_fallback_output_id++);
        wayland_output_recompute_id(output);
        wayland_recompute_output_ids_until_no_conflict(output->wayland);
    }
    else
    {
        ERR("Couldn't allocate space for output name\n");
    }

}

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
 *          xdg_wm_base handling
 */

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    xdg_wm_base_ping,
};

static struct wayland_surface *get_wayland_surface(struct wayland *wayland,
                                                   struct wl_surface *wl_surface)
{
    struct wayland_surface *surface;

    wl_list_for_each(surface, &wayland->surface_list, link)
    {
        if (surface->wl_surface == wl_surface)
            return surface;
    }

    return NULL;
}

/**********************************************************************
 *          Keyboard handling
 */

static void keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
                                   uint32_t format, int fd, uint32_t size)
{
    struct wayland *wayland = data;
    struct xkb_keymap *xkb_keymap = NULL;
    struct xkb_state *xkb_state = NULL;
    char *keymap_str;

    TRACE("format=%d fd=%d size=%d\n", format, fd, size);

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 ||
        !wayland->keyboard.xkb_context)
        goto out;

    keymap_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (!keymap_str)
        goto out;

    xkb_keymap = xkb_keymap_new_from_string(wayland->keyboard.xkb_context,
                                            keymap_str,
                                            XKB_KEYMAP_FORMAT_TEXT_V1,
                                            0);
    munmap(keymap_str, size);
    if (!xkb_keymap)
        goto out;

    xkb_state = xkb_state_new(xkb_keymap);
    xkb_keymap_unref(xkb_keymap);
    if (!xkb_state)
        goto out;

    xkb_state_unref(wayland->keyboard.xkb_state);
    wayland->keyboard.xkb_state = xkb_state;
    if (wayland->keyboard.xkb_compose_state)
        xkb_compose_state_reset(wayland->keyboard.xkb_compose_state);

    wayland_keyboard_update_layout(&wayland->keyboard);

out:
    close(fd);
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
                                  uint32_t serial, struct wl_surface *surface,
                                  struct wl_array *keys)
{
    struct wayland *wayland = data;
    struct wayland_surface *wayland_surface = get_wayland_surface(wayland, surface);

    if (wayland_surface && wayland_surface->hwnd)
    {
        TRACE("surface=%p hwnd=%p\n", wayland_surface, wayland_surface->hwnd);
        wayland->keyboard.focused_surface = wayland_surface;
        wayland->keyboard.enter_serial = serial;
    }
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, struct wl_surface *surface)
{
    struct wayland *wayland = data;

    if (wayland->keyboard.focused_surface &&
        wayland->keyboard.focused_surface->wl_surface == surface)
    {
        TRACE("surface=%p hwnd=%p\n",
              wayland->keyboard.focused_surface,
              wayland->keyboard.focused_surface->hwnd);
        KillTimer(wayland->keyboard.focused_surface->hwnd, (UINT_PTR)keyboard);
        wayland->keyboard.focused_surface = NULL;
        wayland->keyboard.enter_serial = 0;
    }
}

static void CALLBACK repeat_key(HWND hwnd, UINT msg, UINT_PTR timer_id, DWORD elapsed)
{
    struct wayland *wayland = thread_wayland();

    if (wayland->keyboard.repeat_interval_ms > 0)
    {
        wayland_keyboard_emit(&wayland->keyboard, wayland->keyboard.pressed_key,
                              WL_KEYBOARD_KEY_STATE_PRESSED, hwnd);

        SetTimer(hwnd, timer_id, wayland->keyboard.repeat_interval_ms,
                 repeat_key);
    }
}

static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
                                uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t state)
{
    struct wayland *wayland = data;
    HWND focused_hwnd = wayland->keyboard.focused_surface ?
                        wayland->keyboard.focused_surface->hwnd : 0;
    UINT_PTR repeat_key_timer_id = (UINT_PTR)keyboard;

    if (!focused_hwnd)
        return;

    TRACE("key=%d state=%#x focused_hwnd=%p\n", key, state, focused_hwnd);

    wayland->last_dispatch_mask |= QS_KEY | QS_HOTKEY;
    wayland->last_event_type = INPUT_KEYBOARD;

    wayland_keyboard_emit(&wayland->keyboard, key, state, focused_hwnd);

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
    {
        wayland->keyboard.pressed_key = key;
        if (wayland->keyboard.repeat_interval_ms > 0)
        {
            SetTimer(focused_hwnd, repeat_key_timer_id, wayland->keyboard.repeat_delay_ms,
                     repeat_key);
        }
    }
    else
    {
        wayland->keyboard.pressed_key = 0;
        KillTimer(focused_hwnd, repeat_key_timer_id);
    }
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
                                      uint32_t serial, uint32_t mods_depressed,
                                      uint32_t mods_latched, uint32_t mods_locked,
                                      uint32_t group)
{
    struct wayland *wayland = data;
    uint32_t last_group;

    TRACE("depressed=0x%x latched=0x%x locked=0x%x group=%d\n",
          mods_depressed, mods_latched, mods_locked, group);

    if (!wayland->keyboard.xkb_state) return;

    last_group = _xkb_state_get_active_layout(wayland->keyboard.xkb_state);

    xkb_state_update_mask(wayland->keyboard.xkb_state,
                          mods_depressed, mods_latched, mods_locked, 0, 0, group);

    if (group != last_group)
        wayland_keyboard_update_layout(&wayland->keyboard);

}

static void keyboard_handle_repeat_info(void *data, struct wl_keyboard *keyboard,
                                        int rate, int delay)
{
    struct wayland *wayland = data;

    TRACE("rate=%d delay=%d\n", rate, delay);

    /* Handle non-negative rate values, ignore invalid (negative) values.  A
     * rate of 0 disables repeat. Note that a requested rate value larger than
     * 100 may not actually lead to the desired repeat rate, since we are
     * constrained by the USER_TIMER_MINIMUM (=10ms) resolution of win32
     * timers. */
    if (rate > 1000)
        wayland->keyboard.repeat_interval_ms = 1;
    else if (rate > 0)
        wayland->keyboard.repeat_interval_ms = 1000 / rate;
    else if (rate == 0)
        wayland->keyboard.repeat_interval_ms = 0;

    wayland->keyboard.repeat_delay_ms = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers,
    keyboard_handle_repeat_info,
};

static void wayland_keyboard_deinit(struct wayland_keyboard *keyboard)
{
    if (keyboard->wl_keyboard)
        wl_keyboard_destroy(keyboard->wl_keyboard);

    xkb_compose_state_unref(keyboard->xkb_compose_state);
    xkb_state_unref(keyboard->xkb_state);
    xkb_context_unref(keyboard->xkb_context);

    memset(keyboard, 0, sizeof(*keyboard));
}

static void wayland_keyboard_init(struct wayland_keyboard *keyboard,
                                  struct wl_seat *seat)
{
    struct xkb_compose_table *compose_table;
    const char *locale;

    locale = getenv("LC_ALL");
    if (!locale || !*locale)
        locale = getenv("LC_CTYPE");
    if (!locale || !*locale)
        locale = getenv("LANG");
    if (!locale || !*locale)
        locale = "C";

    keyboard->wl_keyboard = wl_seat_get_keyboard(seat);
    /* Some sensible default values for the repeat rate and delay. */
    keyboard->repeat_interval_ms = 40;
    keyboard->repeat_delay_ms = 400;
    keyboard->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!keyboard->xkb_context)
    {
        ERR("Failed to create XKB context\n");
        return;
    }
    compose_table =
        xkb_compose_table_new_from_locale(keyboard->xkb_context, locale,
                                          XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (!compose_table)
    {
        ERR("Failed to create XKB compose table\n");
        return;
    }

    keyboard->xkb_compose_state =
        xkb_compose_state_new(compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
    xkb_compose_table_unref(compose_table);
    if (!keyboard->xkb_compose_state)
        ERR("Failed to create XKB compose table\n");
}

/**********************************************************************
 *          Pointer handling
 */

static void pointer_handle_motion_internal(void *data, struct wl_pointer *pointer,
                                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland *wayland = data;
    HWND focused_hwnd = wayland->pointer.focused_surface ?
                        wayland->pointer.focused_surface->hwnd : 0;
    INPUT input = {0};
    int screen_x, screen_y;

    if (!focused_hwnd)
        return;

    wayland_surface_coords_to_screen(wayland->pointer.focused_surface,
                                     wl_fixed_to_double(sx),
                                     wl_fixed_to_double(sy),
                                     &screen_x, &screen_y);

    TRACE("surface=%p hwnd=%p wayland_xy=%.2f,%.2f screen_xy=%d,%d\n",
          wayland->pointer.focused_surface, focused_hwnd,
          wl_fixed_to_double(sx), wl_fixed_to_double(sy),
          screen_x, screen_y);

    input.type           = INPUT_MOUSE;
    input.mi.dx          = screen_x;
    input.mi.dy          = screen_y;
    input.mi.dwFlags     = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;

    wayland->last_dispatch_mask |= QS_MOUSEMOVE;
    wayland->last_event_type = INPUT_MOUSE;

    __wine_send_input(focused_hwnd, &input, NULL);
}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer,
                                  uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland *wayland = data;

    /* Don't handle absolute motion events if we are in relative mode. */
    if (wayland->pointer.zwp_relative_pointer_v1)
        return;

    pointer_handle_motion_internal(data, pointer, time, sx, sy);
}

static void pointer_handle_enter(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface,
                                 wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland *wayland = data;
    struct wayland_surface *wayland_surface = get_wayland_surface(wayland, surface);

    if (wayland_surface && wayland_surface->hwnd) {
        TRACE("surface=%p hwnd=%p\n", wayland_surface, wayland_surface->hwnd);
        wayland->pointer.focused_surface = wayland_surface;
        wayland->pointer.enter_serial = serial;
        /* Invalidate the set cursor cache, so that next update is
         * unconditionally applied. */
        wayland_invalidate_set_cursor();
        /* Handle the enter as a motion, to account for cases where the
         * window first appears beneath the pointer and won't get a separate
         * motion event. */
        pointer_handle_motion_internal(data, pointer, 0, sx, sy);
    }
}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface)
{
    struct wayland *wayland = data;

    if (wayland->pointer.focused_surface &&
        wayland->pointer.focused_surface->wl_surface == surface)
    {
        TRACE("surface=%p hwnd=%p\n",
              wayland->pointer.focused_surface,
              wayland->pointer.focused_surface->hwnd);
        wayland->pointer.focused_surface = NULL;
        wayland->pointer.enter_serial = 0;
    }
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t serial, uint32_t time, uint32_t button,
                                  uint32_t state)
{
    struct wayland *wayland = data;
    HWND focused_hwnd = wayland->pointer.focused_surface ?
                        wayland->pointer.focused_surface->hwnd : 0;
    INPUT input = {0};

    if (!focused_hwnd)
        return;

    TRACE("button=%#x state=%#x hwnd=%p\n", button, state, focused_hwnd);

    input.type = INPUT_MOUSE;

    switch (button)
    {
    case BTN_LEFT: input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
    case BTN_RIGHT: input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; break;
    case BTN_MIDDLE: input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN; break;
    default: break;
    }

    if (state == WL_POINTER_BUTTON_STATE_RELEASED)
        input.mi.dwFlags <<= 1;

    wayland->last_dispatch_mask |= QS_MOUSEBUTTON;
    wayland->last_event_type = INPUT_MOUSE;

    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
        wayland->last_button_serial = serial;
    else
        wayland->last_button_serial = 0;

    __wine_send_input(focused_hwnd, &input, NULL);
}

static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
                                uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static void pointer_handle_frame(void *data, struct wl_pointer *wl_pointer)
{
}

static void pointer_handle_axis_source(void *data, struct wl_pointer *wl_pointer,
                                       uint32_t axis_source)
{
}

static void pointer_handle_axis_stop(void *data, struct wl_pointer *wl_pointer,
                                     uint32_t time, uint32_t axis)
{
}

static void pointer_handle_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                                         uint32_t axis, int32_t discrete)
{
    struct wayland *wayland = data;
    HWND focused_hwnd = wayland->pointer.focused_surface ?
                        wayland->pointer.focused_surface->hwnd : 0;
    INPUT input = {0};

    if (!focused_hwnd)
        return;

    TRACE("axis=%#x discrete=%d hwnd=%p\n", axis, discrete, focused_hwnd);

    input.type = INPUT_MOUSE;

    switch (axis)
    {
    case WL_POINTER_AXIS_VERTICAL_SCROLL:
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = -WHEEL_DELTA * discrete;
        break;
    case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
        input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = WHEEL_DELTA * discrete;
        break;
    default: break;
    }

    wayland->last_dispatch_mask |= QS_MOUSEBUTTON;
    wayland->last_event_type = INPUT_MOUSE;

    __wine_send_input(focused_hwnd, &input, NULL);
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
    pointer_handle_frame,
    pointer_handle_axis_source,
    pointer_handle_axis_stop,
    pointer_handle_axis_discrete,
};

static void relative_pointer_handle_motion(void *data,
                                           struct zwp_relative_pointer_v1 *rpointer,
                                           uint32_t utime_hi,
                                           uint32_t utime_lo,
                                           wl_fixed_t dx,
                                           wl_fixed_t dy,
                                           wl_fixed_t dx_unaccel,
                                           wl_fixed_t dy_unaccel)
{
    struct wayland *wayland = data;
    HWND focused_hwnd = wayland->pointer.focused_surface ?
                        wayland->pointer.focused_surface->hwnd : 0;
    int wine_dx, wine_dy;
    INPUT input = {0};

    if (!focused_hwnd)
        return;

    wayland_surface_coords_to_wine(wayland->pointer.focused_surface,
                                   wl_fixed_to_int(dx), wl_fixed_to_int(dy),
                                   &wine_dx, &wine_dy);

    TRACE("surface=%p hwnd=%p wayland_dxdy=%d,%d wine_dxdy=%d,%d\n",
          wayland->pointer.focused_surface, focused_hwnd,
          wl_fixed_to_int(dx), wl_fixed_to_int(dy), wine_dx, wine_dy);

    input.type           = INPUT_MOUSE;
    input.mi.dx          = wine_dx;
    input.mi.dy          = wine_dy;
    input.mi.dwFlags     = MOUSEEVENTF_MOVE;

    wayland->last_dispatch_mask |= QS_MOUSEMOVE;
    wayland->last_event_type = INPUT_MOUSE;

    __wine_send_input(focused_hwnd, &input, NULL);
}

static const struct zwp_relative_pointer_v1_listener zwp_relative_pointer_v1_listener = {
    relative_pointer_handle_motion,
};

/**********************************************************************
 *          wayland_pointer_set_relative
 *
 * Set whether the pointer emits relative (if able) or absolute motion events.
 * The default is to emit absolute motion events.
 */
void wayland_pointer_set_relative(struct wayland_pointer *pointer, BOOL relative)
{
    if (!pointer->wayland->zwp_relative_pointer_manager_v1)
        return;

    if (!pointer->zwp_relative_pointer_v1 && relative)
    {
        pointer->zwp_relative_pointer_v1 =
            zwp_relative_pointer_manager_v1_get_relative_pointer(
                pointer->wayland->zwp_relative_pointer_manager_v1,
                pointer->wl_pointer);

        zwp_relative_pointer_v1_add_listener(pointer->zwp_relative_pointer_v1,
                                             &zwp_relative_pointer_v1_listener,
                                             pointer->wayland);
    }
    else if (pointer->zwp_relative_pointer_v1 && !relative)
    {
        zwp_relative_pointer_v1_destroy(pointer->zwp_relative_pointer_v1);
        pointer->zwp_relative_pointer_v1 = NULL;
    }
}

static void wayland_pointer_deinit(struct wayland_pointer *pointer)
{
    if (pointer->zwp_relative_pointer_v1)
    {
        zwp_relative_pointer_v1_destroy(pointer->zwp_relative_pointer_v1);
        pointer->zwp_relative_pointer_v1 = NULL;
    }
    if (pointer->wl_pointer)
    {
        wl_pointer_destroy(pointer->wl_pointer);
        pointer->wl_pointer = NULL;
    }
    if (pointer->cursor_wl_surface)
    {
        wl_surface_destroy(pointer->cursor_wl_surface);
        pointer->cursor_wl_surface = NULL;
    }
    if (pointer->cursor)
    {
        wayland_cursor_destroy(pointer->cursor);
        pointer->cursor = NULL;
    }

    pointer->wayland = NULL;
    pointer->focused_surface = NULL;
    pointer->enter_serial = 0;
}

/**********************************************************************
 *          Seat handling
 */

static void seat_handle_capabilities(void *data, struct wl_seat *seat,
                                     enum wl_seat_capability caps)
{
    struct wayland *wayland = data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wayland->pointer.wl_pointer)
    {
        wayland->pointer.wayland = wayland;
        wayland->pointer.wl_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wayland->pointer.wl_pointer, &pointer_listener, wayland);
        wayland->pointer.cursor_wl_surface =
            wl_compositor_create_surface(wayland->wl_compositor);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wayland->pointer.wl_pointer)
    {
        wayland_pointer_deinit(&wayland->pointer);
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wayland->keyboard.wl_keyboard)
    {
        wayland_keyboard_init(&wayland->keyboard, seat);
        wl_keyboard_add_listener(wayland->keyboard.wl_keyboard, &keyboard_listener, wayland);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && wayland->keyboard.wl_keyboard)
    {
        wayland_keyboard_deinit(&wayland->keyboard);
    }
}

static void seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
    seat_handle_name,
};

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
    else if (strcmp(interface, "wl_subcompositor") == 0)
    {
        wayland->wl_subcompositor =
            wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
    }
    else if (strcmp(interface, "xdg_wm_base") == 0)
    {
        wayland->xdg_wm_base = wl_registry_bind(registry, id,
                &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wayland->xdg_wm_base, &xdg_wm_base_listener, wayland);
    }
    else if (strcmp(interface, "wl_shm") == 0)
    {
        wayland->wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, "wl_seat") == 0)
    {
        wayland->wl_seat = wl_registry_bind(registry, id, &wl_seat_interface,
                                            version < 5 ? version : 5);
        wl_seat_add_listener(wayland->wl_seat, &seat_listener, wayland);
    }
    else if (strcmp(interface, "wp_viewporter") == 0)
    {
        wayland->wp_viewporter = wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
    }
    else if (strcmp(interface, "wl_data_device_manager") == 0)
    {
        wayland->wl_data_device_manager =
            wl_registry_bind(registry, id, &wl_data_device_manager_interface,
                             version < 3 ? version : 3);
        TRACE("manager=%p\n", wayland->wl_data_device_manager);
    }
    else if (strcmp(interface, "wl_output") == 0)
    {
        wayland_add_output(wayland, id, version);
    }
    else if (strcmp(interface, "zwp_pointer_constraints_v1") == 0)
    {
        wayland->zwp_pointer_constraints_v1 =
            wl_registry_bind(registry, id, &zwp_pointer_constraints_v1_interface, 1);
    }
    else if (strcmp(interface, "zwp_relative_pointer_manager_v1") == 0)
    {
        wayland->zwp_relative_pointer_manager_v1 =
            wl_registry_bind(registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "zxdg_output_manager_v1") == 0)
    {
        struct wayland_output *output;

        wayland->zxdg_output_manager_v1 =
            wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface,
                             version < 3 ? version : 3);

        /* Add zxdg_output_v1 to existing outputs. */
        wl_list_for_each(output, &wayland->output_list, link)
        {
            output->zxdg_output_v1 =
                zxdg_output_manager_v1_get_xdg_output(wayland->zxdg_output_manager_v1,
                                                      output->wl_output);
            zxdg_output_v1_add_listener(output->zxdg_output_v1, &zxdg_output_v1_listener,
                                        output);
        }
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
            struct wayland_surface *surface;

            TRACE("removing output->name=%s\n", output->name);

            /* Remove the output from surfaces, as some compositor don't send
             * a leave event if the output is disconnected. */
            wl_list_for_each(surface, &wayland->surface_list, link)
                wayland_surface_leave_output(surface, output);

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
    int flags;

    TRACE("wayland=%p wl_display=%p\n", wayland, process_wl_display);

    wl_list_init(&wayland->thread_link);
    wayland->event_notification_pipe[0] = -1;
    wayland->event_notification_pipe[1] = -1;

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

    if (!(wayland->buffer_wl_event_queue = wl_display_create_queue(wayland->wl_display)))
    {
        ERR("Failed to create buffer event queue\n");
        return FALSE;
    }

    if (!(wayland->wl_registry = wl_display_get_registry(wayland->wl_display)))
    {
        ERR("Failed to get to wayland registry\n");
        return FALSE;
    }
    wl_proxy_set_queue((struct wl_proxy *) wayland->wl_registry, wayland->wl_event_queue);

    /* Start with the default FNV-1a offset for 32-bits. */
    wayland->output_id_fnv_offset = 0x811c9dc5;

    wayland->hidpi_scaling = WAYLAND_HIDPI_SCALING_APPLICATION;
    wayland_read_options_from_registry(wayland);

    wl_list_init(&wayland->output_list);
    wl_list_init(&wayland->surface_list);

    /* Populate registry */
    wl_registry_add_listener(wayland->wl_registry, &registry_listener, wayland);

    /* We need three roundtrips. One to get and bind globals, one to handle all
     * initial events produced from registering the globals and one more to
     * handle third-order registrations (e.g., initial wl_keyboard
     * keymap/repeat_info events). */
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);
    wl_display_roundtrip_queue(wayland->wl_display, wayland->wl_event_queue);

    if (wayland->wl_data_device_manager && wayland->wl_seat)
        wayland_data_device_init(wayland);

    InitializeCriticalSection(&wayland->crit);
    wayland->crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": wayland");

    /* Thread wayland instances have notification pipes to inform them when
     * there might be new events in their queues. The read part of the pipe
     * is also used as the wine server queue fd. */
    if (pipe2(wayland->event_notification_pipe, O_CLOEXEC) == -1)
        return FALSE;
    /* Make just the read end non-blocking */
    if ((flags = fcntl(wayland->event_notification_pipe[0], F_GETFL)) == -1)
        return FALSE;
    if (fcntl(wayland->event_notification_pipe[0], F_SETFL, flags | O_NONBLOCK) == -1)
        return FALSE;
    /* Keep a list of all thread wayland instances, so we can notify them. */
    EnterCriticalSection(&thread_wayland_section);
    wl_list_insert(&thread_wayland_list, &wayland->thread_link);
    LeaveCriticalSection(&thread_wayland_section);

    SetRect(&wayland->cursor_clip, INT_MIN, INT_MIN, INT_MAX, INT_MAX);

    return TRUE;
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

/**********************************************************************
 *          wayland_deinit
 *
 *  Deinitialise a wayland instance, releasing all associated resources.
 */
void wayland_deinit(struct wayland *wayland)
{
    struct wayland_output *output, *output_tmp;

    TRACE("%p\n", wayland);

    EnterCriticalSection(&thread_wayland_section);
    wl_list_remove(&wayland->thread_link);
    LeaveCriticalSection(&thread_wayland_section);

    EnterCriticalSection(&wayland->crit);

    /* Destroying a surface may destroy other related surfaces too.
     * wl_list_for_each_safe doesn't handle this scenario well, so manually
     * keep destroying the first surface in the list, until we have cleared the
     * whole list.  */
    while (wayland->surface_list.next != &wayland->surface_list)
    {
        struct wayland_surface *surface =
            wl_container_of(wayland->surface_list.next, surface, link);
        wayland_surface_destroy(surface);
    }

    LeaveCriticalSection(&wayland->crit);

    wl_list_for_each_safe(output, output_tmp, &wayland->output_list, link)
        wayland_output_destroy(output);

    if (wayland->pointer.wl_pointer)
        wayland_pointer_deinit(&wayland->pointer);

    if (wayland->keyboard.wl_keyboard)
        wayland_keyboard_deinit(&wayland->keyboard);

    if (wayland->wl_data_device)
    {
        wl_data_device_destroy(wayland->wl_data_device);
        wayland->wl_data_device = NULL;
    }

    if (wayland->wl_seat)
    {
        wl_seat_destroy(wayland->wl_seat);
        wayland->wl_seat = NULL;
    }

    if (wayland->wl_shm)
    {
        wl_shm_destroy(wayland->wl_shm);
        wayland->wl_shm = NULL;
    }

    if (wayland->xdg_wm_base)
    {
        xdg_wm_base_destroy(wayland->xdg_wm_base);
        wayland->xdg_wm_base = NULL;
    }

    if (wayland->wl_subcompositor)
    {
        wl_subcompositor_destroy(wayland->wl_subcompositor);
        wayland->wl_subcompositor = NULL;
    }

    if (wayland->wl_compositor)
    {
        wl_compositor_destroy(wayland->wl_compositor);
        wayland->wl_compositor = NULL;
    }

    if (wayland->wl_registry)
    {
        wl_registry_destroy(wayland->wl_registry);
        wayland->wl_registry = NULL;
    }

    if (wayland->wl_event_queue)
    {
        wl_event_queue_destroy(wayland->wl_event_queue);
        wayland->wl_event_queue = NULL;
    }

    if (wayland->buffer_wl_event_queue)
    {
        wl_event_queue_destroy(wayland->buffer_wl_event_queue);
        wayland->buffer_wl_event_queue = NULL;
    }

    wl_display_flush(wayland->wl_display);

    wayland->wl_display = NULL;

    wayland->crit.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&wayland->crit);
}

/**********************************************************************
 *          wayland_dispatch_non_buffer
 *
 * Dispatch all non-buffer events for the specified wayland instance.
 *
 * Returns the number of events dispatched.
 */
int wayland_dispatch_non_buffer(struct wayland *wayland)
{
    char buf[64];

    TRACE("wayland=%p queue=%p\n", wayland, wayland->wl_event_queue);

    wl_display_flush(wayland->wl_display);

    /* Consume notifications */
    while (TRUE)
    {
        int ret = read(wayland->event_notification_pipe[0], buf, sizeof(buf));
        if (ret > 0) continue;
        if (ret == -1)
        {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) break; /* no data to read */
            ERR("failed to read from notification pipe: %s\n", strerror(errno));
            break;
        }
        if (ret == 0)
        {
            ERR("failed to read from notification pipe: pipe is closed\n");
            break;
        }
    }

    return wl_display_dispatch_queue_pending(wayland->wl_display,
                                             wayland->wl_event_queue);
}

/**********************************************************************
 *          wayland_dispatch_buffer
 *
 * Dispatch buffer related events for the specified wayland instance.
 *
 * Returns the number of events dispatched.
 */
int wayland_dispatch_buffer(struct wayland *wayland)
{
    TRACE("wayland=%p buffer_queue=%p\n", wayland, wayland->buffer_wl_event_queue);

    wl_display_flush(wayland->wl_display);

    return wl_display_dispatch_queue_pending(wayland->wl_display,
                                             wayland->buffer_wl_event_queue);
}

static void wayland_notify_threads(void)
{
    struct wayland *w;
    int ret;

    EnterCriticalSection(&thread_wayland_section);

    wl_list_for_each(w, &thread_wayland_list, thread_link)
    {
        while ((ret = write(w->event_notification_pipe[1], "a", 1)) != 1)
        {
            if (ret == -1 && errno != EINTR)
            {
                ERR("failed to write to notification pipe: %s\n", strerror(errno));
                break;
            }
        }
    }

    LeaveCriticalSection(&thread_wayland_section);
}

/**********************************************************************
 *          wayland_read_events
 *
 * Read wayland events from the compositor, place them in their proper
 * event queues and notify threads about the possibility of new events.
 *
 * Returns whether the operation succeeded.
 */
BOOL wayland_read_events(void)
{
    struct pollfd pfd = {0};
    int ret;

    pfd.fd = wl_display_get_fd(process_wl_display);
    pfd.events = POLLIN;

    TRACE("waiting for events...\n");

    /* In order to read events we need to prepare the read on some
     * queue. We can safely use the default queue, since it's
     * otherwise unused (all struct wayland instances dispatch to
     * their own queues). */
    while (wl_display_prepare_read(process_wl_display) != 0)
    {
        if (wl_display_dispatch_pending(process_wl_display) == -1)
        {
            TRACE("... failed wl_display_dispatch_pending errno=%d\n", errno);
            return FALSE;
        }
    }

    wl_display_flush(process_wl_display);

    while ((ret = poll(&pfd, 1, -1)) == -1 && errno == EINTR) continue;

    if (ret == -1 || !(pfd.revents & POLLIN))
    {
        TRACE("... failed poll errno=%d revents=0x%x\n",
              ret == -1 ? errno : 0, pfd.revents);
        wl_display_cancel_read(process_wl_display);
        return FALSE;
    }

    if (wl_display_read_events(process_wl_display) == -1)
    {
        TRACE("... failed wl_display_read_events errno=%d\n", errno);
        return FALSE;
    }

    if (wl_display_dispatch_pending(process_wl_display) == -1)
    {
        TRACE("... failed wl_display_dispatch_pending errno=%d\n", errno);
        return FALSE;
    }

    wayland_notify_threads();

    TRACE("... done\n");

    return TRUE;
}

/**********************************************************************
 *          wayland_change_wine_mode
 *
 * Change the current wine mode for the specified output on a particular
 * wayland instance.
 */
void wayland_change_wine_mode(struct wayland *wayland, int output_id, int width, int height)
{
    struct wayland_output *output;
    struct wayland_output_mode *output_mode;

    TRACE("wayland=%p output_id=%d width=%d height=%d\n",
          wayland, output_id, width, height);

    wl_list_for_each(output, &wayland->output_list, link)
    {
        if (output->id == output_id)
            break;
    }

    if (output->id != output_id)
        return;

    wl_list_for_each(output_mode, &output->mode_list, link)
    {
        if (output_mode->width == width && output_mode->height == height)
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

struct mode_change_info { int output_id; int width; int height; };

static BOOL CALLBACK send_wm_wayland_mode_change(HWND hwnd, LPARAM lp)
{
    struct mode_change_info *m = (struct mode_change_info *) lp;
    SendMessageW(hwnd, WM_WAYLAND_MODE_CHANGE,
                 m->output_id, MAKELPARAM(m->width, m->height));
    return FALSE;
}

/**********************************************************************
 *          wayland_notify_wine_mode_change
 *
 * Notify all wayland instances about a change in the current wine mode.
 * The notification is synchronous, this function returns after all
 * wayland instances have handled the event.
 */
void wayland_notify_wine_mode_change(int output_id, int width, int height)
{
    struct wayland *w;
    struct mode_change_info m = { output_id, width, height };

    EnterCriticalSection(&thread_wayland_section);

    /* For each thread, find a window in that thread and send the message to
     * it. We do this instead of using, e.g., PostThreadMessage, so that we
     * get synchronous handling of the message. */
    wl_list_for_each(w, &thread_wayland_list, thread_link)
        EnumThreadWindows(w->thread_id, send_wm_wayland_mode_change, (LPARAM)&m);

    LeaveCriticalSection(&thread_wayland_section);
}
