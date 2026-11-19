#pragma once

#include "c08_gpu_protocol.h"
#include "graphics_page_residency.hpp"
#include "qssd_disk.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace reverse_windows {

class GpuVram {
 public:
  static constexpr std::uint64_t kBytes = RWGPU_LOGICAL_BYTES;
  static constexpr std::uint64_t kLegacyBackingBytes =
      ((kBytes + QssdDisk::kGiB - 1) / QssdDisk::kGiB) * QssdDisk::kGiB;
  static constexpr std::size_t kPageBytes = QssdDisk::kBlockSize;
  static constexpr std::size_t kTransferBytes = QssdDisk::kMaxBulkTransactionBytes;
  static constexpr std::size_t kResidentBytes = 8U * 1024U * 1024U;
  static constexpr std::size_t kDrawResidentPages = 128;
  static constexpr std::size_t kDrawResidentBytes = kDrawResidentPages * kPageBytes;
  static_assert(kBytes % kPageBytes == 0 && kResidentBytes % kPageBytes == 0 &&
                kPageBytes == RWGPU_PAGE_BYTES && kTransferBytes == RWGPU_DIRECT_TRANSFER_BYTES);

  enum class Result { Complete, InvalidRange, Unavailable };

  static bool valid_range(std::uint64_t offset, std::uint64_t size) noexcept {
    return offset <= kBytes && size <= kBytes - offset;
  }

  explicit GpuVram(std::filesystem::path path, std::shared_ptr<SmpWorkers> workers = {})
      : path_(std::move(path)), bulk_workers_(std::move(workers)) {}

  // A view stays valid until this device's next cache update or reset.
  // The device and guest RAM are accessed by the same VM thread.
  const std::uint8_t* resident_read(std::uint64_t offset, std::size_t size) const noexcept {
    if (!size || !valid_range(offset, size)) return nullptr;
    const auto position = std::size_t(offset % kResidentBytes);
    return resident_range(offset, position, size) ? resident_.get() + position : nullptr;
  }

  Result read(std::uint64_t offset, std::uint8_t* target, std::size_t size) {
    if (!valid_range(offset, size) || (!target && size)) return Result::InvalidRange;
    if (!size) return Result::Complete;
    try {
      if (const auto* data = resident_read(offset, size)) {
        std::memcpy(target, data, size);
        return Result::Complete;
      }
      if (!volume().read_bytes(offset, target, size)) return Result::Unavailable;
      update_resident(offset, target, size);
      return Result::Complete;
    } catch (const std::exception&) {
      return Result::Unavailable;
    }
  }

  // The VM serializes reads and writes to its VRAM volume. A draw copies
  // geometry to the renderer before completing the CPU staging fence.
  Result read_draw(std::uint64_t offset, std::uint8_t* target, std::size_t size) {
    if (!valid_range(offset, size) || (!target && size)) return Result::InvalidRange;
    if (!size) return Result::Complete;
    const auto first = offset / kPageBytes;
    const auto count = (offset + size - 1) / kPageBytes - first + 1;
    if (!cache_enabled_ || count > kDrawResidentPages) return read(offset, target, size);
    try {
      auto& disk = volume();
      std::array<std::uint64_t, kDrawResidentPages> versions;
      if (!disk.block_versions(first, count, versions.data())) return Result::Unavailable;
      if (!draw_) draw_ = std::make_unique<DrawCache>();
      auto& cache = *draw_;
      cache.requests.clear();
      for (std::uint32_t index = 0; index < count; ++index)
        cache.requests.push_back({index, {first + index, versions[index]}});
      const auto* frame = cache.residency.prepare_frame(cache.requests, cache.fence, cache.fence + 1);
      if (!frame) return read(offset, target, size);
      for (const auto& upload : frame->uploads) {
        auto* page = cache.bytes.data() + upload.slot * kPageBytes;
        const auto address = upload.page.page * kPageBytes;
        if (const auto* bytes = resident_read(address, kPageBytes)) {
          std::memcpy(page, bytes, kPageBytes);
        } else if (!disk.read_bytes(address, page, kPageBytes)) {
          cache.residency.discard_frame(false);
          return Result::Unavailable;
        }
      }
      for (const auto& binding : frame->bindings) {
        const auto address = binding.page.page * kPageBytes;
        const auto start = std::max(offset, address);
        const auto end = std::min(offset + size, address + kPageBytes);
        std::memcpy(target + (start - offset),
                    cache.bytes.data() + binding.slot * kPageBytes + (start - address), end - start);
      }
      const auto uploaded = frame->uploads.size();
      cache.residency.submit_frame();
      ++cache.fence;
      draw_page_uploads_ += uploaded;
      draw_page_reuses_ += count - uploaded;
      return Result::Complete;
    } catch (const std::exception&) {
      draw_.reset();
      return read(offset, target, size);
    }
  }

  Result write(std::uint64_t offset, const std::uint8_t* source, std::size_t size) {
    if (!valid_range(offset, size) || size > kTransferBytes - offset % kPageBytes ||
        (!source && size)) return Result::InvalidRange;
    if (!size) return Result::Complete;
    try {
      if (!volume().write_bytes(offset, size, source)) return Result::Unavailable;
      update_resident(offset, source, size);
      return Result::Complete;
    } catch (const std::exception&) {
      return Result::Unavailable;
    }
  }

  void reset() noexcept {
    resident_.reset();
    draw_.reset();
    draw_page_uploads_ = draw_page_reuses_ = 0;
  }

  void set_cache_enabled(bool enabled) noexcept {
    cache_enabled_ = enabled;
    if (!enabled) {
      resident_.reset();
      draw_.reset();
    }
  }

  std::size_t resident_bytes() const noexcept { return (resident_ ? kResidentBytes : 0) + draw_resident_bytes(); }
  std::size_t draw_resident_bytes() const noexcept { return draw_ ? kDrawResidentBytes : 0; }
  std::uint64_t draw_page_uploads() const noexcept { return draw_page_uploads_; }
  std::uint64_t draw_page_reuses() const noexcept { return draw_page_reuses_; }
  QssdDisk::ParallelIoStats parallel_io_stats() const noexcept {
    return disk_ ? disk_->parallel_io_stats() : QssdDisk::ParallelIoStats{};
  }
  std::uint64_t commits() const { return disk_ ? disk_->stats().commits : 0; }

  std::uint64_t journal_bytes() const {
    return disk_ ? disk_->stats().physical_bytes : 0;
  }

 private:
  struct DrawCache {
    xgpu::GraphicsPageResidency residency{kDrawResidentPages};
    std::array<std::uint8_t, kDrawResidentBytes> bytes;
    std::vector<xgpu::GraphicsPageRequest> requests;
    std::uint64_t fence = 0;
  };
  std::filesystem::path path_;
  std::shared_ptr<SmpWorkers> bulk_workers_;
  std::unique_ptr<QssdDisk> disk_;
  std::unique_ptr<std::uint8_t[]> resident_;
  std::array<std::uint64_t, kResidentBytes / kPageBytes> tags_{};
  std::unique_ptr<DrawCache> draw_;
  std::uint64_t draw_page_uploads_ = 0, draw_page_reuses_ = 0;
  bool cache_enabled_ = true;

  QssdDisk& volume() {
    if (!disk_) {
      try {
        disk_ = std::make_unique<QssdDisk>(path_, QssdDisk::Access::ReadWrite, bulk_workers_);
      } catch (const std::system_error& error) {
        if (error.code() != std::errc::no_such_file_or_directory) throw;
        try {
          QssdDisk::create_bytes(path_, kBytes);
        } catch (const std::system_error& create_error) {
          if (create_error.code() != std::errc::file_exists) throw;
        }
        disk_ = std::make_unique<QssdDisk>(path_, QssdDisk::Access::ReadWrite, bulk_workers_);
      }
      // Early C08 volumes rounded the file capacity to 112 GiB. All commands
      // still enforce the public 120,000,000,000-byte boundary.
      if (disk_->bytes() != kBytes && disk_->bytes() != kLegacyBackingBytes) {
        disk_.reset();
        throw std::runtime_error("Capacidad incompatible del respaldo de VRAM.");
      }
    }
    return *disk_;
  }

  bool resident_range(std::uint64_t offset, std::size_t position, std::size_t size) const noexcept {
    if (!resident_ || size > kResidentBytes - position) return false;
    auto page = offset - offset % kPageBytes;
    const auto last = (position + size - 1) / kPageBytes;
    for (auto index = position / kPageBytes; index <= last; ++index, page += kPageBytes)
      if (tags_[index] != page) return false;
    return true;
  }

  void update_resident(std::uint64_t offset, const std::uint8_t* source, std::size_t size) noexcept {
    if (!cache_enabled_) return;
    if (!resident_) {
      resident_.reset(new (std::nothrow) std::uint8_t[kResidentBytes]);
      if (!resident_) return;
      tags_.fill(kBytes);
    }
    while (size) {
      const auto inside = std::size_t(offset % kPageBytes);
      const auto count = std::min(size, kPageBytes - inside);
      const auto page = offset - inside;
      const auto index = std::size_t(page % kResidentBytes);
      auto& tag = tags_[index / kPageBytes];
      if (count == kPageBytes || tag == page) {
        std::memcpy(resident_.get() + index + inside, source, count);
        tag = page;
      }
      offset += count;
      source += count;
      size -= count;
    }
  }
};

} // namespace reverse_windows
