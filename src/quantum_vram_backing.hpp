#pragma once

#include "c08_gpu_protocol.h"
#include "mundo_x/infinite_session.hpp"
#include "qssd_disk.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <zlib.h>

namespace reverse_windows {

// Entanglement can cross the GPU aperture. An explicit checkpoint therefore
// includes the entire shared Éter session, including its measurement RNG.
class QuantumVramBacking {
 public:
  explicit QuantumVramBacking(const std::filesystem::path& vram, std::shared_ptr<SmpWorkers> workers)
      : path_(vram.string() + ".qstate.rqvd"), workers_(std::move(workers)) {}

  unsigned transfer(mundo_x::InfiniteSession& session, bool save, std::size_t& transferred) {
    transferred = 0;
    try {
      if (!mount(save)) return RWGPU_NO_CHECKPOINT;
      std::array<std::uint8_t, 20> header{};
      if (!disk_->read_bytes(0, header.data(), header.size())) return RWGPU_FAILED;
      const bool empty = std::all_of(header.begin(), header.end(), [](auto byte) { return byte == 0; });
      const auto slot = get(header.data()+8);
      const auto size = get(header.data()+12);
      if (!empty && (std::memcmp(header.data(), "QVRAM001", 8) || slot > 1 || !size || size > capacity))
        return RWGPU_CORRUPT_CHECKPOINT;
      if (save) {
        const auto snapshot = session.snapshot();
        if (snapshot.empty() || snapshot.size() > capacity) return RWGPU_QUANTUM_UNAVAILABLE;
        const unsigned next = empty ? 0 : 1-slot;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(snapshot.data());
        constexpr auto chunk = QssdDisk::kMaxBulkTransactionBytes;
        for (std::size_t pos=0; pos<snapshot.size(); pos+=chunk) {
          const auto count = std::min(chunk, snapshot.size()-pos);
          if (!disk_->write_bytes(payload(next)+pos, count, bytes+pos)) return RWGPU_FAILED;
        }
        // The fsynced manifest is the commit point. An interrupted write
        // to the inactive slot leaves the previous checkpoint readable.
        std::memcpy(header.data(), "QVRAM001", 8);
        put(header.data()+8, next);
        put(header.data()+12, static_cast<std::uint32_t>(snapshot.size()));
        put(header.data()+16, crc32(0, bytes, static_cast<uInt>(snapshot.size())));
        if (!disk_->write_bytes(0, header.size(), header.data())) return RWGPU_FAILED;
        transferred = snapshot.size();
      } else {
        if (empty) return RWGPU_NO_CHECKPOINT;
        std::string snapshot(size, '\0');
        auto* bytes = reinterpret_cast<std::uint8_t*>(snapshot.data());
        if (!disk_->read_bytes(payload(slot), bytes, size)) return RWGPU_FAILED;
        if (crc32(0, bytes, size) != get(header.data()+16)) return RWGPU_CORRUPT_CHECKPOINT;
        try {
          session.restore(snapshot);
        } catch (const std::invalid_argument&) {
          return RWGPU_CORRUPT_CHECKPOINT;
        }
        transferred = size;
      }
      return RWGPU_OK;
    } catch (const std::length_error&) {
      return RWGPU_QUANTUM_UNAVAILABLE;
    } catch (const std::exception&) {
      return RWGPU_FAILED;
    }
  }

 private:
  static constexpr std::uint64_t capacity = mundo_x::InfiniteCore::max_snapshot_bytes;
  static_assert(capacity % QssdDisk::kBlockSize == 0 && capacity*2 + QssdDisk::kBlockSize <= QssdDisk::kGiB);
  static std::uint64_t payload(unsigned slot) { return QssdDisk::kBlockSize + slot*capacity; }
  static std::uint32_t get(const std::uint8_t* p) {
    std::uint32_t value=0;
    for (unsigned i=0; i<4; ++i) value |= std::uint32_t(p[i]) << (8*i);
    return value;
  }
  static void put(std::uint8_t* p, std::uint32_t value) {
    for (unsigned i=0; i<4; ++i) p[i]=static_cast<std::uint8_t>(value >> (8*i));
  }

  std::filesystem::path path_;
  std::shared_ptr<SmpWorkers> workers_;
  std::unique_ptr<QssdDisk> disk_;

  bool mount(bool create) {
    if (disk_) return true;
    try {
      disk_ = std::make_unique<QssdDisk>(path_, QssdDisk::Access::ReadWrite, workers_);
    } catch (const std::system_error& error) {
      if (error.code() != std::errc::no_such_file_or_directory) throw;
      if (!create) return false;
      try { QssdDisk::create(path_, 1); }
      catch (const std::system_error& other) { if (other.code() != std::errc::file_exists) throw; }
      disk_ = std::make_unique<QssdDisk>(path_, QssdDisk::Access::ReadWrite, workers_);
    }
    if (disk_->bytes() != QssdDisk::kGiB) {
      disk_.reset();
      throw std::runtime_error("Wrong capacity for quantum state volume");
    }
    return true;
  }
};

} // namespace reverse_windows
