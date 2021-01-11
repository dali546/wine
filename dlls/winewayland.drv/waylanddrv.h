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

#include <wayland-client.h>
#include <wayland-egl.h>
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"

/**********************************************************************
 *          Internal messages and data
 */
enum wayland_window_message
{
    WM_WAYLAND_CONFIGURE = 0x80001000,
};

enum wayland_configure_flags
{
    WAYLAND_CONFIGURE_FLAG_RESIZING   = (1 << 0),
    WAYLAND_CONFIGURE_FLAG_ACTIVATED  = (1 << 1),
    WAYLAND_CONFIGURE_FLAG_MAXIMIZED  = (1 << 2),
    WAYLAND_CONFIGURE_FLAG_FULLSCREEN = (1 << 3),
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
};

struct wayland
{
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
    CRITICAL_SECTION crit;
};

struct wayland_output_mode
{
    struct wl_list link;
    int32_t width;
    int32_t height;
    int32_t refresh;
    BOOL current;
};

struct wayland_output
{
    struct wl_list link;
    struct wl_output *wl_output;
    struct wl_list mode_list;
    WCHAR name[128];
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
    struct wayland_surface *gl;
    /* The offset of this surface relative to its owning win32 window */
    int offset_x, offset_y;
    HWND hwnd;
    CRITICAL_SECTION crit;
    struct wayland_surface_configure pending;
    struct wayland_surface_configure current;
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
    RGNDATA *damage_region_data;
};

extern struct wayland process_wayland;

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

BOOL wayland_init(struct wayland *wayland, const struct wayland *process_wayland);
void wayland_deinit(struct wayland *wayland);
void wayland_init_display_devices(struct wayland *wayland);

/**********************************************************************
 *          Wayland event dispatch
 */

int wayland_dispatch_non_buffer(struct wayland *wayland);
int wayland_dispatch_buffer(struct wayland *wayland);
BOOL wayland_read_events(void);

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
BOOL wayland_surface_create_gl(struct wayland_surface *surface);
void wayland_surface_destroy_gl(struct wayland_surface *surface);
void wayland_surface_reconfigure_gl(struct wayland_surface *surface, int x, int y,
                                    int width, int height);
void wayland_surface_ack_configure(struct wayland_surface *surface);
void wayland_surface_unmap(struct wayland_surface *surface);
BOOL wayland_surface_configure_is_compatible(struct wayland_surface_configure *conf,
                                             int width, int height,
                                             enum wayland_configure_flags flags);
struct wayland_surface *wayland_surface_for_hwnd(HWND hwnd);

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
RGNDATA *wayland_shm_buffer_get_damage(struct wayland_shm_buffer *shm_buffer);

/**********************************************************************
 *          Wayland data device
 */

void wayland_data_device_init(struct wayland *wayland);
HWND wayland_data_device_create_clipboard_window(void);

/**********************************************************************
 *          Keyboard helpers
 */

void wayland_keyboard_emit(uint32_t key, uint32_t state, HWND hwnd);

/**********************************************************************
 *          Cursor helpers
 */

void wayland_pointer_update_cursor_from_win32(struct wayland_pointer *pointer, HCURSOR handle);
void wayland_cursor_destroy(struct wayland_cursor *wayland_cursor);

/**********************************************************************
 *          OpenGL support
 */

struct opengl_funcs *wayland_get_wgl_driver(UINT version);
void wayland_destroy_gl_drawable(HWND hwnd);

/**********************************************************************
 *          Debugging helpers
 */

void dump_pixels(const char *fpattern, int dbgid, unsigned int *pixels, int width, int height,
                 BOOL alpha, HRGN damage, HRGN win_region);
