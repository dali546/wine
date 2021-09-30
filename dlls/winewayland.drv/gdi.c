/*
 * Wayland gdi functions
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

#include "config.h"

#include "waylanddrv.h"

#include "wine/heap.h"

typedef struct
{
    struct gdi_physdev dev;
} WAYLAND_PDEVICE;

static inline WAYLAND_PDEVICE *get_wayland_dev(PHYSDEV dev)
{
    return (WAYLAND_PDEVICE *)dev;
}

static WAYLAND_PDEVICE *create_wayland_physdev(void)
{
    WAYLAND_PDEVICE *physDev;

    physDev = heap_alloc_zero(sizeof(*physDev));

    return physDev;
}

/**********************************************************************
 *           WAYLAND_CreateDC
 */
BOOL CDECL WAYLAND_CreateDC(PHYSDEV *pdev, LPCWSTR device,
                            LPCWSTR output, const DEVMODEW* initData)
{
    WAYLAND_PDEVICE *physDev = create_wayland_physdev();

    if (!physDev) return FALSE;

    push_dc_driver(pdev, &physDev->dev, &waylanddrv_funcs.dc_funcs);

    return TRUE;
}

/**********************************************************************
 *           WAYLAND_CreateCompatibleDC
 */
BOOL CDECL WAYLAND_CreateCompatibleDC(PHYSDEV orig, PHYSDEV *pdev)
{
    WAYLAND_PDEVICE *physDev = create_wayland_physdev();

    if (!physDev) return FALSE;

    push_dc_driver(pdev, &physDev->dev, &waylanddrv_funcs.dc_funcs);

    return TRUE;
}

/**********************************************************************
 *           WAYLAND_DeleteDC
 */
BOOL CDECL WAYLAND_DeleteDC(PHYSDEV dev)
{
    WAYLAND_PDEVICE *physDev = get_wayland_dev(dev);

    HeapFree(GetProcessHeap(), 0, physDev);
    return TRUE;
}

/**********************************************************************
 *           WAYLAND_wine_get_wgl_driver
 */
struct opengl_funcs * CDECL WAYLAND_wine_get_wgl_driver(PHYSDEV dev, UINT version)
{
    struct opengl_funcs *ret;

    if (!(ret = wayland_get_wgl_driver(version)))
    {
        dev = GET_NEXT_PHYSDEV(dev, wine_get_wgl_driver);
        ret = dev->funcs->wine_get_wgl_driver(dev, version);
    }

    return ret;
}
