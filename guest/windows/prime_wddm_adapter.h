#pragma once
#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
typedef _Return_type_success_(return >= 0) LONG NTSTATUS;
#include <d3dkmthk.h>
#include "../../src/c08_wddm_protocol.h"
#include <cstdio>
#include <stdexcept>

namespace prime_wddm {
inline Microsoft::WRL::ComPtr<IDXGIAdapter1> find_adapter() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) throw std::runtime_error("create DXGI factory");
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const auto result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) return {};
        if (FAILED(result)) throw std::runtime_error("enumerate DXGI adapters");
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) throw std::runtime_error("read DXGI adapter");
        if (desc.VendorId == RWGPU_VENDOR_ID && desc.DeviceId == RWGPU_DEVICE_ID &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return adapter;
    }
}

class Adapter {
public:
    explicit Adapter(IDXGIAdapter1& dxgi) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(dxgi.GetDesc1(&desc))) throw std::runtime_error("read PrimeVGC DXGI identity");
        D3DKMT_OPENADAPTERFROMLUID open{};
        open.AdapterLuid = desc.AdapterLuid;
        if (D3DKMTOpenAdapterFromLuid(&open) != 0) throw std::runtime_error("open PrimeVGC LUID");
        handle_ = open.hAdapter;
    }
    ~Adapter() { D3DKMT_CLOSEADAPTER close{handle_}; D3DKMTCloseAdapter(&close); }
    Adapter(const Adapter&) = delete;
    Adapter& operator=(const Adapter&) = delete;

    RwGpuWddmAdapter query() const {
        RwGpuWddmAdapter info{};
        info.header = {RWGPU_WDDM_MAGIC, RWGPU_WDDM_ABI, RWGPU_WDDM_QUERY, sizeof(info)};
        D3DKMT_ESCAPE request{};
        request.hAdapter = handle_;
        request.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
        request.Flags.HardwareAccess = 1;
        request.pPrivateDriverData = &info;
        request.PrivateDriverDataSize = sizeof(info);
        const auto status = D3DKMTEscape(&request);
        if (status != 0 || info.header.magic != RWGPU_WDDM_MAGIC ||
            info.header.version != RWGPU_WDDM_ABI || info.header.command != RWGPU_WDDM_QUERY ||
            info.header.bytes != sizeof(info) || info.pci_id != ((RWGPU_DEVICE_ID << 16) | RWGPU_VENDOR_ID) ||
            info.logical_vram_bytes != RWGPU_LOGICAL_BYTES || !(info.capabilities & RWGPU_CAP_WDDM_SCHEDULER))
            throw std::runtime_error("validate PrimeVGC ABI and VRAM");
        return info;
    }
private:
    D3DKMT_HANDLE handle_ = 0;
};
} // namespace prime_wddm
