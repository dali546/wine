/*
 * winewayland.drv entry points
 *
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

#include "waylanddrv_dll.h"

static unixlib_handle_t waylanddrv_handle;
NTSTATUS (CDECL *waylanddrv_unix_call)(enum waylanddrv_unix_func func, void *params);

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    struct waylanddrv_unix_init_params init_params =
    {
        .pNtWaitForMultipleObjects = NtWaitForMultipleObjects,
    };

    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls(instance);
    if (NtQueryVirtualMemory(GetCurrentProcess(), instance, MemoryWineUnixFuncs,
                             &waylanddrv_handle, sizeof(waylanddrv_handle), NULL))
        return FALSE;

    if (__wine_unix_call(waylanddrv_handle, waylanddrv_unix_func_init, &init_params))
        return FALSE;

    waylanddrv_unix_call = init_params.unix_call;

    return TRUE;
}
