/*
 * WAYLANDDRV initialization code
 *
 * Copyright 2020 Alexandre Frantzis for Collabora Ltd
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

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "waylanddrv.h"

#include "wine/debug.h"
#include "wine/server.h"

#include <stdlib.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

char *process_name = NULL;
char *option_drm_device = NULL;
BOOL option_use_system_cursors = TRUE;

#define IS_OPTION_TRUE(ch) \
    ((ch) == 'y' || (ch) == 'Y' || (ch) == 't' || (ch) == 'T' || (ch) == '1')

/**********************************************************************
 *          ascii_to_unicode_maybe_z
 *
 * Converts an ascii, possibly zero-terminated, string containing up to
 * src_max_chars to a unicode string. Returns the number of characters
 * (including any trailing zero) in the source ascii string. If the returned
 * number of characters is greater than dst_max_chars the output will have been
 * truncated.
 */
static size_t ascii_to_unicode_maybe_z(WCHAR *dst, size_t dst_max_chars,
                                       const char *src, size_t src_max_chars)
{
    size_t src_len = 0;

    while (src_max_chars--)
    {
        src_len++;
        if (dst_max_chars)
        {
            *dst++ = *src;
            dst_max_chars--;
        }
        if (!*src++) break;
    }

    return src_len;
}

/**********************************************************************
 *          ascii_to_unicode_z
 *
 * Converts an ascii, possibly zero-terminated, string containing up to
 * src_max_chars to a zero-terminated unicode string. Returns the number of
 * characters (including the trailing zero) written to the destination string.
 * If there isn't enough space in the destination to hold all the characters
 * and the trailing zero, the string is truncated enough so that a trailing
 * zero can be placed.
 */
size_t ascii_to_unicode_z(WCHAR *dst, size_t dst_max_chars,
                          const char *src, size_t src_max_chars)
{
    size_t len;
    if (src_max_chars == 0) return 0;
    len = ascii_to_unicode_maybe_z(dst, dst_max_chars, src, src_max_chars);
    if (len >= dst_max_chars) len = dst_max_chars - 1;
    if (len > 0 && dst[len - 1] == 0) len--;
    dst[len] = 0;
    return len + 1;
}

/**********************************************************************
 *          unicode_to_ascii_maybe_z
 *
 * Converts a unicode, possibly zero-terminated, string containing up to
 * src_max_chars to an ascii string. Returns the number of characters
 * (including any trailing zero) in the source unicode string. If the returned
 * number of characters is greater than dst_max_chars the output will have been
 * truncated.
 */
static size_t unicode_to_ascii_maybe_z(char *dst, size_t dst_max_chars,
                                       const WCHAR *src, size_t src_max_chars)
{
    size_t src_len = 0;

    while (src_max_chars--)
    {
        src_len++;
        if (dst_max_chars)
        {
            *dst++ = *src;
            dst_max_chars--;
        }
        if (!*src++) break;
    }

    return src_len;
}

static HKEY reg_open_key_w(HKEY root, const WCHAR *nameW)
{
    INT name_len = nameW ? lstrlenW(nameW) * sizeof(WCHAR) : 0;
    UNICODE_STRING name_unicode = { name_len, name_len, (WCHAR *)nameW };
    OBJECT_ATTRIBUTES attr;
    HANDLE ret;

    if (!nameW || !*nameW) return root;

    attr.Length = sizeof(attr);
    attr.RootDirectory = root;
    attr.ObjectName = &name_unicode;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    return NtOpenKeyEx(&ret, MAXIMUM_ALLOWED, &attr, 0) ? 0 : ret;
}

static HKEY reg_open_key_a(HKEY root, const char *name)
{
    WCHAR nameW[256];
    if (!name || !*name) return root;
    if (ascii_to_unicode_maybe_z(nameW, ARRAY_SIZE(nameW), name, -1) > ARRAY_SIZE(nameW))
        return 0;
    return reg_open_key_w(root, nameW);
}

static HKEY reg_open_hkcu_key_a(const char *name)
{
    static HKEY hkcu;

    if (!hkcu)
    {
        char buffer[256];
        DWORD_PTR sid_data[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(DWORD_PTR)];
        DWORD i, len = sizeof(sid_data);
        SID *sid;

        if (NtQueryInformationToken(GetCurrentThreadEffectiveToken(), TokenUser, sid_data,
                                    len, &len))
        {
            return 0;
        }

        sid = ((TOKEN_USER *)sid_data)->User.Sid;
        len = snprintf(buffer, ARRAY_SIZE(buffer), "\\Registry\\User\\S-%u-%u",
                       sid->Revision,
                       (int)MAKELONG(MAKEWORD(sid->IdentifierAuthority.Value[5],
                                              sid->IdentifierAuthority.Value[4]),
                                     MAKEWORD(sid->IdentifierAuthority.Value[3],
                                              sid->IdentifierAuthority.Value[2])));
        if (len >= ARRAY_SIZE(buffer)) return 0;

        for (i = 0; i < sid->SubAuthorityCount; i++)
        {
            len += snprintf(buffer + len, ARRAY_SIZE(buffer) - len, "-%u",
                            (UINT)sid->SubAuthority[i]);
            if (len >= ARRAY_SIZE(buffer)) return 0;
        }

        hkcu = reg_open_key_a(NULL, buffer);
    }

    return reg_open_key_a(hkcu, name);
}

static DWORD reg_get_value_info(HKEY hkey, const WCHAR *nameW, ULONG type,
                                KEY_VALUE_PARTIAL_INFORMATION *info,
                                ULONG info_size)
{
    unsigned int name_size = lstrlenW(nameW) * sizeof(WCHAR);
    UNICODE_STRING name_unicode = { name_size, name_size, (WCHAR *)nameW };

    if (NtQueryValueKey(hkey, &name_unicode, KeyValuePartialInformation,
                        info, info_size, &info_size))
        return ERROR_FILE_NOT_FOUND;

    if (info->Type != type) return ERROR_DATATYPE_MISMATCH;

    return ERROR_SUCCESS;
}

/**********************************************************************
 *          reg_get_value_a
 *
 *  Get the value of the specified registry key (or subkey if name is not NULL),
 *  having the specified type. If the types do not match an error is returned.
 *  If the stored value is REG_SZ the string is transformed into ASCII before
 *  being returned.
 */
static DWORD reg_get_value_a(HKEY hkey, const char *name, ULONG type,
                             char *buffer, DWORD *buffer_len)
{
    WCHAR nameW[256];
    char info_buf[2048];
    KEY_VALUE_PARTIAL_INFORMATION *info = (void *)info_buf;
    ULONG info_size = ARRAY_SIZE(info_buf);
    DWORD err;

    if (name && ascii_to_unicode_maybe_z(nameW, ARRAY_SIZE(nameW), name, -1) > ARRAY_SIZE(nameW))
        return ERROR_INSUFFICIENT_BUFFER;

    if ((err = reg_get_value_info(hkey, name ? nameW : NULL, type, info, info_size)))
        return err;

    if (type == REG_SZ)
    {
        size_t nchars = unicode_to_ascii_maybe_z(buffer, *buffer_len, (WCHAR *)info->Data,
                                                 info->DataLength / sizeof(WCHAR));
        err = *buffer_len >= nchars ? ERROR_SUCCESS : ERROR_MORE_DATA;
        *buffer_len = nchars;
    }
    else
    {
        err = *buffer_len >= info->DataLength ? ERROR_SUCCESS : ERROR_MORE_DATA;
        if (err == ERROR_SUCCESS) memcpy(buffer, info->Data, info->DataLength);
        *buffer_len = info->DataLength;
    }

    return err;
}

/***********************************************************************
 *		get_config_key
 *
 * Get a config key from either the app-specific or the default config
 */
static inline DWORD get_config_key(HKEY defkey, HKEY appkey, const char *name,
                                   ULONG type, char *buffer, DWORD size)
{
    if (appkey && !reg_get_value_a(appkey, name, type, buffer, &size)) return 0;
    if (defkey && !reg_get_value_a(defkey, name, type, buffer, &size)) return 0;
    return ERROR_FILE_NOT_FOUND;
}

/***********************************************************************
 *		wayland_read_options_from_registry
 *
 * Read the Wayland driver options from the registry.
 */
static void wayland_read_options_from_registry(void)
{
    static const WCHAR waylanddriverW[] = {'\\','W','a','y','l','a','n','d',' ','D','r','i','v','e','r',0};
    char buffer[64];
    HKEY hkey, appkey = 0;
    DWORD process_name_len;

    /* @@ Wine registry key: HKCU\Software\Wine\Wayland Driver */
    hkey = reg_open_hkcu_key_a("Software\\Wine\\Wayland Driver");

    /* open the app-specific key */
    process_name_len = process_name ? strlen(process_name) : 0;
    if (process_name_len > 0)
    {
        WCHAR appname[MAX_PATH + sizeof(waylanddriverW) / sizeof(WCHAR)];
        DWORD reslen;
        if (!RtlUTF8ToUnicodeN(appname, MAX_PATH * sizeof(WCHAR), &reslen,
                               process_name, process_name_len))
        {
            HKEY tmpkey;
            memcpy((char *)appname + reslen, waylanddriverW, sizeof(waylanddriverW));
            /* @@ Wine registry key: HKCU\Software\Wine\AppDefaults\app.exe\Wayland Driver */
            if ((tmpkey = reg_open_hkcu_key_a("Software\\Wine\\AppDefaults")))
            {
                appkey = reg_open_key_w(tmpkey, appname);
                NtClose(tmpkey);
            }
        }
    }

    if (!get_config_key(hkey, appkey, "DRMDevice", REG_SZ, buffer, sizeof(buffer)))
        option_drm_device = strdup(buffer);

    if (!get_config_key(hkey, appkey, "UseSystemCursors", REG_SZ, buffer, sizeof(buffer)))
        option_use_system_cursors = IS_OPTION_TRUE(buffer[0]);

    if (appkey) NtClose(appkey);
    if (hkey) NtClose(hkey);
}

static void set_queue_fd(struct wayland *wayland)
{
    HANDLE handle;
    int wfd;
    int ret;

    wfd = wayland->event_notification_pipe[0];

    if (wine_server_fd_to_handle(wfd, GENERIC_READ | SYNCHRONIZE, 0, &handle))
    {
        ERR("Can't allocate handle for wayland fd\n");
        NtTerminateProcess(0, 1);
    }

    SERVER_START_REQ(set_queue_fd)
    {
        req->handle = wine_server_obj_handle(handle);
        ret = wine_server_call(req);
    }
    SERVER_END_REQ;

    if (ret)
    {
        ERR("Can't store handle for wayland fd %x\n", ret);
        NtTerminateProcess(0, 1);
    }

    NtClose(handle);
}

/***********************************************************************
 *           Initialize per thread data
 */
struct wayland_thread_data *wayland_init_thread_data(void)
{
    struct wayland_thread_data *data = wayland_thread_data();

    if (data) return data;

    if (!(data = calloc(1, sizeof(*data))))
    {
        ERR("could not create data\n");
        NtTerminateProcess(0, 1);
    }

    if (!wayland_init(&data->wayland))
    {
        ERR_(winediag)("waylanddrv: Can't open wayland display. Please ensure "
                       "that your wayland server is running and that "
                       "$WAYLAND_DISPLAY is set correctly.\n");
        NtTerminateProcess(0, 1);
    }

    set_queue_fd(&data->wayland);
    NtUserGetThreadInfo()->driver_data = (UINT_PTR)data;

    return data;
}

/***********************************************************************
 *           ThreadDetach (WAYLAND.@)
 */
static void WAYLAND_ThreadDetach(void)
{
    struct wayland_thread_data *data = wayland_thread_data();

    if (data)
    {
        wayland_deinit(&data->wayland);
        free(data);
        /* clear data in case we get re-entered from user32 before the thread is truly dead */
        NtUserGetThreadInfo()->driver_data = 0;
    }
}

static const struct user_driver_funcs waylanddrv_funcs =
{
    .pChangeDisplaySettings = WAYLAND_ChangeDisplaySettings,
    .pCreateWindow = WAYLAND_CreateWindow,
    .pDesktopWindowProc = WAYLAND_DesktopWindowProc,
    .pDestroyWindow = WAYLAND_DestroyWindow,
    .pGetCurrentDisplaySettings = WAYLAND_GetCurrentDisplaySettings,
    .pGetDisplayDepth = WAYLAND_GetDisplayDepth,
    .pGetKeyNameText = WAYLAND_GetKeyNameText,
    .pMapVirtualKeyEx = WAYLAND_MapVirtualKeyEx,
    .pProcessEvents = WAYLAND_ProcessEvents,
    .pSetCursor = WAYLAND_SetCursor,
    .pSetLayeredWindowAttributes = WAYLAND_SetLayeredWindowAttributes,
    .pSetWindowRgn = WAYLAND_SetWindowRgn,
    .pSetWindowStyle = WAYLAND_SetWindowStyle,
    .pShowWindow = WAYLAND_ShowWindow,
    .pSysCommand = WAYLAND_SysCommand,
    .pToUnicodeEx = WAYLAND_ToUnicodeEx,
    .pUpdateLayeredWindow = WAYLAND_UpdateLayeredWindow,
    .pVkKeyScanEx = WAYLAND_VkKeyScanEx,
    .pThreadDetach = WAYLAND_ThreadDetach,
    .pUpdateDisplayDevices = WAYLAND_UpdateDisplayDevices,
    .pWindowMessage = WAYLAND_WindowMessage,
    .pWindowPosChanged = WAYLAND_WindowPosChanged,
    .pWindowPosChanging = WAYLAND_WindowPosChanging,
    .pwine_get_wgl_driver = WAYLAND_wine_get_wgl_driver,
};

static void wayland_init_process_name(void)
{
    WCHAR *p, *appname;
    WCHAR appname_lower[MAX_PATH];
    DWORD appname_len;
    DWORD appnamez_size;
    DWORD utf8_size;
    int i;

    appname = NtCurrentTeb()->Peb->ProcessParameters->ImagePathName.Buffer;
    if ((p = wcsrchr(appname, '/'))) appname = p + 1;
    if ((p = wcsrchr(appname, '\\'))) appname = p + 1;
    appname_len = lstrlenW(appname);

    if (appname_len == 0 || appname_len >= MAX_PATH) return;

    for (i = 0; appname[i]; i++) appname_lower[i] = RtlDowncaseUnicodeChar(appname[i]);
    appname_lower[i] = 0;

    appnamez_size = (appname_len + 1) * sizeof(WCHAR);

    if (!RtlUnicodeToUTF8N(NULL, 0, &utf8_size, appname_lower, appnamez_size) &&
        (process_name = malloc(utf8_size)))
    {
        RtlUnicodeToUTF8N(process_name, utf8_size, &utf8_size, appname_lower, appnamez_size);
    }
}

static NTSTATUS waylanddrv_unix_init(void *arg)
{
    /* Set the user driver functions now so that they are available during
     * our initialization. We clear them on error. */
    __wine_set_user_driver(&waylanddrv_funcs, WINE_GDI_DRIVER_VERSION);

    wayland_init_process_name();

    wayland_read_options_from_registry();

    if (!wayland_init_set_cursor()) goto err;

    if (!wayland_process_init()) goto err;

    return 0;

err:
    __wine_set_user_driver(NULL, WINE_GDI_DRIVER_VERSION);
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS waylanddrv_unix_read_events(void *arg)
{
    while (wayland_read_events_and_dispatch_process()) continue;
    /* This function only returns on a fatal error, e.g., if our connection
     * to the Wayland server is lost. */
    return STATUS_UNSUCCESSFUL;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    waylanddrv_unix_init,
    waylanddrv_unix_read_events,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == waylanddrv_unix_func_count);

#ifdef _WIN64

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    waylanddrv_unix_init,
    waylanddrv_unix_read_events,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == waylanddrv_unix_func_count);

#endif /* _WIN64 */
