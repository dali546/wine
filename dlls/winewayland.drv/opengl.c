/*
 * Wayland OpenGL functions
 *
 * Copyright 2000 Lionel Ulmer
 * Copyright 2005 Alex Woods
 * Copyright 2005 Raphael Junqueira
 * Copyright 2006-2009 Roderick Colenbrander
 * Copyright 2006 Tomas Carnecky
 * Copyright 2013 Matteo Bruni
 * Copyright 2012, 2013, 2014, 2017 Alexandre Julliard
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd.
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

#define GLAPIENTRY /* nothing */
#include "wine/wgl.h"
#undef GLAPIENTRY
#include "wine/wgl_driver.h"
#include "wine/debug.h"

#include <EGL/egl.h>
#include <assert.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static void *egl_handle;
static void *opengl_handle;

static BOOL egl_init(void)
{
    static int retval = -1;

    if (retval != -1) return retval;
    retval = 0;

    if (!(egl_handle = dlopen(SONAME_LIBEGL, RTLD_NOW|RTLD_GLOBAL)))
    {
        ERR("failed to load %s: %s\n", SONAME_LIBEGL, dlerror());
        return FALSE;
    }

    if (!(opengl_handle = dlopen(SONAME_LIBGL, RTLD_NOW|RTLD_GLOBAL)))
    {
        WARN("failed to load %s: %s, trying GLESv2\n", SONAME_LIBGL, dlerror());
        if (!(opengl_handle = dlopen(SONAME_LIBGLESV2, RTLD_NOW|RTLD_GLOBAL)))
        {
            ERR("failed to load %s: %s\n", SONAME_LIBGLESV2, dlerror());
            return FALSE;
        }
    }

    retval = 1;
    return TRUE;
}

/* generate stubs for GL functions that are not exported */

#define USE_GL_FUNC(name) \
static void glstub_##name(void) \
{ \
    ERR(#name " called\n"); \
    assert(0); \
    ExitProcess(1); \
}

ALL_WGL_FUNCS
#undef USE_GL_FUNC

static struct opengl_funcs egl_funcs =
{
#define USE_GL_FUNC(name) (void *)glstub_##name,
    .gl = { ALL_WGL_FUNCS }
#undef USE_GL_FUNC
};

struct opengl_funcs *wayland_get_wgl_driver(UINT version)
{
    if (version != WINE_WGL_DRIVER_VERSION)
    {
        ERR("version mismatch, opengl32 wants %u but driver has %u\n",
            version, WINE_WGL_DRIVER_VERSION);
        return NULL;
    }
    if (!egl_init()) return NULL;
    return &egl_funcs;
}
