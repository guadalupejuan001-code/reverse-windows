#include "sparse_ram_selected.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using cosmic_pc::SparseRam;
constexpr auto kPage = SparseRam::kPageSize;
constexpr auto kMiB = std::uint64_t{1} << 20;
using Page = std::array<std::uint8_t, kPage>;

struct RamBacking final : SparseRam::PageBacking {
    std::vector<Page> pages;
    std::uint64_t read_calls = 0, write_calls = 0;
    explicit RamBacking(std::size_t count) : pages(count) {}
    void read(std::uint64_t page, std::uint8_t* bytes) override {
        ++read_calls;
        std::memcpy(bytes, pages.at(page).data(), kPage);
    }
    void write(std::uint64_t page, const std::uint8_t* bytes) override {
        ++write_calls;
        std::memcpy(pages.at(page).data(), bytes, kPage);
    }
    void reset() noexcept override {}
};

Page contents(std::uint64_t page) {
    Page bytes;
    std::uint32_t state = static_cast<std::uint32_t>(page + 1);
    for (auto& byte : bytes) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<std::uint8_t>(state);
    }
    return bytes;
}

std::uint64_t value_at(const SparseRam& ram, std::uint64_t page, std::uint64_t iteration) {
    std::uint64_t value;
    const auto offset = ((iteration >> 7) & 7) * 64;
    ram.read_block(page * kPage + offset, reinterpret_cast<std::uint8_t*>(&value), sizeof(value));
    return value;
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::invalid_argument("case iterations");
        const std::string mode = argv[1];
        const auto iterations = std::stoull(argv[2]);
        if (!iterations || (mode != "resident" && mode != "hit" && mode != "miss" && mode != "resize"))
            throw std::invalid_argument("Unknown case or zero iterations");
        const auto count = mode == "hit" ? 32U : mode == "resize" ? 64U : 128U;
        SparseRam ram(256 * kPage, 32 * kMiB);
        auto backing = std::make_unique<RamBacking>(256);
        auto* store = backing.get();
        ram.attach_page_backing(std::move(backing), SparseRam::BackingCachePolicy::Adaptive);
#ifdef ENABLE_TIMING
        ram.set_backing_read_timing_enabled(true);
#endif
        for (std::uint64_t page = 0; page < count; ++page) {
            const auto bytes = contents(page);
            ram.write_block(page * kPage, bytes.data(), bytes.size());
        }
        if (mode != "resident") {
            if (ram.adjust_payload_budget(0, count) != SparseRam::BudgetAdjustment::Applied ||
                ram.adjust_payload_budget(32 * kMiB, 0) != SparseRam::BudgetAdjustment::Applied)
                throw std::runtime_error("Could not initialize spilled fixture");
        }
        for (std::uint64_t page = 0; page < count; ++page) (void)value_at(ram, page, 0);
        const auto before = ram.metrics();
        const auto read_calls = store->read_calls;
        const auto write_calls = store->write_calls;
#ifdef HAS_PRESSURE
        const auto pressure_before = ram.pressure_metrics();
#endif
        std::uint64_t checksum = 0;
        const auto cpu_started = std::clock();
        const auto started = std::chrono::steady_clock::now();
        if (mode == "resize") {
            for (std::uint64_t i = 0; i < iterations; ++i) {
                const auto budget = ((i & 1) ? 32 : 64) * kMiB;
                if (ram.adjust_payload_budget(budget, 0) != SparseRam::BudgetAdjustment::Applied)
                    throw std::runtime_error("Resize did not apply");
                for (std::uint64_t page = 0; page < count; ++page)
                    checksum += value_at(ram, page, i) ^ (i * count + page);
            }
        } else {
            const auto mask = count - 1;
            for (std::uint64_t i = 0; i < iterations; ++i)
                checksum += value_at(ram, i & mask, i) ^ i;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        const auto cpu_elapsed = std::clock() - cpu_started;
        const auto after = ram.metrics();
        const auto operations = iterations * (mode == "resize" ? count : 1);
        const auto transfers = store->read_calls - read_calls;
        if (store->write_calls != write_calls || after.allocated_bytes + after.backing_cache_capacity_bytes > after.budget_bytes)
            throw std::runtime_error("Unexpected writes or exceeded resident budget");
        if ((mode == "resident" || mode == "hit") && transfers != 0)
            throw std::runtime_error("Expected resident or cached accesses");
        if (mode == "miss" && transfers != operations)
            throw std::runtime_error("The miss stream was not entirely cold");
        std::cout << "{\"case\":\"" << mode << "\",\"iterations\":" << iterations
                  << ",\"operations\":" << operations << ",\"wall_ns\":" << elapsed
                  << ",\"cpu_ns\":" << static_cast<std::uint64_t>(1e9 * cpu_elapsed / CLOCKS_PER_SEC)
                  << ",\"checksum\":" << checksum << ",\"backing_reads\":" << transfers
                  << ",\"cache_hits\":" << after.backing_cache_hits - before.backing_cache_hits
                  << ",\"cache_bytes\":" << after.backing_cache_bytes;
#ifdef HAS_PRESSURE
        const auto pressure = ram.pressure_metrics();
        std::cout << ",\"backing_attempts\":" << pressure.backing_read_attempts - pressure_before.backing_read_attempts
                  << ",\"read_ns\":" << pressure.backing_read_ns - pressure_before.backing_read_ns
                  << ",\"lock_wait_ns\":" << pressure.backing_lock_wait_ns - pressure_before.backing_lock_wait_ns;
#ifdef HAS_TIMING_FLAG
        std::cout << ",\"timing_enabled\":" << (pressure.backing_timing_enabled ? "true" : "false")
                  << ",\"timed_attempts\":" << pressure.backing_timed_attempts - pressure_before.backing_timed_attempts;
#endif
#endif
        std::cout << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
