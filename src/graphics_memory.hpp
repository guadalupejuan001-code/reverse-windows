#pragma once

#include "c08_gpu_protocol.h"
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

namespace reverse_windows {

class GpuVram {
 public:
  static constexpr std::uint64_t kBytes = RWGPU_LOGICAL_BYTES;
  static constexpr std::uint64_t kLegacyBackingBytes =
      ((kBytes + QssdDisk::kGiB - 1) / QssdDisk::kGiB) * QssdDisk::kGiB;
  static constexpr std::size_t kPageBytes = QssdDisk::kBlockSize;
  static constexpr std::size_t kTransferBytes = QssdDisk::kMaxBulkTransactionBytes;
  static constexpr std::size_t kResidentBytes = 8U * 1024U * 1024U;
  static_assert(kBytes % kPageBytes == 0 && kResidentBytes % kPageBytes == 0 &&
                kPageBytes == RWGPU_PAGE_BYTES && kTransferBytes == RWGPU_DIRECT_TRANSFER_BYTES);

  enum class Result { Complete, InvalidRange, Unavailable };

  static bool valid_range(std::uint64_t offset, std::uint64_t size) noexcept {
    return offset <= kBytes && size <= kBytes - offset;
  }

  explicit GpuVram(std::filesystem::path path) : path_(std::move(path)) {}

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

  void reset() noexcept { resident_.reset(); }

  void set_cache_enabled(bool enabled) noexcept {
    cache_enabled_ = enabled;
    if (!enabled) resident_.reset();
  }

  std::size_t resident_bytes() const noexcept { return resident_ ? kResidentBytes : 0; }
  std::uint64_t commits() const { return disk_ ? disk_->stats().commits : 0; }

  std::uint64_t journal_bytes() const {
    return disk_ ? disk_->stats().physical_bytes : 0;
  }

 private:
  std::filesystem::path path_;
  std::unique_ptr<QssdDisk> disk_;
  std::unique_ptr<std::uint8_t[]> resident_;
  std::array<std::uint64_t, kResidentBytes / kPageBytes> tags_{};
  bool cache_enabled_ = true;

  QssdDisk& volume() {
    if (!disk_) {
      try {
        disk_ = std::make_unique<QssdDisk>(path_);
      } catch (const std::system_error& error) {
        if (error.code() != std::errc::no_such_file_or_directory) throw;
        try {
          QssdDisk::create_bytes(path_, kBytes);
        } catch (const std::system_error& create_error) {
          if (create_error.code() != std::errc::file_exists) throw;
        }
        disk_ = std::make_unique<QssdDisk>(path_);
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
