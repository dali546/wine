/*
 * Registry helpers
 *
 * Copyright (c) 2022 Alexandros Frantzis for Collabora Ltd
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

/**********************************************************************
 *          reg_open_key_a
 *
 *  Open a registry key with the specified ASCII name.
 */
HKEY reg_open_key_a(HKEY root, const char *name)
{
    WCHAR nameW[256];
    if (!name || !*name) return root;
    if (ascii_to_unicode_maybe_z(nameW, ARRAY_SIZE(nameW), name, -1) > ARRAY_SIZE(nameW))
        return 0;
    return reg_open_key_w(root, nameW);
}

/**********************************************************************
 *          reg_open_key_w
 *
 *  Open a registry key with the specified Unicode name.
 */
HKEY reg_open_key_w(HKEY root, const WCHAR *nameW)
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
DWORD reg_get_value_a(HKEY hkey, const char *name, ULONG type, char *buffer,
                      DWORD *buffer_len)
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

/**********************************************************************
 *          reg_get_value_w
 *
 *  Get the value of the specified registry key (or subkey if name is not NULL),
 *  having the specified type. If the types do not match an error is returned.
 */
DWORD reg_get_value_w(HKEY hkey, const WCHAR *nameW, ULONG type, char *buffer,
                      DWORD *buffer_len)
{
    char info_buf[2048];
    KEY_VALUE_PARTIAL_INFORMATION *info = (void *)info_buf;
    ULONG info_size = ARRAY_SIZE(info_buf);
    DWORD err;

    if ((err = reg_get_value_info(hkey, nameW, type, info, info_size)))
        return err;

    err = *buffer_len >= info->DataLength ? ERROR_SUCCESS : ERROR_MORE_DATA;
    if (err == ERROR_SUCCESS) memcpy(buffer, info->Data, info->DataLength);
    *buffer_len = info->DataLength;

    return err;
}

/**********************************************************************
 *          reg_set_value_a
 *
 *  Set the value an type of the specified registry key (or subkey if name is
 *  not NULL).
 */
DWORD reg_set_value_a(HKEY hkey, const char *name, ULONG type, const char *buffer,
                      DWORD buffer_len)
{
    WCHAR nameW[256];
    UNICODE_STRING name_unicode;

    if (name)
    {
        size_t nchars = ascii_to_unicode_maybe_z(nameW, ARRAY_SIZE(nameW), name, -1);
        if (nchars > ARRAY_SIZE(nameW)) return ERROR_INSUFFICIENT_BUFFER;
        name_unicode = (UNICODE_STRING) { (nchars - 1) * sizeof(WCHAR), sizeof(nameW), nameW };
    }
    else
    {
        name_unicode = (UNICODE_STRING) { 0, 0, 0 };
    }

    return NtSetValueKey(hkey, &name_unicode, 0, type, buffer, buffer_len) ?
           ERROR_FILE_NOT_FOUND : ERROR_SUCCESS;
}
