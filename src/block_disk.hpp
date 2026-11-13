#pragma once
#include <cstddef>
#include <cstdint>

namespace reverse_windows {
// Windows sees sector-accurate bytes, independent of the host storage backend.
class BlockDisk {
 public:
  virtual ~BlockDisk() = default;
  virtual std::uint64_t bytes() const noexcept = 0;
  virtual std::uint64_t sectors() const noexcept = 0;
  virtual bool read_bytes(std::uint64_t offset, std::uint8_t* dest, std::size_t size) = 0;
  virtual bool read(std::uint64_t lba, std::uint16_t count, std::uint8_t* dest) = 0;
  virtual bool write(std::uint64_t lba, std::uint16_t count, const std::uint8_t* source) = 0;
  // Complete a guest FLUSH CACHE with the host backend's real sync barrier.
  virtual bool flush() = 0;
};
}  // namespace reverse_windows
