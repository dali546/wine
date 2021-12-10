/*
 * winewayland.drv options
 *
 * Copyright 1998 Patrik Stridvall
 * Copyright 2000 Alexandre Julliard
 * Copyright 2021 Alexandros Frantzis
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

/* Code to read options from the registry, adapted from the X11 driver */

#include "config.h"

#include "waylanddrv.h"

#include "wine/unicode.h"

#include "winreg.h"
#include "winuser.h"

#define IS_OPTION_TRUE(ch) \
    ((ch) == 'y' || (ch) == 'Y' || (ch) == 't' || (ch) == 'T' || (ch) == '1')

extern BOOL show_systray;

/***********************************************************************
 *		get_config_key
 *
 * Get a config key from either the app-specific or the default config
 */
static inline DWORD get_config_key(HKEY defkey, HKEY appkey, const char *name,
                                   DWORD flags, char *buffer, DWORD size)
{
    if (appkey && !RegGetValueA(appkey, NULL, name, flags, NULL, (LPBYTE)buffer, &size)) return 0;
    if (defkey && !RegGetValueA(defkey, NULL, name, flags, NULL, (LPBYTE)buffer, &size)) return 0;
    return ERROR_FILE_NOT_FOUND;
}

/***********************************************************************
 *		wayland_read_options_from_registry
 *
 * Read the Wayland driver options from the registry.
 */
void wayland_read_options_from_registry(struct wayland *wayland)
{
    static const WCHAR waylanddriverW[] = {'\\','W','a','y','l','a','n','d',' ','D','r','i','v','e','r',0};
    char buffer[64];
    WCHAR bufferW[MAX_PATH + 16];
    HKEY hkey, appkey = 0;
    DWORD len;

    /* @@ Wine registry key: HKCU\Software\Wine\Wayland Driver */
    if (RegOpenKeyA(HKEY_CURRENT_USER, "Software\\Wine\\Wayland Driver", &hkey)) hkey = 0;

    /* open the app-specific key */

    len = GetModuleFileNameW(0, bufferW, MAX_PATH);
    if (len && len < MAX_PATH)
    {
        HKEY tmpkey;
        WCHAR *p, *appname = bufferW;
        if ((p = strrchrW(appname, '/'))) appname = p + 1;
        if ((p = strrchrW(appname, '\\'))) appname = p + 1;
        CharLowerW(appname);
        strcatW(appname, waylanddriverW);
        /* @@ Wine registry key: HKCU\Software\Wine\AppDefaults\app.exe\Wayland Driver */
        if (!RegOpenKeyA(HKEY_CURRENT_USER, "Software\\Wine\\AppDefaults", &tmpkey))
        {
            if (RegOpenKeyW(tmpkey, appname, &appkey)) appkey = 0;
            RegCloseKey(tmpkey);
        }
    }

    if (!get_config_key(hkey, appkey, "HiDPIScaling", RRF_RT_REG_SZ, buffer, sizeof(buffer)))
    {
        if (!strcasecmp(buffer, "Application"))
            wayland->hidpi_scaling = WAYLAND_HIDPI_SCALING_APPLICATION;
        else if (!strcasecmp(buffer, "Compositor"))
            wayland->hidpi_scaling = WAYLAND_HIDPI_SCALING_COMPOSITOR;
    }

    if (!get_config_key(hkey, appkey, "ShowSystray", RRF_RT_REG_SZ, buffer, sizeof(buffer)))
        show_systray = IS_OPTION_TRUE(buffer[0]);

    if (appkey) RegCloseKey(appkey);
    if (hkey) RegCloseKey(hkey);
}
