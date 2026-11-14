#pragma once

#include "relaciones_gpu.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace reverse_windows {

class GpuPciC01 {
 public:
  static constexpr unsigned kSlot = 3;
  static constexpr std::uint32_t kPciId = 0xc101fffeU; // Local prototype ID.
  static constexpr std::uint32_t kClassRevision = 0x03800001U; // Other display.
  static constexpr std::uint32_t kWindowBase = 0xfec10000U;
  static constexpr std::uint32_t kWindowBytes = 0x10000U;
  static constexpr std::uint32_t kBarBytes = 0x1000U;
  static constexpr std::uint32_t kBarMask = ~(kBarBytes - 1U);

  static constexpr unsigned kAbi = 0x00, kCapabilities = 0x04, kStatus = 0x08;
  static constexpr unsigned kDoorbell = 0x0c, kSource = 0x10, kDestination = 0x14;
  static constexpr unsigned kCountLow = 0x18, kCountHigh = 0x1c;
  static constexpr unsigned kResultLow = 0x20, kResultHigh = 0x24;
  static constexpr unsigned kData = 0x400, kDataBytes = 2048;
  enum Command : std::uint8_t { Copy = 1, SumIndices = 2 };
  enum Status : std::uint32_t { Idle, Done, InvalidRange, UnknownCommand, Unavailable };

  static bool in_window(std::uint64_t address) noexcept {
    return address >= kWindowBase && address < std::uint64_t(kWindowBase) + kWindowBytes;
  }

  std::uint32_t config_read(unsigned offset, unsigned bytes) const noexcept {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < bytes; ++i) {
      const unsigned at = offset + i;
      result |= std::uint32_t(byte_at(config_register(at & ~3U), at)) << (8U * i);
    }
    return result;
  }

  void config_write(unsigned offset, unsigned bytes, std::uint32_t value) noexcept {
    for (unsigned i = 0; i < bytes; ++i) {
      const unsigned at = offset + i;
      const auto byte = byte_at(value, i);
      if (at == 0x04) command_ = byte & 2U;
      if (at >= 0x10 && at < 0x14) store_byte(bar_, at, byte);
    }
    bar_ &= kBarMask;
  }

  // The caller forwards only addresses inside the firmware's PCI window.
  // Its unassigned portion reads as open bus even on a large RAM profile.
  std::uint8_t mmio_read(std::uint64_t address) const noexcept {
    if (!decodes(address)) return 0xff;
    const auto offset = unsigned(address - bar_);
    if (in_data(offset)) return data_[offset - kData];
    return byte_at(device_register(offset & ~3U), offset);
  }

  void mmio_write(std::uint64_t address, std::uint8_t byte) {
    if (!decodes(address)) return;
    const auto offset = unsigned(address - bar_);
    if (in_data(offset)) {
      data_[offset - kData] = byte;
      return;
    }
    switch (offset & ~3U) {
      case kSource: store_byte(source_, offset, byte); break;
      case kDestination: store_byte(destination_, offset, byte); break;
      case kCountLow: store_byte(count_low_, offset, byte); break;
      case kCountHigh: store_byte(count_high_, offset, byte); break;
      case kDoorbell:
        if (offset == kDoorbell) submit(byte);
        break;
    }
  }

 private:
  std::uint32_t command_ = 0, bar_ = kWindowBase;
  std::uint32_t source_ = 0, destination_ = 0, count_low_ = 0, count_high_ = 0;
  Status status_ = Idle;
  std::uint64_t result_ = 0;
  std::array<std::uint8_t, kDataBytes> data_{};

  static std::uint8_t byte_at(std::uint32_t value, unsigned offset) noexcept {
    return std::uint8_t(value >> ((offset & 3U) * 8U));
  }

  static void store_byte(std::uint32_t& target, unsigned offset, std::uint8_t value) noexcept {
    const auto shift = (offset & 3U) * 8U;
    target = (target & ~(0xffU << shift)) | (std::uint32_t(value) << shift);
  }

  std::uint32_t config_register(unsigned offset) const noexcept {
    switch (offset) {
      case 0x00: return kPciId;
      case 0x04: return command_;
      case 0x08: return kClassRevision;
      case 0x10: return bar_;
      case 0x3c: return 0xff;
      default: return 0;
    }
  }

  std::uint32_t device_register(unsigned offset) const noexcept {
    switch (offset) {
      case kAbi: return 1;
      case kCapabilities: return 3;
      case kStatus: return status_;
      case kSource: return source_;
      case kDestination: return destination_;
      case kCountLow: return count_low_;
      case kCountHigh: return count_high_;
      case kResultLow: return std::uint32_t(result_);
      case kResultHigh: return std::uint32_t(result_ >> 32);
      default: return 0;
    }
  }

  bool decodes(std::uint64_t address) const noexcept {
    return command_ && in_window(bar_) && address >= bar_ && address - bar_ < kBarBytes;
  }

  static bool in_data(unsigned offset) noexcept {
    return offset >= kData && offset < kData + kDataBytes;
  }

  void submit(std::uint8_t operation) {
    const auto count = (std::uint64_t(count_high_) << 32) | count_low_;
    result_ = 0;
    switch (operation) {
      case Copy:
        if (count > data_.size() || source_ > data_.size() - count ||
            destination_ > data_.size() - count) {
          status_ = InvalidRange;
          return;
        }
        if (count) std::memmove(data_.data() + destination_, data_.data() + source_,
                                std::size_t(count));
        result_ = count;
        break;
      case SumIndices:
        if (count > xgpu::domain_limit) {
          status_ = InvalidRange;
          return;
        }
        if (count) {
          try {
            // The field describes the index value at every lane. C12 reduces
            // this affine field exactly, without allocating the logical lanes.
            xgpu::ValueReader indices(xgpu::field_root(xgpu::Field(count, xgpu::Mode({0, 1}))));
            result_ = indices.sum_unsigned(0, count).value;
          } catch (const std::bad_alloc&) {
            status_ = Unavailable;
            return;
          }
        }
        break;
      default:
        status_ = UnknownCommand;
        return;
    }
    status_ = Done;
  }
};

} // namespace reverse_windows
