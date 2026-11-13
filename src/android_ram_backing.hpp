#pragma once

#include "sparse_ram.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace reverse_windows {

// Unlinked, process-private storage for disposable guest RAM. Each guest
// page has a fixed file offset. First writes are bounded and reuse of an
// offset does not count toward the bound again.
class AndroidRamBacking final : public cosmic_pc::SparseRam::PageBacking {
 public:
  static constexpr std::uint64_t kPageSize = cosmic_pc::SparseRam::kPageSize;
  static constexpr std::uint64_t kStorageReserve = 1ULL << 30;
  static constexpr std::uint64_t kMaxSpill = 4ULL << 30;

  static std::uint64_t spill_budget(const std::filesystem::path& directory) {
    struct statvfs stats {};
    if (directory.empty() || ::statvfs(directory.c_str(), &stats) != 0) return 0;
    const auto available = available_bytes(stats);
    return available >= kStorageReserve + write_batch_bytes(stats) ?
        std::min(kMaxSpill, available - kStorageReserve) / kPageSize * kPageSize : 0;
  }

  AndroidRamBacking(const std::filesystem::path& directory, std::uint64_t logical_bytes,
                    std::uint64_t spill_bytes)
      : page_count_(checked_page_count(logical_bytes)),
        slot_limit_(std::min(spill_bytes, kMaxSpill) / kPageSize),
        written_((page_count_ + 63) / 64, 0) {
    if (!slot_limit_)
      throw std::invalid_argument("Tamaño inválido para el respaldo de RAM.");
    const auto file_template = (directory / "reverse-ram-XXXXXX").string();
    std::vector<char> name(file_template.begin(), file_template.end());
    name.push_back('\0');
    const int fd = ::mkstemp(name.data());
    if (fd < 0) throw_io("Crear respaldo temporal de RAM");
    // Delete the directory entry before any guest bytes enter the file. The
    // kernel releases the file when its last fd closes, including after a crash.
    int unlinked;
    do {
      unlinked = ::unlink(name.data());
    } while (unlinked != 0 && errno == EINTR);
    if (unlinked != 0) {
      const auto error = errno;
      ::close(fd);
      throw std::system_error(error, std::generic_category(), "Desvincular respaldo temporal de RAM");
    }
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
      const auto error = errno;
      ::close(fd);
      throw std::system_error(error, std::generic_category(), "Proteger descriptor de la RAM");
    }
    fd_ = fd;
  }

  AndroidRamBacking(const AndroidRamBacking&) = delete;
  AndroidRamBacking& operator=(const AndroidRamBacking&) = delete;
  ~AndroidRamBacking() override { ::close(fd_); }

  void read(std::uint64_t page_index, std::uint8_t* page) override {
    check_index(page_index);
    if (!contains(page_index)) throw std::runtime_error("Página ausente en el respaldo de RAM.");
    std::size_t done = 0;
    while (done < kPageSize) {
      const auto bytes = ::pread(fd_, page + done, kPageSize - done, offset(page_index, done));
      if (bytes < 0 && errno == EINTR) continue;
      if (bytes < 0) throw_io("Leer respaldo de RAM");
      if (bytes == 0) throw std::runtime_error("Lectura incompleta del respaldo de RAM.");
      done += static_cast<std::size_t>(bytes);
    }
  }

  void write(std::uint64_t page_index, const std::uint8_t* page) override {
    check_index(page_index);
    const bool new_slot = !contains(page_index);
    if (new_slot && committed_slots_ >= slot_limit_)
      throw std::runtime_error("Se agotó el espacio asignado al respaldo temporal de RAM.");
    if (new_slot && (committed_slots_ % kSpaceCheckInterval == 0)) check_storage();

    std::size_t done = 0;
    while (done < kPageSize) {
      const auto bytes = ::pwrite(fd_, page + done, kPageSize - done, offset(page_index, done));
      if (bytes < 0 && errno == EINTR) continue;
      if (bytes < 0) throw_io("Escribir respaldo de RAM");
      if (bytes == 0) throw std::runtime_error("Escritura incompleta del respaldo de RAM.");
      done += static_cast<std::size_t>(bytes);
    }
    if (new_slot) {
      written_[page_index / 64] |= 1ULL << (page_index % 64);
      ++committed_slots_;
    }
  }

  void reset() noexcept override {
    // Failure to release blocks does not restore old guest bytes. SparseRam
    // clears its page table and any later eviction overwrites the whole slot.
    int result;
    do {
      result = ::ftruncate(fd_, 0);
    } while (result != 0 && errno == EINTR);
    if (result == 0) {
      std::fill(written_.begin(), written_.end(), 0);
      committed_slots_ = 0;
    }
  }

  std::uint64_t file_payload_bytes() const noexcept { return committed_slots_ * kPageSize; }

 private:
  // Keep the free-space check off the eviction hot path and leave room for
  // the following batch of writes, including larger filesystem fragments.
  static constexpr std::uint64_t kSpaceCheckInterval = 256;

  static std::uint64_t checked_page_count(std::uint64_t bytes) {
    if (!bytes || bytes > kMaxSpill ||
        bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
      throw std::invalid_argument("Tamaño inválido para el respaldo de RAM.");
    return bytes / kPageSize + (bytes % kPageSize != 0);
  }

  static std::uint64_t fragment_bytes(const struct statvfs& stats) noexcept {
    return stats.f_frsize ? stats.f_frsize : stats.f_bsize;
  }

  static std::uint64_t available_bytes(const struct statvfs& stats) {
    const auto unit = fragment_bytes(stats);
    const auto max = std::numeric_limits<std::uint64_t>::max();
    return unit && stats.f_bavail > max / unit ? max : std::uint64_t(stats.f_bavail) * unit;
  }

  static std::uint64_t write_batch_bytes(const struct statvfs& stats) noexcept {
    return std::max(fragment_bytes(stats), kPageSize) * kSpaceCheckInterval;
  }

  static off_t offset(std::uint64_t index, std::size_t in_page) {
    return static_cast<off_t>(index * kPageSize + in_page);
  }

  [[noreturn]] static void throw_io(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
  }

  void check_index(std::uint64_t index) const {
    if (index >= page_count_) throw std::out_of_range("Página de respaldo fuera de la RAM invitada.");
  }

  bool contains(std::uint64_t index) const {
    return (written_[index / 64] & (1ULL << (index % 64))) != 0;
  }

  void check_storage() const {
    struct statvfs stats {};
    if (::fstatvfs(fd_, &stats) != 0) throw_io("Consultar almacenamiento para la RAM");
    if (available_bytes(stats) < kStorageReserve + write_batch_bytes(stats))
      throw std::runtime_error("Android tiene poco espacio para continuar el respaldo de RAM.");
  }

  const std::uint64_t page_count_;
  const std::uint64_t slot_limit_;
  std::vector<std::uint64_t> written_;
  std::uint64_t committed_slots_ = 0;
  int fd_ = -1;
};

} // namespace reverse_windows
