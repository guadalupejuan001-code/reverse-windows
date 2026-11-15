#pragma once

#include "qssd/transient_pages.hpp"
#include "sparse_ram.hpp"

namespace reverse_windows {

class AndroidRamBacking final : public cosmic_pc::SparseRam::PageBacking {
 public:
  static constexpr auto kPageSize = qssd::TransientPages::kPageSize;
  static_assert(kPageSize == cosmic_pc::SparseRam::kPageSize);

  static std::uint64_t spill_budget(const std::filesystem::path& directory) {
    return qssd::TransientPages::spill_budget(directory);
  }

  AndroidRamBacking(const std::filesystem::path& directory, std::uint64_t logical_bytes,
                    std::uint64_t spill_bytes)
      : pages_(directory, logical_bytes, spill_bytes) {}

  void read(std::uint64_t index, std::uint8_t* page) override { pages_.read(index, page); }
  void write(std::uint64_t index, const std::uint8_t* page) override { pages_.write(index, page); }
  void reset() noexcept override { pages_.reset(); }

  std::uint64_t file_payload_bytes() const noexcept { return pages_.file_payload_bytes(); }
  std::uint64_t read_us() const noexcept { return pages_.read_us(); }
  std::uint64_t write_us() const noexcept { return pages_.write_us(); }

 private:
  qssd::TransientPages pages_;
};

} // namespace reverse_windows
