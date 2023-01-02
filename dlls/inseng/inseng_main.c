/*
 *    INSENG Implementation
 *
 * Copyright 2006 Mike McCormack
 * Copyright 2016 Michael Müller
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

#define COBJMACROS


#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "ole2.h"
#include "rpcproxy.h"
#include "urlmon.h"
#include "shlwapi.h"
#include "initguid.h"
#include "inseng.h"

#include "inseng_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(inseng);

enum thread_operation
{
    OP_DOWNLOAD,
    OP_INSTALL
};

struct thread_info
{
    DWORD operation;
    DWORD jobflags;
    IEnumCifComponents *enum_comp;

    DWORD download_size;
    DWORD install_size;

    DWORD downloaded_kb;
    ULONGLONG download_start;
};

struct InstallEngine {
    IInstallEngine2 IInstallEngine2_iface;
    IInstallEngineTiming IInstallEngineTiming_iface;
    LONG ref;

    IInstallEngineCallback *callback;
    char *baseurl;
    char *downloaddir;
    ICifFile *icif;
    DWORD status;

    /* used for the installation thread */
    struct thread_info thread;
};

struct downloadcb
{
    IBindStatusCallback IBindStatusCallback_iface;
    LONG ref;

    WCHAR *file_name;
    WCHAR *cache_file;

    char *id;
    char *display;

    DWORD dl_size;
    DWORD dl_previous_kb;

    InstallEngine *engine;
    HANDLE event_done;
    HRESULT hr;
};

static inline InstallEngine *impl_from_IInstallEngine2(IInstallEngine2 *iface)
{
    return CONTAINING_RECORD(iface, InstallEngine, IInstallEngine2_iface);
}

static inline struct downloadcb *impl_from_IBindStatusCallback(IBindStatusCallback *iface)
{
    return CONTAINING_RECORD(iface, struct downloadcb, IBindStatusCallback_iface);
}

static inline InstallEngine *impl_from_IInstallEngineTiming(IInstallEngineTiming *iface)
{
    return CONTAINING_RECORD(iface, InstallEngine, IInstallEngineTiming_iface);
}

static HRESULT WINAPI downloadcb_QueryInterface(IBindStatusCallback *iface, REFIID riid, void **ppv)
{
    struct downloadcb *This = impl_from_IBindStatusCallback(iface);

    if (IsEqualGUID(&IID_IUnknown, riid))
    {
        TRACE("(%p)->(IID_IUnknown %p)\n", This, ppv);
        *ppv = &This->IBindStatusCallback_iface;
    }
    else if (IsEqualGUID(&IID_IBindStatusCallback, riid))
    {
        TRACE("(%p)->(IID_IBindStatusCallback %p)\n", This, ppv);
        *ppv = &This->IBindStatusCallback_iface;
    }
    else
    {
        FIXME("(%p)->(%s %p) not found\n", This, debugstr_guid(riid), ppv);
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*ppv);
    return S_OK;
}

static ULONG WINAPI downloadcb_AddRef(IBindStatusCallback *iface)
{
    struct downloadcb *This = impl_from_IBindStatusCallback(iface);
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref = %d\n", This, ref);

    return ref;
}

static ULONG WINAPI downloadcb_Release(IBindStatusCallback *iface)
{
    struct downloadcb *This = impl_from_IBindStatusCallback(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref = %d\n", This, ref);

    if (!ref)
    {
        heap_free(This->file_name);
        heap_free(This->cache_file);

        IInstallEngine2_Release(&This->engine->IInstallEngine2_iface);
        heap_free(This);
    }

    return ref;
}

static HRESULT WINAPI downloadcb_OnStartBinding(IBindStatusCallback *iface, DWORD reserved, IBinding *pbind)
{
    struct downloadcb *This = impl_from_IBindStatusCallback(iface);

    TRACE("(%p)->(%u %p)\n", This, reserved, pbind);

    return S_OK;
}

static HRESULT WINAPI downloadcb_GetPriority(IBindStatusCallback *iface, LONG *priority)
{
    struct downloadcb *This = impl_from_IBindStatusCallback(iface);

    FIXME("(%p)->(%p): stub\n", This, priority);

    return E_NOTIMPL;
}

static HRESULT WINAPI downloadcb_OnLowResource(IBindStatusCallback *iface, DWORD reserved)
{
    struct downloadcb *This = impl_from_IBindStatusCallback(iface);

    FIXME("(%p)->(%u): stub\n", This, reserved);

    return E_NOTIMPL;
}

static HRESULT WINAPI downloadcb_OnProgress(IBindStatusCallback *iface, ULONG progress,
        ULONG progress_max, ULONG status, const WCHAR *status_text)
{
    struct downloadcb *This = impl_from_IBindStatusCallback(iface);
    HRESULT hr = S_OK;

    TRACE("%p)->(%u %u %u %s)\n", This, progress, progress_max, status, debugstr_w(status_text));

    switch(status)
    {
        case BINDSTATUS_BEGINDOWNLOADDATA:
            if (!This->engine->thread.download_start)
                This->engine->thread.download_start = GetTickCount64();
            /* fall-through */
        case BINDSTATUS_DOWNLOADINGDATA:
        case BINDSTATUS_ENDDOWNLOADDATA:
            This->engine->thread.downloaded_kb = This->dl_previous_kb + progress / 1024;
            if (This->engine->callback)
            {
                hr = IInstallEngineCallback_OnComponentProgress(This->engine->callback,
                         This->id, INSTALLSTATUS_DOWNLOADING, This->display, NULL, progress / 1024, This->dl_size);
            }
            break;

        case BINDSTATUS_CACHEFILENAMEAVAILABLE:
            This->cache_file = strdupW(status_text);
            if (!This->cache_file)
            {
                ERR("Failed to allocate memory for cache file\n");
                hr = E_OUTOFMEMORY;
            }
            break;

        case BINDSTATUS_CONNECTING:
        case BINDSTATUS_SENDINGREQUEST:
        case BINDSTATUS_MIMETYPEAVAILABLE:
        case BINDSTATUS_FINDINGRESOURCE:
            break;

        default:
            FIXME("Unsupported status %u\n", status);
    }

    return hr;
}

static HRESULT WINAPI downloadcb_OnStopBinding(IBindStatusCallback *iface, HRESULT hresult, LPCWSTR szError)
{
    struct downloadcb *This = impl_from_IBindStatusCallback(iface);

    TRACE("(%p)->(%08x %s)\n", This, hresult, debugstr_w(szError));

    if (FAILED(hresult))
    {
        This->hr = hresult;
        goto done;
    }

    if (!This->cache_file)
    {
        This->hr = E_FAIL;
        goto done;
    }

    if (CopyFileW(This->cache_file, This->file_name, FALSE))
        This->hr = S_OK;
    else
    {
        ERR("CopyFile failed: %u\n", GetLastError());
        This->hr = E_FAIL;
    }

done:
    SetEvent(This->event_done);
    return S_OK;
}

static HRESULT WINAPI downloadcb_GetBindInfo(IBindStatusCallback *iface,
        DWORD *grfBINDF, BINDINFO *pbindinfo)
{
    struct downloadcb *This = impl_from_IBindStatusCallback(iface);

    TRACE("(%p)->(%p %p)\n", This, grfBINDF, pbindinfo);

    *grfBINDF = BINDF_PULLDATA | BINDF_NEEDFILE;
    return S_OK;
}

static HRESULT WINAPI downloadcb_OnDataAvailable(IBindStatusCallback *iface,
        DWORD grfBSCF, DWORD dwSize, FORMATETC *pformatetc, STGMEDIUM *pstgmed)
{
    struct downloadcb *This = impl_from_IBindStatusCallback(iface);

    TRACE("(%p)->(%08x %u %p %p)\n", This, grfBSCF, dwSize, pformatetc, pstgmed);

    return S_OK;
}

static HRESULT WINAPI downloadcb_OnObjectAvailable(IBindStatusCallback *iface,
        REFIID riid, IUnknown *punk)
{
    struct downloadcb *This = impl_from_IBindStatusCallback(iface);

    FIXME("(%p)->(%s %p): stub\n", This, debugstr_guid(riid), punk);

    return E_NOTIMPL;
}

static const IBindStatusCallbackVtbl BindStatusCallbackVtbl =
{
    downloadcb_QueryInterface,
    downloadcb_AddRef,
    downloadcb_Release,
    downloadcb_OnStartBinding,
    downloadcb_GetPriority,
    downloadcb_OnLowResource,
    downloadcb_OnProgress,
    downloadcb_OnStopBinding,
    downloadcb_GetBindInfo,
    downloadcb_OnDataAvailable,
    downloadcb_OnObjectAvailable
};

static HRESULT downloadcb_create(InstallEngine *engine, HANDLE event, char *file_name, char *id,
                                 char *display, DWORD dl_size, struct downloadcb **callback)
{
    struct downloadcb *cb;

    cb = heap_alloc_zero(sizeof(*cb));
    if (!cb) return E_OUTOFMEMORY;

    cb->IBindStatusCallback_iface.lpVtbl = &BindStatusCallbackVtbl;
    cb->ref = 1;
    cb->hr = E_FAIL;
    cb->id = id;
    cb->display = display;
    cb->engine = engine;
    cb->dl_size = dl_size;
    cb->dl_previous_kb = engine->thread.downloaded_kb;
    cb->event_done = event;
    cb->file_name = strAtoW(file_name);
    if (!cb->file_name)
    {
        heap_free(cb);
        return E_OUTOFMEMORY;
    }

    IInstallEngine2_AddRef(&engine->IInstallEngine2_iface);

    *callback = cb;
    return S_OK;
}

static HRESULT WINAPI InstallEngine_QueryInterface(IInstallEngine2 *iface, REFIID riid, void **ppv)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        TRACE("(%p)->(IID_IUnknown %p)\n", This, ppv);
        *ppv = &This->IInstallEngine2_iface;
    }else if(IsEqualGUID(&IID_IInstallEngine, riid)) {
        TRACE("(%p)->(IID_IInstallEngine %p)\n", This, ppv);
        *ppv = &This->IInstallEngine2_iface;
    }else if(IsEqualGUID(&IID_IInstallEngine2, riid)) {
        TRACE("(%p)->(IID_IInstallEngine2 %p)\n", This, ppv);
        *ppv = &This->IInstallEngine2_iface;
    }else if(IsEqualGUID(&IID_IInstallEngineTiming, riid)) {
        TRACE("(%p)->(IID_IInstallEngineTiming %p)\n", This, ppv);
        *ppv = &This->IInstallEngineTiming_iface;
    }else {
        FIXME("(%p)->(%s %p) not found\n", This, debugstr_guid(riid), ppv);
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*ppv);
    return S_OK;
}

static ULONG WINAPI InstallEngine_AddRef(IInstallEngine2 *iface)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    return ref;
}

static ULONG WINAPI InstallEngine_Release(IInstallEngine2 *iface)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    if(!ref)
        heap_free(This);

    return ref;
}

static HRESULT WINAPI InstallEngine_GetEngineStatus(IInstallEngine2 *iface, DWORD *status)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%p)\n", This, status);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_SetCifFile(IInstallEngine2 *iface, const char *cab_name, const char *cif_name)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%s %s)\n", This, debugstr_a(cab_name), debugstr_a(cif_name));
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_DownloadComponents(IInstallEngine2 *iface, DWORD flags)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%lx)\n", This, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_InstallComponents(IInstallEngine2 *iface, DWORD flags)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%lx)\n", This, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_EnumInstallIDs(IInstallEngine2 *iface, UINT index, char **id)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%d %p)\n", This, index, id);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_EnumDownloadIDs(IInstallEngine2 *iface, UINT index, char **id)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%d %p)\n", This, index, id);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_IsComponentInstalled(IInstallEngine2 *iface, const char *id, DWORD *status)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%s %p)\n", This, debugstr_a(id), status);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_RegisterInstallEngineCallback(IInstallEngine2 *iface, IInstallEngineCallback *callback)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%p)\n", This, callback);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_UnregisterInstallEngineCallback(IInstallEngine2 *iface)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_SetAction(IInstallEngine2 *iface, const char *id, DWORD action, DWORD priority)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%s %ld %ld)\n", This, debugstr_a(id), action, priority);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_GetSizes(IInstallEngine2 *iface, const char *id, COMPONENT_SIZES *sizes)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%s %p)\n", This, debugstr_a(id), sizes);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_LaunchExtraCommand(IInstallEngine2 *iface, const char *inf_name, const char *section)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%s %s)\n", This, debugstr_a(inf_name), debugstr_a(section));
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_GetDisplayName(IInstallEngine2 *iface, const char *id, const char *name)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%s %s)\n", This, debugstr_a(id), debugstr_a(name));
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_SetBaseUrl(IInstallEngine2 *iface, const char *base_name)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%s)\n", This, debugstr_a(base_name));
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_SetDownloadDir(IInstallEngine2 *iface, const char *download_dir)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%s)\n", This, debugstr_a(download_dir));
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_SetInstallDrive(IInstallEngine2 *iface, char drive)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%c)\n", This, drive);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_SetInstallOptions(IInstallEngine2 *iface, DWORD flags)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%lx)\n", This, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_SetHWND(IInstallEngine2 *iface, HWND hwnd)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%p)\n", This, hwnd);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_SetIStream(IInstallEngine2 *iface, IStream *stream)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%p)\n", This, stream);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_Abort(IInstallEngine2 *iface, DWORD flags)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%lx)\n", This, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_Suspend(IInstallEngine2 *iface)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine_Resume(IInstallEngine2 *iface)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine2_SetLocalCif(IInstallEngine2 *iface, const char *cif)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%s)\n", This, debugstr_a(cif));
    return E_NOTIMPL;
}

static HRESULT WINAPI InstallEngine2_GetICifFile(IInstallEngine2 *iface, ICifFile **cif_file)
{
    InstallEngine *This = impl_from_IInstallEngine2(iface);
    FIXME("(%p)->(%p)\n", This, cif_file);
    return E_NOTIMPL;
}

static const IInstallEngine2Vtbl InstallEngine2Vtbl = {
    InstallEngine_QueryInterface,
    InstallEngine_AddRef,
    InstallEngine_Release,
    InstallEngine_GetEngineStatus,
    InstallEngine_SetCifFile,
    InstallEngine_DownloadComponents,
    InstallEngine_InstallComponents,
    InstallEngine_EnumInstallIDs,
    InstallEngine_EnumDownloadIDs,
    InstallEngine_IsComponentInstalled,
    InstallEngine_RegisterInstallEngineCallback,
    InstallEngine_UnregisterInstallEngineCallback,
    InstallEngine_SetAction,
    InstallEngine_GetSizes,
    InstallEngine_LaunchExtraCommand,
    InstallEngine_GetDisplayName,
    InstallEngine_SetBaseUrl,
    InstallEngine_SetDownloadDir,
    InstallEngine_SetInstallDrive,
    InstallEngine_SetInstallOptions,
    InstallEngine_SetHWND,
    InstallEngine_SetIStream,
    InstallEngine_Abort,
    InstallEngine_Suspend,
    InstallEngine_Resume,
    InstallEngine2_SetLocalCif,
    InstallEngine2_GetICifFile
};

static HRESULT WINAPI InstallEngineTiming_QueryInterface(IInstallEngineTiming *iface, REFIID riid, void **ppv)
{
    InstallEngine *This = impl_from_IInstallEngineTiming(iface);
    return IInstallEngine2_QueryInterface(&This->IInstallEngine2_iface, riid, ppv);
}

static ULONG WINAPI InstallEngineTiming_AddRef(IInstallEngineTiming *iface)
{
    InstallEngine *This = impl_from_IInstallEngineTiming(iface);
    return IInstallEngine2_AddRef(&This->IInstallEngine2_iface);
}

static ULONG WINAPI InstallEngineTiming_Release(IInstallEngineTiming *iface)
{
    InstallEngine *This = impl_from_IInstallEngineTiming(iface);
    return IInstallEngine2_Release(&This->IInstallEngine2_iface);
}

static HRESULT WINAPI InstallEngineTiming_GetRates(IInstallEngineTiming *iface, DWORD *download, DWORD *install)
{
    InstallEngine *This = impl_from_IInstallEngineTiming(iface);

    FIXME("(%p)->(%p, %p): stub\n", This, download, install);

    *download = 0;
    *install = 0;

    return S_OK;
}

static HRESULT WINAPI InstallEngineTiming_GetInstallProgress(IInstallEngineTiming *iface, INSTALLPROGRESS *progress)
{
    InstallEngine *This = impl_from_IInstallEngineTiming(iface);
    ULONGLONG elapsed;
    static int once;

    if (!once)
        FIXME("(%p)->(%p): semi-stub\n", This, progress);
    else
        TRACE("(%p)->(%p): semi-stub\n", This, progress);

    progress->dwDownloadKBRemaining = max(This->thread.download_size, This->thread.downloaded_kb) - This->thread.downloaded_kb;

    elapsed = GetTickCount64() - This->thread.download_start;
    if (This->thread.download_start && This->thread.downloaded_kb && elapsed > 100)
        progress->dwDownloadSecsRemaining = (progress->dwDownloadKBRemaining * elapsed) / (This->thread.downloaded_kb * 1000);
    else
        progress->dwDownloadSecsRemaining = -1;

    progress->dwInstallKBRemaining = 0;
    progress->dwInstallSecsRemaining = -1;

    return S_OK;
}

static const IInstallEngineTimingVtbl InstallEngineTimingVtbl =
{
    InstallEngineTiming_QueryInterface,
    InstallEngineTiming_AddRef,
    InstallEngineTiming_Release,
    InstallEngineTiming_GetRates,
    InstallEngineTiming_GetInstallProgress,
};

static HRESULT WINAPI ClassFactory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    *ppv = NULL;

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        TRACE("(%p)->(IID_IUnknown %p)\n", iface, ppv);
        *ppv = iface;
    }else if(IsEqualGUID(&IID_IClassFactory, riid)) {
        TRACE("(%p)->(IID_IClassFactory %p)\n", iface, ppv);
        *ppv = iface;
    }

    if(*ppv) {
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    FIXME("(%p)->(%s %p)\n", iface, debugstr_guid(riid), ppv);
    return E_NOINTERFACE;
}

static ULONG WINAPI ClassFactory_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI ClassFactory_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI ClassFactory_LockServer(IClassFactory *iface, BOOL fLock)
{
    return S_OK;
}

static HRESULT WINAPI InstallEngineCF_CreateInstance(IClassFactory *iface, IUnknown *outer,
        REFIID riid, void **ppv)
{
    InstallEngine *engine;
    HRESULT hres;

    TRACE("(%p %s %p)\n", outer, debugstr_guid(riid), ppv);

    engine = heap_alloc_zero(sizeof(*engine));
    if(!engine)
        return E_OUTOFMEMORY;

    engine->IInstallEngine2_iface.lpVtbl = &InstallEngine2Vtbl;
    engine->IInstallEngineTiming_iface.lpVtbl = &InstallEngineTimingVtbl;
    engine->ref = 1;
    engine->status = ENGINESTATUS_NOTREADY;

    hres = IInstallEngine2_QueryInterface(&engine->IInstallEngine2_iface, riid, ppv);
    IInstallEngine2_Release(&engine->IInstallEngine2_iface);
    return hres;
}

static const IClassFactoryVtbl InstallEngineCFVtbl = {
    ClassFactory_QueryInterface,
    ClassFactory_AddRef,
    ClassFactory_Release,
    InstallEngineCF_CreateInstance,
    ClassFactory_LockServer
};

static IClassFactory InstallEngineCF = { &InstallEngineCFVtbl };

/***********************************************************************
 *             DllGetClassObject (INSENG.@)
 */
HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID iid, LPVOID *ppv)
{
    if(IsEqualGUID(rclsid, &CLSID_InstallEngine)) {
        TRACE("(CLSID_InstallEngine %s %p)\n", debugstr_guid(iid), ppv);
        return IClassFactory_QueryInterface(&InstallEngineCF, iid, ppv);
    }

    FIXME("(%s %s %p)\n", debugstr_guid(rclsid), debugstr_guid(iid), ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

BOOL WINAPI CheckTrustEx( LPVOID a, LPVOID b, LPVOID c, LPVOID d, LPVOID e )
{
    FIXME("%p %p %p %p %p\n", a, b, c, d, e );
    return TRUE;
}

/***********************************************************************
 *  DllInstall (INSENG.@)
 */
HRESULT WINAPI DllInstall(BOOL bInstall, LPCWSTR cmdline)
{
    FIXME("(%s, %s): stub\n", bInstall ? "TRUE" : "FALSE", debugstr_w(cmdline));
    return S_OK;
}
