/*
 * WAYLAND display device functions
 *
 * Copyright 2019 Zhiyi Zhang for CodeWeavers
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

#define NONAMELESSUNION
#define NONAMELESSSTRUCT

#include "config.h"

#include "waylanddrv.h"

#include "winreg.h"
#include "wine/debug.h"
#include "wine/unicode.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static BOOL force_display_devices_refresh;

static void wayland_refresh_display_devices(void)
{
    UINT32 num_path, num_mode;
    force_display_devices_refresh = TRUE;
    /* trigger refresh in win32u */
    NtUserGetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_path, &num_mode);
}

void wayland_init_display_devices()
{
    wayland_refresh_display_devices();
}

static void wayland_add_gpu(const struct gdi_device_manager *device_manager,
                            void *param)
{
    static const WCHAR wayland_gpuW[] = {'W','a','y','l','a','n','d','G','P','U',0};
    struct gdi_gpu gpu = {0};
    lstrcpyW(gpu.name, wayland_gpuW);

    /* TODO: Fill in gpu information from vulkan. */

    TRACE("id=0x%s name=%s\n",
          wine_dbgstr_longlong(gpu.id), wine_dbgstr_w(gpu.name));

    device_manager->add_gpu(&gpu, param);
}

static void wayland_add_adapter(const struct gdi_device_manager *device_manager,
                                void *param, INT output_id)
{
    struct gdi_adapter adapter;
    adapter.id = output_id;
    adapter.state_flags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP;
    if (output_id == 0)
        adapter.state_flags |= DISPLAY_DEVICE_PRIMARY_DEVICE;

    TRACE("id=0x%s state_flags=0x%x\n",
          wine_dbgstr_longlong(adapter.id), adapter.state_flags);

    device_manager->add_adapter(&adapter, param);
}

static void wayland_add_monitor(const struct gdi_device_manager *device_manager,
                                void *param, struct wayland_output *output)
{

    struct gdi_monitor monitor = {0};

    if (!MultiByteToWideChar(CP_UTF8, 0, output->name, -1, monitor.name,
                             ARRAY_SIZE(monitor.name)))
    {
        monitor.name[0] = 0;
    }

    SetRect(&monitor.rc_monitor, output->x, output->y,
            output->x + output->current_mode->width,
            output->y + output->current_mode->height);

    /* We don't have a direct way to get the work area in Wayland. */
    monitor.rc_work = monitor.rc_monitor;

    monitor.state_flags = DISPLAY_DEVICE_ATTACHED | DISPLAY_DEVICE_ACTIVE;

    TRACE("name=%s rc_monitor=rc_work=%s state_flags=0x%x\n",
          wine_dbgstr_w(monitor.name), wine_dbgstr_rect(&monitor.rc_monitor),
          monitor.state_flags);

    device_manager->add_monitor(&monitor, param);
}

/***********************************************************************
 *      UpdateDisplayDevices (WAYLAND.@)
 */
void CDECL WAYLAND_UpdateDisplayDevices(const struct gdi_device_manager *device_manager,
                                        BOOL force, void *param)
{
    struct wayland *wayland;
    struct wayland_output *output;
    INT output_id = 0;

    if (!force && !force_display_devices_refresh) return;

    TRACE("force=%d force_refresh=%d\n", force, force_display_devices_refresh);

    force_display_devices_refresh = FALSE;

    wayland = wayland_process_acquire();

    wayland_add_gpu(device_manager, param);

    wl_list_for_each(output, &wayland->output_list, link)
    {
        if (!output->current_mode) continue;

        /* TODO: Detect and support multiple monitors per adapter (i.e., mirroring). */
        wayland_add_adapter(device_manager, param, output_id);
        wayland_add_monitor(device_manager, param, output);

        output_id++;
    }

    wayland_process_release();
}
