/*
 * WAYLANDDRV initialization code
 *
 * Copyright 1998 Patrik Stridvall
 * Copyright 2000 Alexandre Julliard
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

#include "config.h"

#include "waylanddrv.h"

#include "wine/debug.h"
#include "wine/server.h"

#include <stdlib.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

DWORD thread_data_tls_index = TLS_OUT_OF_INDEXES;

static DWORD WINAPI wayland_read_thread(void *arg)
{
    while (wayland_read_events_and_dispatch_process()) continue;
    /* This thread terminates only if an unrecoverable error occured during
     * event reading. */
    exit(1);
    return 0;
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
        ExitProcess(1);
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
        ExitProcess(1);
    }

    CloseHandle(handle);
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
        ExitProcess(1);
    }

    if (!wayland_init(&data->wayland))
    {
        ERR_(winediag)("waylanddrv: Can't open wayland display. Please ensure "
                       "that your wayland server is running and that "
                       "$WAYLAND_DISPLAY is set correctly.\n");
        ExitProcess(1);
    }

    set_queue_fd(&data->wayland);
    TlsSetValue(thread_data_tls_index, data);

    return data;
}

/***********************************************************************
 *           ThreadDetach (WAYLAND.@)
 */
static void CDECL WAYLAND_ThreadDetach(void)
{
    struct wayland_thread_data *data = wayland_thread_data();

    if (data)
    {
        wayland_deinit(&data->wayland);
        free(data);
        /* clear data in case we get re-entered from user32 before the thread is truly dead */
        TlsSetValue(thread_data_tls_index, NULL);
    }
}

static const struct user_driver_funcs waylanddrv_funcs =
{
    .pCreateWindow = WAYLAND_CreateWindow,
    .pDestroyWindow = WAYLAND_DestroyWindow,
    .pEnumDisplaySettingsEx = WAYLAND_EnumDisplaySettingsEx,
    .pMsgWaitForMultipleObjectsEx = WAYLAND_MsgWaitForMultipleObjectsEx,
    .pSetCursor = WAYLAND_SetCursor,
    .pToUnicodeEx = WAYLAND_ToUnicodeEx,
    .pThreadDetach = WAYLAND_ThreadDetach,
    .pUpdateDisplayDevices = WAYLAND_UpdateDisplayDevices,
    .pWindowMessage = WAYLAND_WindowMessage,
    .pWindowPosChanged = WAYLAND_WindowPosChanged,
    .pWindowPosChanging = WAYLAND_WindowPosChanging,
};

/***********************************************************************
 *           WAYLANDDRV process initialisation routine
 */
static BOOL process_attach(void)
{
    DWORD id;

    if ((thread_data_tls_index = TlsAlloc()) == TLS_OUT_OF_INDEXES) return FALSE;

    __wine_set_user_driver(&waylanddrv_funcs, WINE_GDI_DRIVER_VERSION);

    if (!wayland_init_set_cursor()) return FALSE;

    if (!wayland_process_init()) return FALSE;

    /* Read wayland events from a dedicated thread. */
    CreateThread(NULL, 0, wayland_read_thread, NULL, 0, &id);

    return TRUE;
}

/***********************************************************************
 *           WAYLANDDRV initialisation routine
 */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    BOOL ret = TRUE;

    switch(reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinst);
        ret = process_attach();
        break;
    }

    return ret;
}
