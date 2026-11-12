#pragma once

// REVERSE Windows: a real, independent, writable raw HDD image.
// Never writes to the boot ISO, any host block device, or an unselected path.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace reverse_windows {
class DiskImage {
 public:
  static constexpr std::uint64_t kSectorSize = 512;
  static constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;

  static void create(const std::filesystem::path& path, std::uint64_t gib) {
    if (gib == 0 || gib > 256) throw std::invalid_argument("Virtual disk capacity: 1..256 GiB");
    verify_name(path);
    if (std::filesystem::exists(path) || std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
      throw std::runtime_error("Virtual disk already exists; refusing overwrite");
    {
      std::ofstream new_file(path, std::ios::binary | std::ios::out | std::ios::trunc);
      if (!new_file) throw std::runtime_error("Cannot create virtual disk image");
    }
    // Logical capacity is large, but this does not grant unlimited real host storage.
    // Filesystems may allocate sparse files; disk usage must be monitored.
    try {
      std::filesystem::resize_file(path, gib * kGiB);
    } catch (...) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      throw;
    }
  }

  explicit DiskImage(std::filesystem::path path) : path_(std::move(path)) {
    verify_name(path_);
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path_)) ||
        !std::filesystem::is_regular_file(path_))
      throw std::runtime_error("Selected virtual disk must be a regular non-symlink file");
    bytes_ = std::filesystem::file_size(path_);
    if (bytes_ == 0 || bytes_ % kSectorSize || bytes_ > 256 * kGiB)
      throw std::runtime_error("Invalid virtual disk size/alignment");
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_) throw std::runtime_error("Cannot open virtual disk for read and write");
  }

  DiskImage(const DiskImage&) = delete;
  DiskImage& operator=(const DiskImage&) = delete;
  std::uint64_t bytes() const noexcept { return bytes_; }
  std::uint64_t sectors() const noexcept { return bytes_ / kSectorSize; }

  bool read(std::uint64_t lba, std::uint16_t sectors, std::uint8_t* target) {
    if (!target || !within(lba, sectors)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto offset = static_cast<std::streamoff>(lba * kSectorSize);
    const auto size = static_cast<std::streamsize>(std::uint64_t(sectors) * kSectorSize);
    file_.clear();
    file_.seekg(offset, std::ios::beg);
    if (!file_) return false;
    file_.read(reinterpret_cast<char*>(target), size);
    return file_.gcount() == size && static_cast<bool>(file_);
  }

  bool write(std::uint64_t lba, std::uint16_t sectors, const std::uint8_t* source) {
    if (!source || !within(lba, sectors)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto offset = static_cast<std::streamoff>(lba * kSectorSize);
    const auto size = static_cast<std::streamsize>(std::uint64_t(sectors) * kSectorSize);
    file_.clear();
    file_.seekp(offset, std::ios::beg);
    if (!file_) return false;
    file_.write(reinterpret_cast<const char*>(source), size);
    file_.flush();
    return static_cast<bool>(file_);
  }

 private:
  static void verify_name(const std::filesystem::path& path) {
    if (path.empty() || path.extension() != ".rvdisk")
      throw std::invalid_argument("Only explicit .rvdisk paths are accepted (never a host device)");
  }
  bool within(std::uint64_t lba, std::uint16_t count) const noexcept {
    return count && lba <= sectors() && count <= sectors() - lba;
  }
  std::filesystem::path path_;
  std::uint64_t bytes_ = 0;
  std::fstream file_;
  std::mutex mutex_;
};
}  // namespace reverse_windows
