#define WIN32_LEAN_AND_MEAN
#include "prime_wddm_adapter.h"
#include <cstdio>

int main() try {
    const auto dxgi = prime_wddm::find_adapter();
    if (!dxgi) {
        std::fprintf(stderr, "PRIME_C08_WDDM_BRIDGE_ADAPTER_ABSENT pci=1b36:c083\n");
        return 3;
    }
    const prime_wddm::Adapter adapter(*dxgi.Get());
    const auto info = adapter.query();

    std::printf("PRIME_C08_WDDM_DISCOVERED pci=1b36:c083 vram=%llu scheduled_ticket=%u\n",
                static_cast<unsigned long long>(info.logical_vram_bytes), info.last_scheduled_draw_ticket);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
