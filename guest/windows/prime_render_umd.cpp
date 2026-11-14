#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <d3d10umddi.h>
#include "../../src/c08_wddm_protocol.h"

namespace {
constexpr UINT64 supported_ddi = D3DWDDM1_3_DDI_SUPPORTED;

SIZE_T APIENTRY device_size(D3D10DDI_HADAPTER, const D3D10DDIARG_CALCPRIVATEDEVICESIZE*) {
    return 0;
}

HRESULT APIENTRY create_device(D3D10DDI_HADAPTER, D3D10DDIARG_CREATEDEVICE*) {
    // The transport probe has no Direct3D resource or shader implementation.
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT APIENTRY close_adapter(D3D10DDI_HADAPTER) {
    return S_OK;
}

HRESULT APIENTRY versions(D3D10DDI_HADAPTER, UINT32* count, UINT64* out) {
    if (!count) return E_INVALIDARG;
    if (out && !*count) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    *count = 1;
    if (out) *out = supported_ddi;
    return S_OK;
}

HRESULT APIENTRY caps(D3D10DDI_HADAPTER, const D3D10_2DDIARG_GETCAPS* args) {
    if (!args || !args->pData) return E_INVALIDARG;
    if (args->Type != D3D11DDICAPS_3DPIPELINESUPPORT) return E_NOTIMPL;
    if (args->DataSize != sizeof(D3D11DDI_3DPIPELINESUPPORT_CAPS)) return E_INVALIDARG;
    static_cast<D3D11DDI_3DPIPELINESUPPORT_CAPS*>(args->pData)->Caps = 0;
    return S_OK;
}
}

extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER* args) {
    if (!args || !args->pAdapterFuncs_2 || !args->pAdapterCallbacks ||
        !args->pAdapterCallbacks->pfnQueryAdapterInfoCb) return E_INVALIDARG;
    RwGpuWddmAdapter adapter{};
    D3DDDICB_QUERYADAPTERINFO query{&adapter, sizeof(adapter)};
    const HRESULT status = args->pAdapterCallbacks->pfnQueryAdapterInfoCb(args->hRTAdapter.handle, &query);
    if (FAILED(status)) return status;
    if (adapter.header.magic != RWGPU_WDDM_MAGIC || adapter.header.version != RWGPU_WDDM_ABI ||
        adapter.header.command != RWGPU_WDDM_QUERY || adapter.header.bytes != sizeof(adapter) ||
        adapter.pci_id != ((RWGPU_DEVICE_ID << 16) | RWGPU_VENDOR_ID) ||
        adapter.logical_vram_bytes != RWGPU_LOGICAL_BYTES) return DXGI_ERROR_UNSUPPORTED;

    const D3D10_2DDI_ADAPTERFUNCS callbacks{device_size, create_device, close_adapter, versions, caps};
    *args->pAdapterFuncs_2 = callbacks;
    args->hAdapter.pDrvPrivate = args->hRTAdapter.handle;
    return S_OK;
}
