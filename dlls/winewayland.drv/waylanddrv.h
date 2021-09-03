/*
 * Wayland driver
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

#include <stdarg.h>
#include <wayland-client.h>
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"

#include "wine/gdi_driver.h"

extern struct wl_display *process_wl_display;

/**********************************************************************
  *          Internal messages and data
  */

enum wayland_window_message
{
    WM_WAYLAND_BROADCAST_DISPLAY_CHANGE = 0x80001000,
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

struct wayland
{
    BOOL initialized;
    DWORD process_id;
    DWORD thread_id;
    struct wl_display *wl_display;
    struct wl_event_queue *wl_event_queue;
    struct wl_event_queue *buffer_wl_event_queue;
    struct wl_registry *wl_registry;
    struct wl_compositor *wl_compositor;
    struct wl_subcompositor *wl_subcompositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_shm *wl_shm;
    struct zxdg_output_manager_v1 *zxdg_output_manager_v1;
    uint32_t next_fallback_output_id;
    struct wl_list output_list;
    int event_notification_pipe[2];
    struct wl_list thread_link;
};

struct wayland_output_mode
{
    struct wl_list link;
    int32_t width;
    int32_t height;
    int32_t refresh;
    int bpp;
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
    int logical_x, logical_y;  /* logical position */
    int logical_w, logical_h;  /* logical size */
    int x, y;  /* position in native pixel coordinate space */
    int scale; /* wayland output scale factor for hidpi */
    char *name;
    WCHAR wine_name[128];
    uint32_t global_id;
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
    struct wayland *wayland;
    struct wl_surface *wl_surface;
    struct wl_subsurface *wl_subsurface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wayland_surface *parent;
    HWND hwnd;
    CRITICAL_SECTION crit;
    struct wayland_surface_configure pending;
    struct wayland_surface_configure current;
    BOOL mapped;
    LONG ref;
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

struct wayland_shm_buffer
{
    struct wl_list link;
    struct wl_buffer *wl_buffer;
    int width, height, stride;
    enum wl_shm_format format;
    void *map_data;
    size_t map_size;
    BOOL busy;
    HRGN damage_region;
};

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
    struct wayland_thread_data *data = TlsGetValue(thread_data_tls_index);
    SetLastError(err);
    return data;
}

static inline struct wayland *thread_init_wayland(void)
{
    return &wayland_init_thread_data()->wayland;
}

/**********************************************************************
 *          Wayland initialisation
 */

BOOL wayland_process_init(void);
BOOL wayland_init(struct wayland *wayland);
void wayland_deinit(struct wayland *wayland);
BOOL wayland_is_process(struct wayland *wayland);
struct wayland *wayland_process_acquire(void);
void wayland_process_release(void);
void wayland_init_display_devices(void);

/**********************************************************************
 *          Wayland output
 */

BOOL wayland_output_create(struct wayland *wayland, uint32_t id, uint32_t version);
void wayland_output_destroy(struct wayland_output *output);
void wayland_output_use_xdg_extension(struct wayland_output *output);
void wayland_update_outputs_from_process(struct wayland *wayland);
struct wayland_output *wayland_output_get_by_wine_name(struct wayland *wayland,
                                                       LPCWSTR wine_name);

/**********************************************************************
 *          Wayland event dispatch
 */

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

struct wayland_surface *wayland_surface_create_plain(struct wayland *wayland);
struct wayland_surface *wayland_surface_create_toplevel(struct wayland *wayland,
                                                        struct wayland_surface *parent);
struct wayland_surface *wayland_surface_create_subsurface(struct wayland *wayland,
                                                          struct wayland_surface *parent);
BOOL wayland_surface_configure_is_compatible(struct wayland_surface_configure *conf,
                                             int width, int height,
                                             enum wayland_configure_flags flags);
BOOL wayland_surface_commit_buffer(struct wayland_surface *surface,
                                   struct wayland_shm_buffer *shm_buffer,
                                   HRGN surface_damage_region);
void wayland_surface_destroy(struct wayland_surface *surface);
void wayland_surface_unmap(struct wayland_surface *surface);
void wayland_surface_ack_pending_configure(struct wayland_surface *surface);
void wayland_surface_coords_from_wine(struct wayland_surface *surface,
                                      int wine_x, int wine_y,
                                      double *wayland_x, double *wayland_y);
void wayland_surface_coords_rounded_from_wine(struct wayland_surface *surface,
                                              int wine_x, int wine_y,
                                              int *wayland_x, int *wayland_y);
struct wayland_surface *wayland_surface_ref(struct wayland_surface *surface);
void wayland_surface_unref(struct wayland_surface *surface);

/**********************************************************************
 *          Wayland SHM buffer
 */

struct wayland_shm_buffer *wayland_shm_buffer_create(struct wayland *wayland,
                                                     int width, int height,
                                                     enum wl_shm_format format);
void wayland_shm_buffer_destroy(struct wayland_shm_buffer *shm_buffer);
void wayland_shm_buffer_clear_damage(struct wayland_shm_buffer *shm_buffer);
void wayland_shm_buffer_add_damage(struct wayland_shm_buffer *shm_buffer, HRGN damage);
RGNDATA *wayland_shm_buffer_get_damage_clipped(struct wayland_shm_buffer *shm_buffer,
                                               HRGN clip);

/**********************************************************************
 *          Wayland window surface
 */

struct window_surface *wayland_window_surface_create(HWND hwnd, const RECT *rect);
void CDECL wayland_window_surface_flush(struct window_surface *window_surface);
BOOL wayland_window_surface_needs_flush(struct window_surface *surface);
void wayland_window_surface_update_wayland_surface(struct window_surface *surface,
                                                   struct wayland_surface *wayland_surface);

/**********************************************************************
 *          USER driver functions
 */

extern BOOL CDECL WAYLAND_CreateWindow(HWND hwnd) DECLSPEC_HIDDEN;
extern void CDECL WAYLAND_DestroyWindow(HWND hwnd) DECLSPEC_HIDDEN;
extern BOOL CDECL WAYLAND_EnumDisplaySettingsEx(LPCWSTR name, DWORD n, LPDEVMODEW devmode, DWORD flags) DECLSPEC_HIDDEN;
extern LRESULT CDECL WAYLAND_WindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) DECLSPEC_HIDDEN;
extern void CDECL WAYLAND_WindowPosChanged(HWND hwnd, HWND insert_after, UINT swp_flags,
                                           const RECT *window_rect, const RECT *client_rect,
                                           const RECT *visible_rect, const RECT *valid_rects,
                                           struct window_surface *surface) DECLSPEC_HIDDEN;
extern BOOL CDECL WAYLAND_WindowPosChanging(HWND hwnd, HWND insert_after, UINT swp_flags,
                                            const RECT *window_rect, const RECT *client_rect,
                                            RECT *visible_rect, struct window_surface **surface) DECLSPEC_HIDDEN;

#endif /* __WINE_WAYLANDDRV_H */
