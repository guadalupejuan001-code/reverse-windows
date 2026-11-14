#define WIN32_LEAN_AND_MEAN
#include "prime_wddm_adapter.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

int main() try {
    const auto dxgi = prime_wddm::find_adapter();
    if (!dxgi) {
        std::fprintf(stderr, "PRIME_C08_WDDM_BRIDGE_ADAPTER_ABSENT pci=1b36:c083\n");
        return 3;
    }
    const prime_wddm::Adapter adapter(*dxgi.Get());
    const auto info = adapter.query();

    constexpr unsigned width = 96, height = 64;
    constexpr std::uint32_t red = 0x800000ff, green = 0xff00ff00, clear = 0x741e140a;
    const std::array<RwGpuVertex, 6> vertices{{
        {-.7f,-.55f,.2f,1,red}, {.7f,-.55f,.2f,1,red}, {0,.65f,.2f,1,red},
        {-.8f,-.7f,.8f,1,green}, {.8f,-.7f,.8f,1,green}, {0,.85f,.8f,1,green}
    }};
    constexpr UINT bytes = sizeof(RwGpuWddmFrame) + sizeof(vertices) + width * height * 4;
    const RwGpuWddmFrame frame{{RWGPU_WDDM_MAGIC, RWGPU_WDDM_ABI, RWGPU_WDDM_DRAW, bytes},
                              width, height, clear, static_cast<std::uint32_t>(vertices.size()), 0};
    std::vector<std::uint8_t> data(bytes);
    std::memcpy(data.data(), &frame, sizeof(frame));
    std::memcpy(data.data() + sizeof(frame), vertices.data(), sizeof(vertices));
    if (!adapter.escape(data.data(), bytes)) return 1;
    RwGpuWddmFrame completed{};
    std::memcpy(&completed, data.data(), sizeof(completed));
    if (!completed.draw_ticket || completed.draw_ticket != adapter.query().last_draw_ticket) return 1;

    const std::uint8_t* pixels = data.data() + sizeof(frame) + sizeof(vertices);
    const auto color = [&](unsigned x, unsigned y) {
        const auto* p = pixels + (y * width + x) * 4;
        return (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2];
    };
    if (color(width / 2, height / 2) != red || color(width / 2, 7) != green ||
        color(width - 1, height - 1) != clear) {
        std::fprintf(stderr, "PrimeVGC WDDM bridge returned unexpected BGRA pixels\n");
        return 1;
    }
    std::printf("PRIME_C08_WDDM_BRIDGE_PASS pci=1b36:c083 vram=%llu ticket=%u samples=3\n",
                static_cast<unsigned long long>(info.logical_vram_bytes), completed.draw_ticket);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
