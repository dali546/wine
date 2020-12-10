/*
 * Wayland graphics driver initialisation functions
 *
 * Copyright 1996 Alexandre Julliard
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

/***********************************************************************
 *           WAYLAND_GetDeviceCaps
 */
static INT CDECL WAYLAND_GetDeviceCaps(PHYSDEV dev, INT cap)
{
    switch(cap)
    {
    default:
        dev = GET_NEXT_PHYSDEV(dev, pGetDeviceCaps);
        return dev->funcs->pGetDeviceCaps(dev, cap);
    }
}

/**********************************************************************
 *           WAYLAND_wine_get_wgl_driver
 */
static struct opengl_funcs * CDECL WAYLAND_wine_get_wgl_driver(PHYSDEV dev, UINT version)
{
    struct opengl_funcs *ret;

    if (!(ret = wayland_get_wgl_driver(version)))
    {
        dev = GET_NEXT_PHYSDEV(dev, wine_get_wgl_driver);
        ret = dev->funcs->wine_get_wgl_driver(dev, version);
    }

    return ret;
}

static const struct gdi_dc_funcs wayland_gdi_dc_funcs =
{
    NULL,                               /* pAbortDoc */
    NULL,                               /* pAbortPath */
    NULL,                               /* pAlphaBlend */
    NULL,                               /* pAngleArc */
    NULL,                               /* pArc */
    NULL,                               /* pArcTo */
    NULL,                               /* pBeginPath */
    NULL,                               /* pBlendImage */
    NULL,                               /* pChord */
    NULL,                               /* pCloseFigure */
    WAYLAND_CreateCompatibleDC,         /* pCreateCompatibleDC */
    WAYLAND_CreateDC,                   /* pCreateDC */
    WAYLAND_DeleteDC,                   /* pDeleteDC */
    NULL,                               /* pDeleteObject */
    NULL,                               /* pDeviceCapabilities */
    NULL,                               /* pEllipse */
    NULL,                               /* pEndDoc */
    NULL,                               /* pEndPage */
    NULL,                               /* pEndPath */
    NULL,                               /* pEnumFonts */
    NULL,                               /* pEnumICMProfiles */
    NULL,                               /* pExcludeClipRect */
    NULL,                               /* pExtDeviceMode */
    NULL,                               /* pExtEscape */
    NULL,                               /* pExtFloodFill */
    NULL,                               /* pExtSelectClipRgn */
    NULL,                               /* pExtTextOut */
    NULL,                               /* pFillPath */
    NULL,                               /* pFillRgn */
    NULL,                               /* pFlattenPath */
    NULL,                               /* pFontIsLinked */
    NULL,                               /* pFrameRgn */
    NULL,                               /* pGdiComment */
    NULL,                               /* pGetBoundsRect */
    NULL,                               /* pGetCharABCWidths */
    NULL,                               /* pGetCharABCWidthsI */
    NULL,                               /* pGetCharWidth */
    NULL,                               /* pGetCharWidthInfo */
    WAYLAND_GetDeviceCaps,              /* pGetDeviceCaps */
    NULL,                               /* pGetDeviceGammaRamp */
    NULL,                               /* pGetFontData */
    NULL,                               /* pGetFontRealizationInfo */
    NULL,                               /* pGetFontUnicodeRanges */
    NULL,                               /* pGetGlyphIndices */
    NULL,                               /* pGetGlyphOutline */
    NULL,                               /* pGetICMProfile */
    NULL,                               /* pGetImage */
    NULL,                               /* pGetKerningPairs */
    NULL,                               /* pGetNearestColor */
    NULL,                               /* pGetOutlineTextMetrics */
    NULL,                               /* pGetPixel */
    NULL,                               /* pGetSystemPaletteEntries */
    NULL,                               /* pGetTextCharsetInfo */
    NULL,                               /* pGetTextExtentExPoint */
    NULL,                               /* pGetTextExtentExPointI */
    NULL,                               /* pGetTextFace */
    NULL,                               /* pGetTextMetrics */
    NULL,                               /* pGradientFill */
    NULL,                               /* pIntersectClipRect */
    NULL,                               /* pInvertRgn */
    NULL,                               /* pLineTo */
    NULL,                               /* pModifyWorldTransform */
    NULL,                               /* pMoveTo */
    NULL,                               /* pOffsetClipRgn */
    NULL,                               /* pOffsetViewportOrg */
    NULL,                               /* pOffsetWindowOrg */
    NULL,                               /* pPaintRgn */
    NULL,                               /* pPatBlt */
    NULL,                               /* pPie */
    NULL,                               /* pPolyBezier */
    NULL,                               /* pPolyBezierTo */
    NULL,                               /* pPolyDraw */
    NULL,                               /* pPolyPolygon */
    NULL,                               /* pPolyPolyline */
    NULL,                               /* pPolygon */
    NULL,                               /* pPolyline */
    NULL,                               /* pPolylineTo */
    NULL,                               /* pPutImage */
    NULL,                               /* pRealizeDefaultPalette */
    NULL,                               /* pRealizePalette */
    NULL,                               /* pRectangle */
    NULL,                               /* pResetDC */
    NULL,                               /* pRestoreDC */
    NULL,                               /* pRoundRect */
    NULL,                               /* pSaveDC */
    NULL,                               /* pScaleViewportExt */
    NULL,                               /* pScaleWindowExt */
    NULL,                               /* pSelectBitmap */
    NULL,                               /* pSelectBrush */
    NULL,                               /* pSelectClipPath */
    NULL,                               /* pSelectFont */
    NULL,                               /* pSelectPalette */
    NULL,                               /* pSelectPen */
    NULL,                               /* pSetArcDirection */
    NULL,                               /* pSetBkColor */
    NULL,                               /* pSetBkMode */
    NULL,                               /* pSetBoundsRect */
    NULL,                               /* pSetDCBrushColor */
    NULL,                               /* pSetDCPenColor */
    NULL,                               /* pSetDIBitsToDevice */
    NULL,                               /* pSetDeviceClipping */
    NULL,                               /* pSetDeviceGammaRamp */
    NULL,                               /* pSetLayout */
    NULL,                               /* pSetMapMode */
    NULL,                               /* pSetMapperFlags */
    NULL,                               /* pSetPixel */
    NULL,                               /* pSetPolyFillMode */
    NULL,                               /* pSetROP2 */
    NULL,                               /* pSetRelAbs */
    NULL,                               /* pSetStretchBltMode */
    NULL,                               /* pSetTextAlign */
    NULL,                               /* pSetTextCharacterExtra */
    NULL,                               /* pSetTextColor */
    NULL,                               /* pSetTextJustification */
    NULL,                               /* pSetViewportExt */
    NULL,                               /* pSetViewportOrg */
    NULL,                               /* pSetWindowExt */
    NULL,                               /* pSetWindowOrg */
    NULL,                               /* pSetWorldTransform */
    NULL,                               /* pStartDoc */
    NULL,                               /* pStartPage */
    NULL,                               /* pStretchBlt */
    NULL,                               /* pStretchDIBits */
    NULL,                               /* pStrokeAndFillPath */
    NULL,                               /* pStrokePath */
    NULL,                               /* pUnrealizePalette */
    NULL,                               /* pWidenPath */
    NULL,                               /* pD3DKMTCheckVidPnExclusiveOwnership */
    NULL,                               /* pD3DKMTSetVidPnSourceOwner */
    WAYLAND_wine_get_wgl_driver,        /* wine_get_wgl_driver */
    NULL,                               /* wine_get_vulkan_driver */
    GDI_PRIORITY_GRAPHICS_DRV           /* priority */
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
