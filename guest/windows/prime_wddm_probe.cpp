#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dkmthk.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "../../src/c08_wddm_protocol.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;

ComPtr<IDXGIAdapter1> find_prime() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(i, &adapter))) return {};
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == RWGPU_VENDOR_ID &&
            desc.DeviceId == RWGPU_DEVICE_ID && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            return adapter;
    }
}

bool escape(D3DKMT_HANDLE adapter, void* packet, UINT bytes) {
    D3DKMT_ESCAPE request{};
    request.hAdapter = adapter;
    request.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    request.Flags.HardwareAccess = 1;
    request.pPrivateDriverData = packet;
    request.PrivateDriverDataSize = bytes;
    const auto status = D3DKMTEscape(&request);
    if (status == 0) return true;
    std::fprintf(stderr, "PrimeVGC WDDM escape failed (0x%08lx)\n", static_cast<ULONG>(status));
    return false;
}
} // namespace

int main() {
    auto dxgi = find_prime();
    if (!dxgi) {
        std::fprintf(stderr, "PRIME_C08_WDDM_BRIDGE_ADAPTER_ABSENT pci=1b36:c083\n");
        return 3;
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(dxgi->GetDesc1(&desc))) return 1;
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = desc.AdapterLuid;
    if (D3DKMTOpenAdapterFromLuid(&open) != 0) return 1;
    struct AdapterHandle {
        D3DKMT_HANDLE handle;
        ~AdapterHandle() { D3DKMT_CLOSEADAPTER close{handle}; D3DKMTCloseAdapter(&close); }
    } adapter{open.hAdapter};

    RwGpuWddmAdapter info{{RWGPU_WDDM_MAGIC, RWGPU_WDDM_ABI, RWGPU_WDDM_QUERY, sizeof(info)}};
    constexpr auto caps = RWGPU_CAP_DRAW_RAM | RWGPU_CAP_COLOR_DMA;
    if (!escape(adapter.handle, &info, sizeof(info)) ||
        info.pci_id != ((RWGPU_DEVICE_ID << 16) | RWGPU_VENDOR_ID) ||
        info.logical_vram_bytes != RWGPU_LOGICAL_BYTES || (info.capabilities & caps) != caps)
        return 1;

    constexpr unsigned width = 96, height = 64;
    constexpr std::uint32_t red = 0xff0000ff, green = 0xff00ff00, clear = 0xff1e140a;
    const std::array<RwGpuVertex, 6> vertices{{
        {-.7f,-.55f,.2f,1,red}, {.7f,-.55f,.2f,1,red}, {0,.65f,.2f,1,red},
        {-.8f,-.7f,.8f,1,green}, {.8f,-.7f,.8f,1,green}, {0,.85f,.8f,1,green}
    }};
    constexpr UINT bytes = sizeof(RwGpuWddmFrame) + sizeof(vertices) + width * height * 4;
    const RwGpuWddmFrame frame{{RWGPU_WDDM_MAGIC, RWGPU_WDDM_ABI, RWGPU_WDDM_DRAW, bytes},
                              width, height, clear, static_cast<std::uint32_t>(vertices.size())};
    std::vector<std::uint8_t> data(bytes);
    std::memcpy(data.data(), &frame, sizeof(frame));
    std::memcpy(data.data() + sizeof(frame), vertices.data(), sizeof(vertices));
    if (!escape(adapter.handle, data.data(), bytes)) return 1;

    const std::uint8_t* pixels = data.data() + sizeof(frame) + sizeof(vertices);
    const auto color = [&](unsigned x, unsigned y) {
        const auto* p = pixels + (y * width + x) * 4;
        return 0xff000000U | (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2];
    };
    if (color(width / 2, height / 2) != red || color(width / 2, 7) != green ||
        color(width - 1, height - 1) != clear) {
        std::fprintf(stderr, "PrimeVGC WDDM bridge returned unexpected BGRX pixels\n");
        return 1;
    }
    std::printf("PRIME_C08_WDDM_BRIDGE_PASS pci=1b36:c083 vram=%llu pixels=%u\n",
                static_cast<unsigned long long>(info.logical_vram_bytes), width * height);
}
