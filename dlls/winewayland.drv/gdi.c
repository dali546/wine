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

#include "wine/gdi_driver.h"
#include "wine/debug.h"
#include "wine/heap.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static const struct gdi_dc_funcs wayland_gdi_dc_funcs;

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
static BOOL CDECL WAYLAND_CreateDC(PHYSDEV *pdev, LPCWSTR driver, LPCWSTR device,
                                       LPCWSTR output, const DEVMODEW* initData)
{
    WAYLAND_PDEVICE *physDev = create_wayland_physdev();

    if (!physDev) return FALSE;

    push_dc_driver(pdev, &physDev->dev, &wayland_gdi_dc_funcs);

    return TRUE;
}

/**********************************************************************
 *           WAYLAND_CreateCompatibleDC
 */
static BOOL CDECL WAYLAND_CreateCompatibleDC(PHYSDEV orig, PHYSDEV *pdev)
{
    WAYLAND_PDEVICE *physDev = create_wayland_physdev();

    if (!physDev) return FALSE;

    push_dc_driver(pdev, &physDev->dev, &wayland_gdi_dc_funcs);

    return TRUE;
}

/**********************************************************************
 *           WAYLAND_DeleteDC
 */
static BOOL CDECL WAYLAND_DeleteDC(PHYSDEV dev)
{
    WAYLAND_PDEVICE *physDev = get_wayland_dev(dev);

    HeapFree(GetProcessHeap(), 0, physDev);
    return TRUE;
}

static const struct gdi_dc_funcs wayland_gdi_dc_funcs =
{
    .pCreateDC = WAYLAND_CreateDC,
    .pCreateCompatibleDC = WAYLAND_CreateCompatibleDC,
    .pDeleteDC = WAYLAND_DeleteDC,
    .priority = GDI_PRIORITY_GRAPHICS_DRV
};

/******************************************************************************
 *      WAYLAND_get_gdi_driver
 */
const struct gdi_dc_funcs * CDECL WAYLAND_get_gdi_driver(unsigned int version)
{
    if (version != WINE_GDI_DRIVER_VERSION)
    {
        ERR("version mismatch, gdi32 wants %u but winewayland has %u\n",
            version, WINE_GDI_DRIVER_VERSION);
        return NULL;
    }
    return &wayland_gdi_dc_funcs;
}
