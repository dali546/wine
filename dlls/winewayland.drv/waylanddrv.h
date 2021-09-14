/*
 * Wayland core handling
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

#ifndef __WINE_WAYLANDDRV_H
#define __WINE_WAYLANDDRV_H

#ifndef __WINE_CONFIG_H
# error You must include config.h to use this header
#endif

#include <wayland-client.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"

/**********************************************************************
 *          Internal messages and data
 */
enum wayland_window_message
{
    WM_WAYLAND_CONFIGURE = 0x80001000,
    WM_WAYLAND_MODE_CHANGE = 0x80001001,
    WM_WAYLAND_POINTER_CONFINEMENT_UPDATE = 0x80001002,
    WM_WAYLAND_SURFACE_OUTPUT_CHANGE = 0x80001003,
};

enum wayland_configure_flags
{
    WAYLAND_CONFIGURE_FLAG_RESIZING   = (1 << 0),
    WAYLAND_CONFIGURE_FLAG_ACTIVATED  = (1 << 1),
    WAYLAND_CONFIGURE_FLAG_MAXIMIZED  = (1 << 2),
    WAYLAND_CONFIGURE_FLAG_FULLSCREEN = (1 << 3),
};

enum wayland_pointer_confinement
{
    WAYLAND_POINTER_CONFINEMENT_RETAIN_CLIP,
    WAYLAND_POINTER_CONFINEMENT_SYSTEM_CLIP,
    WAYLAND_POINTER_CONFINEMENT_UNSET_CLIP,
};

enum wayland_hidpi_scaling
{
    WAYLAND_HIDPI_SCALING_APPLICATION,
    WAYLAND_HIDPI_SCALING_COMPOSITOR,
};

/**********************************************************************
 *          Definitions for wayland types
 */
struct wayland_surface;
struct wayland_shm_buffer;

struct wayland_keyboard
{
    struct wl_keyboard *wl_keyboard;
    struct wayland_surface *focused_surface;
    int repeat_interval_ms;
    int repeat_delay_ms;
    uint32_t pressed_key;
    uint32_t enter_serial;
    struct xkb_context *xkb_context;
    struct xkb_state *xkb_state;
    struct xkb_compose_state *xkb_compose_state;
    UINT xkb_keycode_to_vkey[256];
    WORD xkb_keycode_to_scancode[256];
};

struct wayland_cursor
{
    struct wayland_shm_buffer *shm_buffer;
    int hotspot_x;
    int hotspot_y;
};

struct wayland_pointer
{
    struct wayland *wayland;
    struct wl_pointer *wl_pointer;
    struct wayland_surface *focused_surface;
    struct wl_surface *cursor_wl_surface;
    uint32_t enter_serial;
    struct wayland_cursor *cursor;
    HCURSOR hcursor;
    struct zwp_relative_pointer_v1 *zwp_relative_pointer_v1;
};

struct wayland
{
    DWORD thread_id;
    struct wl_display *wl_display;
    struct wl_event_queue *wl_event_queue;
    struct wl_event_queue *buffer_wl_event_queue;
    struct wl_registry *wl_registry;
    struct wl_compositor *wl_compositor;
    struct wl_subcompositor *wl_subcompositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_shm *wl_shm;
    struct wl_seat *wl_seat;
    struct wp_viewporter *wp_viewporter;
    struct wl_data_device_manager *wl_data_device_manager;
    struct wl_data_device *wl_data_device;
    struct zwp_pointer_constraints_v1 *zwp_pointer_constraints_v1;
    struct zwp_relative_pointer_manager_v1 *zwp_relative_pointer_manager_v1;
    struct zxdg_output_manager_v1 *zxdg_output_manager_v1;
    uint32_t output_id_fnv_offset;
    uint32_t next_fallback_output_id;
    struct wl_list output_list;
    struct wl_list surface_list;
    struct wayland_keyboard keyboard;
    struct wayland_pointer pointer;
    DWORD last_dispatch_mask;
    uint32_t last_button_serial;
    DWORD last_event_type;
    int event_notification_pipe[2];
    struct wl_list thread_link;
    HWND clipboard_hwnd;
    RECT cursor_clip;
    enum wayland_hidpi_scaling hidpi_scaling;
    CRITICAL_SECTION crit;
};

struct wayland_output_mode
{
    struct wl_list link;
    int32_t width;
    int32_t height;
    int32_t refresh;
    BOOL native;
};

struct wayland_output
{
    struct wl_list link;
    struct wayland *wayland;
    struct wl_output *wl_output;
    struct zxdg_output_v1 *zxdg_output_v1;
    struct wl_list mode_list;
    struct wayland_output_mode *current_mode;
    struct wayland_output_mode *current_wine_mode;
    int logical_x, logical_y;  /* logical position */
    int logical_w, logical_h;  /* logical size */
    int x, y;  /* position in native pixel coordinate space */
    int scale; /* wayland output scale factor for hidpi */
    /* Scale factor by which we need to multiply values in the wine coordinate
     * space to get values in the wayland coordinate space for this output. Used
     * when emulating a display mode change. */
    double wine_scale;
    char *name;
    WCHAR wine_name[128];
    uint32_t id;
    uint32_t global_id;
};

struct wayland_buffer_queue
{
    struct wayland *wayland;
    struct wl_list buffer_list;
    int width;
    int height;
    enum wl_shm_format format;
    HRGN damage_region;
};

struct wayland_surface_configure
{
    int width;
    int height;
    enum wayland_configure_flags configure_flags;
    uint32_t serial;
};

struct wayland_output_ref
{
    struct wl_list link;
    struct wayland_output *output;
};

struct wayland_surface
{
    struct wl_list link;
    struct wayland *wayland;
    struct wl_surface *wl_surface;
    struct wl_subsurface *wl_subsurface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wp_viewport *wp_viewport;
    struct wl_egl_window *wl_egl_window;
    struct wayland_surface *parent;
    struct wayland_surface *glvk;
    struct zwp_confined_pointer_v1 *zwp_confined_pointer_v1;
    struct zwp_locked_pointer_v1 *zwp_locked_pointer_v1;
    /* The offset of this surface relative to its owning win32 window */
    int offset_x, offset_y;
    HWND hwnd;
    CRITICAL_SECTION crit;
    struct wayland_surface_configure pending;
    struct wayland_surface_configure current;
    BOOL mapped;
    LONG ref;
    struct wl_list output_ref_list;
    struct wayland_output *main_output;
};

struct wayland_shm_buffer
{
    struct wl_list link;
    struct wayland_surface *wayland_surface;
    struct wl_buffer *wl_buffer;
    int width, height, stride;
    enum wl_shm_format format;
    void *map_data;
    size_t map_size;
    BOOL busy;
    HRGN damage_region;
};

extern struct wl_display *process_wl_display;

/**********************************************************************
 *          Wayland thread data
 */

struct wayland_thread_data
{
    struct wayland wayland;
};

extern struct wayland_thread_data *wayland_init_thread_data(void) DECLSPEC_HIDDEN;
extern DWORD thread_data_tls_index DECLSPEC_HIDDEN;

static inline struct wayland_thread_data *wayland_thread_data(void)
{
    DWORD err = GetLastError();  /* TlsGetValue always resets last error */
    struct wayland_thread_data *data = TlsGetValue( thread_data_tls_index );
    SetLastError( err );
    return data;
}

static inline struct wayland *thread_init_wayland(void)
{
    return &wayland_init_thread_data()->wayland;
}

static inline struct wayland *thread_wayland(void)
{
    struct wayland_thread_data *data = wayland_thread_data();
    if (!data) return NULL;
    return &data->wayland;
}

/**********************************************************************
 *          Wayland initialisation
 */

BOOL wayland_process_init(void);
BOOL wayland_init(struct wayland *wayland);
void wayland_deinit(struct wayland *wayland);
void wayland_init_display_devices(struct wayland *wayland);
void wayland_read_options_from_registry(struct wayland *wayland);

/**********************************************************************
 *          Wayland event dispatch
 */

int wayland_dispatch_non_buffer(struct wayland *wayland);
int wayland_dispatch_buffer(struct wayland *wayland);
BOOL wayland_read_events(void);

/**********************************************************************
 *          Wayland mode change
 */
void wayland_change_wine_mode(struct wayland *wayland, int output_id, int width, int height);
void wayland_notify_wine_mode_change(int output_id, int width, int height);
struct wayland_output *wayland_get_output_by_wine_name(struct wayland *wayland, LPCWSTR name);
void wayland_output_destroy(struct wayland_output *output);

/**********************************************************************
 *          Wayland buffer queue
 */

struct wayland_buffer_queue *wayland_buffer_queue_create(struct wayland *wayland,
                                                         int width, int heigh,
                                                         enum wl_shm_format format);

void wayland_buffer_queue_destroy(struct wayland_buffer_queue *queue);
void wayland_buffer_queue_add_damage(struct wayland_buffer_queue *queue, HRGN damage);
struct wayland_shm_buffer *wayland_buffer_queue_acquire_buffer(struct wayland_buffer_queue *queue);

/**********************************************************************
 *          Wayland surface
 */

struct wayland_surface *wayland_surface_create_toplevel(struct wayland *wayland,
                                                        struct wayland_surface *parent);
struct wayland_surface *wayland_surface_create_subsurface(struct wayland *wayland,
                                                          struct wayland_surface *parent);
void wayland_surface_destroy(struct wayland_surface *surface);
void wayland_surface_reconfigure(struct wayland_surface *surface, int x, int y,
                                 int width,int height);
void wayland_surface_commit_buffer(struct wayland_surface *surface,
                                   struct wayland_shm_buffer *shm_buffer,
                                   HRGN surface_damage_region);
BOOL wayland_surface_create_or_ref_gl(struct wayland_surface *surface);
BOOL wayland_surface_create_or_ref_vk(struct wayland_surface *surface);
void wayland_surface_unref_glvk(struct wayland_surface *surface);
void wayland_surface_reconfigure_glvk(struct wayland_surface *surface, int x, int y,
                                      int width, int height);
void wayland_surface_ack_configure(struct wayland_surface *surface);
void wayland_surface_unmap(struct wayland_surface *surface);
BOOL wayland_surface_configure_is_compatible(struct wayland_surface_configure *conf,
                                             int width, int height,
                                             enum wayland_configure_flags flags);
struct wayland_surface *wayland_surface_for_hwnd(HWND hwnd);
void wayland_surface_coords_to_screen(struct wayland_surface *surface,
                                      double wayland_x, double wayland_y,
                                      int *screen_x, int *screen_y);
void wayland_surface_coords_from_screen(struct wayland_surface *surface,
                                        int screen_x, int screen_y,
                                        double *wayland_x, double *wayland_y);
void wayland_surface_coords_from_wine(struct wayland_surface *surface,
                                      int wine_x, int wine_y,
                                      double *wayland_x, double *wayland_y);
void wayland_surface_coords_rounded_from_wine(struct wayland_surface *surface,
                                              int wine_x, int wine_y,
                                              int *wayland_x, int *wayland_y);
void wayland_surface_coords_to_wine(struct wayland_surface *surface,
                                    double wayland_x, double wayland_y,
                                    int *wine_x, int *wine_y);
void wayland_surface_find_wine_fullscreen_fit(struct wayland_surface *surface,
                                              int wayland_width, int wayland_height,
                                              int *wine_width, int *wine_height);
void wayland_surface_ensure_mapped(struct wayland_surface *surface);
struct wayland_surface *wayland_surface_ref(struct wayland_surface *surface);
void wayland_surface_unref(struct wayland_surface *surface);
void wayland_surface_update_pointer_confinement(struct wayland_surface *surface);
void wayland_surface_set_main_output(struct wayland_surface *surface,
                                     struct wayland_output *output);
void wayland_surface_leave_output(struct wayland_surface *surface,
                                  struct wayland_output *output);
int wayland_surface_get_buffer_scale(struct wayland_surface *surface);

/**********************************************************************
 *          Wayland SHM buffer
 */

struct wayland_shm_buffer *wayland_shm_buffer_create(struct wayland *wayland,
                                                     int width, int height,
                                                     enum wl_shm_format format);
void wayland_shm_buffer_resize(struct wayland_shm_buffer *shm_buffer, int width, int height);
void wayland_shm_buffer_destroy(struct wayland_shm_buffer *shm_buffer);
void wayland_shm_buffer_clear_damage(struct wayland_shm_buffer *shm_buffer);
void wayland_shm_buffer_add_damage(struct wayland_shm_buffer *shm_buffer, HRGN damage);
RGNDATA *wayland_shm_buffer_get_damage_clipped(struct wayland_shm_buffer *shm_buffer,
                                               HRGN clip);

/**********************************************************************
 *          Wayland data device
 */

void wayland_data_device_init(struct wayland *wayland);
HWND wayland_data_device_create_clipboard_window(void);

/**********************************************************************
 *          Keyboard helpers
 */

void wayland_keyboard_emit(struct wayland_keyboard *keyboard, uint32_t key,
                           uint32_t state, HWND hwnd);
void wayland_keyboard_update_layout(struct wayland_keyboard *keyboard);

/**********************************************************************
 *          Pointer/Cursor helpers
 */

BOOL wayland_init_set_cursor(void);
void wayland_invalidate_set_cursor(void);
void wayland_pointer_update_cursor_from_win32(struct wayland_pointer *pointer, HCURSOR handle);
void wayland_cursor_destroy(struct wayland_cursor *wayland_cursor);
void wayland_pointer_set_relative(struct wayland_pointer *pointer, BOOL relative);

/**********************************************************************
 *          OpenGL support
 */

struct opengl_funcs *wayland_get_wgl_driver(UINT version);
void wayland_update_gl_drawable(HWND hwnd, struct wayland_surface *wayland_surface);
void wayland_destroy_gl_drawable(HWND hwnd);

/**********************************************************************
 *          Vulkan support
 */

const struct vulkan_funcs *wayland_get_vulkan_driver(UINT version);
void wayland_invalidate_vulkan_objects(HWND hwnd);

/**********************************************************************
 *          Debugging helpers
 */

void dump_pixels(const char *fpattern, int dbgid, unsigned int *pixels, int width, int height,
                 BOOL alpha, HRGN damage, HRGN win_region);

/**********************************************************************
 *          XKB helpers
 */
xkb_layout_index_t _xkb_state_get_active_layout(struct xkb_state *xkb_state);
int _xkb_keysyms_to_utf8(const xkb_keysym_t *syms, int nsyms, char *utf8, int utf8_size);

#endif /* __WINE_WAYLANDDRV_H */
