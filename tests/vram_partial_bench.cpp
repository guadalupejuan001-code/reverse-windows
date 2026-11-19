#include "graphics_memory.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using reverse_windows::GpuVram;
using Result = GpuVram::Result;

int main() {
  try {
    auto name = (std::filesystem::temp_directory_path() / "prime-partial-vram-XXXXXX").string();
    if (!::mkdtemp(name.data())) throw std::runtime_error("temporary VRAM directory");
    struct Cleanup {
      std::filesystem::path directory;
      ~Cleanup() { std::error_code ec; std::filesystem::remove_all(directory, ec); }
    } cleanup{name};
    GpuVram vram(cleanup.directory / "partial.rqvd");
    constexpr unsigned pages = 32, reads = 200000;
    std::array<std::array<std::uint8_t, 37>, pages> data;
    for (unsigned page = 0; page < pages; ++page) {
      for (unsigned i = 0; i < data[page].size(); ++i) data[page][i] = (page * 23 + i) & 255;
      if (vram.write(page * GpuVram::kPageBytes + 19, data[page].data(), data[page].size()) != Result::Complete)
        throw std::runtime_error("partial VRAM write failed");
    }
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t checksum = 0;
    for (unsigned i = 0; i < reads; ++i) {
      const auto page = i % pages, at = i % 34;
      std::array<std::uint8_t, 4> word;
      if (vram.read(page * GpuVram::kPageBytes + 19 + at, word.data(), word.size()) != Result::Complete ||
          !std::equal(word.begin(), word.end(), data[page].begin() + at))
        throw std::runtime_error("partial VRAM read returned different bytes");
      checksum += word[0] + word[1] + word[2] + word[3];
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "PRIME_PARTIAL_VRAM_PASS reads=" << reads << " checksum=" << checksum << " elapsed_ns=" << ns;
#ifndef PRIME_R102_BASELINE
    if (vram.read_hits() != reads || vram.backend_reads() || vram.resident_bytes() != pages * GpuVram::kPageBytes ||
        vram.cached_payload_bytes() != pages * 37) throw std::runtime_error("partial VRAM accounting");
    std::cout << " hits=" << vram.read_hits() << " backend_reads=" << vram.backend_reads()
              << " resident_bytes=" << vram.resident_bytes() << " cached_bytes=" << vram.cached_payload_bytes();
    // An aliased page, a disjoint interval and a gap must never expose stale bytes.
    std::uint8_t byte = 0xe7;
    if (vram.write(GpuVram::kResidentBytes + 90, &byte, 1) != Result::Complete ||
        vram.resident_read(19, 1) || vram.resident_read(GpuVram::kResidentBytes + 89, 2) ||
        !vram.resident_read(GpuVram::kResidentBytes + 90, 1) ||
        vram.read(19, &byte, 1) != Result::Complete || byte != data[0][0])
      throw std::runtime_error("alias or uninitialized byte returned from VRAM cache");
    byte = 0x7c;
    if (vram.write(80, &byte, 1) != Result::Complete || vram.resident_read(19, 62) ||
        vram.read(79, &byte, 1) != Result::Complete || byte)
      throw std::runtime_error("disjoint VRAM ranges exposed an unknown byte");
    vram.set_cache_enabled(false);
    if (vram.resident_bytes() || vram.cached_payload_bytes() || vram.cache_reserved_bytes())
      throw std::runtime_error("disabled cache still holds VRAM");
#endif
    std::cout << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
