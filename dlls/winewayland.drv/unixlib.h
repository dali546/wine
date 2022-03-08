/*
 * Copyright 2022 Alexandros Frantzis for Collabora Ltd
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

#ifndef __WINE_WAYLANDDRV_UNIXLIB_H
#define __WINE_WAYLANDDRV_UNIXLIB_H

#include "windef.h"
#include "ntuser.h"
#include "wine/unixlib.h"

/* A pointer to memory that is guaranteed to be usable by both 32-bit and
 * 64-bit processes. */
typedef UINT PTR32;

enum waylanddrv_unix_func
{
    waylanddrv_unix_func_init,
    waylanddrv_unix_func_count,
};

struct waylanddrv_unix_init_params
{
    NTSTATUS (WINAPI *pNtWaitForMultipleObjects)(ULONG,const HANDLE*,BOOLEAN,BOOLEAN,const LARGE_INTEGER*);
    NTSTATUS (CDECL *unix_call)(enum waylanddrv_unix_func func, void *params);
};

/* driver client callbacks exposed with KernelCallbackTable interface */
enum waylanddrv_client_func
{
    waylanddrv_client_func_load_cursor = NtUserDriverCallbackFirst,
    waylanddrv_client_func_last,
};

C_ASSERT(waylanddrv_client_func_last <= NtUserDriverCallbackLast + 1);

struct waylanddrv_client_load_cursor_params
{
    PTR32 name;
};

#endif /* __WINE_WAYLANDDRV_UNIXLIB_H */
