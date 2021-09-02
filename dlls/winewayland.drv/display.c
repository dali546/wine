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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "waylanddrv.h"

#include "wine/debug.h"

#include "ntuser.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static BOOL force_display_devices_refresh;

static void wayland_refresh_display_devices(void)
{
    UINT32 num_path, num_mode;
    force_display_devices_refresh = TRUE;
    /* trigger refresh in win32u */
    NtUserGetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_path, &num_mode);
}

static void wayland_broadcast_wm_display_change(void)
{
    BOOL wayland_initialized = wayland_process_acquire()->initialized;
    wayland_process_release();

    /* During process wayland initialization we will get our initial output
     * information and init the display devices. There is no need to send out
     * WM_DISPLAYCHANGE in this case, since this is the initial display state.
     * Additionally, initialization may occur in a context that has acquired
     * the internal Wine user32 lock, and sending messages would lead
     * to an internal user32 lock error. */
    if (wayland_initialized)
        send_message(NtUserGetDesktopWindow(), WM_WAYLAND_BROADCAST_DISPLAY_CHANGE, 0, 0);
}

void wayland_init_display_devices()
{
    wayland_refresh_display_devices();
    wayland_notify_wine_monitor_change();
    wayland_broadcast_wm_display_change();
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
    DWORD len = 0;

    if (RtlUTF8ToUnicodeN(monitor.name, sizeof(monitor.name), &len,
                          output->name, strlen(output->name) + 1))
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

static void populate_devmode(struct wayland_output_mode *output_mode, DEVMODEW *mode)
{
    mode->dmFields = DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                     DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
    mode->dmDisplayOrientation = DMDO_DEFAULT;
    mode->dmDisplayFlags = 0;
    mode->dmBitsPerPel = output_mode->bpp;
    mode->dmPelsWidth = output_mode->width;
    mode->dmPelsHeight = output_mode->height;
    mode->dmDisplayFrequency = output_mode->refresh / 1000;
}

static void wayland_add_modes(const struct gdi_device_manager *device_manager,
                              void *param, struct wayland_output *output)
{

    struct wayland_output_mode *output_mode;

    wl_list_for_each(output_mode, &output->mode_list, link)
    {
        DEVMODEW mode;
        populate_devmode(output_mode, &mode);
        device_manager->add_mode(&mode, param);
    }
}

/***********************************************************************
 *      UpdateDisplayDevices (WAYLAND.@)
 */
BOOL WAYLAND_UpdateDisplayDevices(const struct gdi_device_manager *device_manager,
                                  BOOL force, void *param)
{
    struct wayland *wayland;
    struct wayland_output *output;
    INT output_id = 0;

    if (!force && !force_display_devices_refresh) return TRUE;

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
        wayland_add_modes(device_manager, param, output);

        output_id++;
    }

    /* Set wine name in wayland_output so that we can look it up. */
    output_id = 0;
    wl_list_for_each(output, &wayland->output_list, link)
    {
        char buf[16];
        int len;
        len = snprintf(buf, sizeof(buf), "\\\\.\\DISPLAY%d", output_id + 1);
        if (len > sizeof(buf)) len = sizeof(buf);
        ascii_to_unicode_z(output->wine_name, ARRAY_SIZE(output->wine_name), buf, len);
        TRACE("name=%s wine_name=%s\n",
              output->name, wine_dbgstr_w(output->wine_name));
        output_id++;
    }

    wayland_process_release();

    return TRUE;
}

static BOOL wayland_get_current_devmode(struct wayland *wayland, LPCWSTR name, DEVMODEW *mode)
{
    struct wayland_output *output;

    output = wayland_output_get_by_wine_name(wayland, name);
    if (!output)
        return FALSE;

    if (!output->current_mode)
        return FALSE;

    populate_devmode(output->current_mode, mode);

    mode->dmFields |= DM_POSITION;
    mode->dmPosition.x = output->x;
    mode->dmPosition.y = output->y;

    return TRUE;
}

static BOOL wayland_get_devmode(struct wayland *wayland, LPCWSTR name, DWORD n, DEVMODEW *mode)
{
    struct wayland_output *output;
    struct wayland_output_mode *output_mode;
    DWORD i = 0;

    output = wayland_output_get_by_wine_name(wayland, name);
    if (!output)
        return FALSE;

    wl_list_for_each(output_mode, &output->mode_list, link)
    {
        if (i == n)
        {
            populate_devmode(output_mode, mode);
            return TRUE;
        }
        i++;
    }

    return FALSE;
}

/***********************************************************************
 *		EnumDisplaySettingsEx  (WAYLAND.@)
 *
 */
BOOL WAYLAND_EnumDisplaySettingsEx(LPCWSTR name, DWORD n, LPDEVMODEW devmode, DWORD flags)
{
    struct wayland *wayland = wayland_process_acquire();
    BOOL ret;

    TRACE("(%s,%d,%p,0x%08x) wayland=%p\n", debugstr_w(name), n, devmode, flags, wayland);

    /* We don't handle n == ENUM_REGISTRY_SETTINGS here, it is handled by win32u. */

    /* We don't handle n == ENUM_CURRENT_SETTINGS here, it is handled by win32u through
     * WAYLAND_GetCurrentDisplaySettings. */

    ret = wayland_get_devmode(wayland, name, n, devmode);

    wayland_process_release();

    if (ret)
    {
        TRACE("=> %dx%d\n", devmode->dmPelsWidth, devmode->dmPelsHeight);
    }
    else
    {
        WARN("Modes index out of range\n");
        RtlSetLastWin32Error(ERROR_NO_MORE_FILES);
    }

    return ret;
}

/***********************************************************************
 *             GetCurrentDisplaySettings  (WAYLAND.@)
 *
 */
BOOL WAYLAND_GetCurrentDisplaySettings(LPCWSTR name, LPDEVMODEW devmode)
{
    struct wayland *wayland = wayland_process_acquire();
    BOOL ret;

    TRACE("(%s,%p) wayland=%p\n", debugstr_w(name), devmode, wayland);

    ret = wayland_get_current_devmode(wayland, name, devmode);

    wayland_process_release();

    if (ret)
    {
        TRACE("=> %d,%d+%dx%d\n", devmode->dmPosition.x, devmode->dmPosition.y,
              devmode->dmPelsWidth, devmode->dmPelsHeight);
    }
    else
    {
        ERR("Failed to get %s current display settings.\n", wine_dbgstr_w(name));
    }

    return ret;
}
