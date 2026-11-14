#pragma once

#include "c08_gpu_protocol.h"
#include "qssd_disk.hpp"
#include <filesystem>
#include <memory>

namespace reverse_windows {

// The user's exact sparse QSSD VRAM profile. CPU staging and GPU-resident
// vertex buffers have their own much smaller allocation and lifetimes.
class GraphicsMemory {
 public:
  static constexpr std::uint64_t logical_bytes = RWGPU_LOGICAL_BYTES;
  static constexpr std::uint64_t page_bytes = QssdDisk::kBlockSize;
  static constexpr std::uint64_t backing_gib = (logical_bytes + QssdDisk::kGiB - 1) / QssdDisk::kGiB;
  static_assert(page_bytes == RWGPU_PAGE_BYTES && logical_bytes % page_bytes == 0);

  explicit GraphicsMemory(std::filesystem::path path) : path_(std::move(path)) {}
  bool available() const noexcept { return !path_.empty(); }
  bool read(std::uint64_t offset, std::uint8_t* bytes, unsigned count) {
    if (!valid(offset, count)) return false;
    return disk().read_bytes(offset, bytes, count);
  }
  bool write(std::uint64_t offset, const std::uint8_t* bytes, unsigned count) {
    if (!valid(offset, count)) return false;
    return disk().write(offset / QssdDisk::kSectorSize, count / QssdDisk::kSectorSize, bytes);
  }
  std::uint64_t backing_bytes() const { return disk_ ? disk_->stats().physical_bytes : 0; }

 private:
  std::filesystem::path path_;
  std::unique_ptr<QssdDisk> disk_;

  static bool valid(std::uint64_t offset, unsigned count) noexcept {
    return count == page_bytes && offset % page_bytes == 0 && offset <= logical_bytes - page_bytes;
  }
  QssdDisk& disk() {
    if (!disk_) {
      if (!std::filesystem::exists(path_)) {
        try { QssdDisk::create(path_, backing_gib); }
        catch (const std::system_error& err) {
          if (err.code() != std::errc::file_exists) throw;
        }
      }
      auto view = std::make_unique<QssdDisk>(path_);
      if (view->bytes() != backing_gib * QssdDisk::kGiB)
        throw std::runtime_error("El volumen QSSD de VRAM tiene otra capacidad.");
      disk_ = std::move(view);
    }
    return *disk_;
  }
};

}  // namespace reverse_windows
