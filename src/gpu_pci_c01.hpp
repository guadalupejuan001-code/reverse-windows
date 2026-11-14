#pragma once

#include "gpu_vram.hpp"
#include "pc_machine.hpp"
#include "relaciones_gpu.hpp"
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

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
  static constexpr unsigned kSourceHigh = 0x28, kDestinationHigh = 0x2c;
  static constexpr unsigned kVramLow = 0x30, kVramHigh = 0x34, kTransferMax = 0x38;
  enum Command : std::uint8_t { Copy = 1, SumIndices = 2, Upload = 3, Download = 4 };
  enum Status : std::uint32_t { Idle, Done, InvalidRange, UnknownCommand, Unavailable };

  explicit GpuPciC01(cosmic_pc::Cpu& cpu, std::filesystem::path path = {},
                     std::filesystem::path scratch = {})
      : cpu_(cpu), vram_(std::move(path), std::move(scratch)) {}

  void reset() noexcept {
    command_ = source_ = source_high_ = destination_ = destination_high_ = 0;
    count_low_ = count_high_ = 0;
    bar_ = kWindowBase;
    status_ = Idle;
    result_ = 0;
    vram_.reset();
  }

  bool durable() const noexcept { return vram_.durable(); }
  std::uint64_t journal_bytes() const { return vram_.journal_bytes(); }
  void release_cache() noexcept { vram_.release_cache(); }

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
      if (at == 0x04) command_ = byte & 6U;
      if (at >= 0x10 && at < 0x14) store_byte(bar_, at, byte);
    }
    bar_ &= kBarMask;
  }

  // The caller forwards only addresses inside the firmware's PCI window.
  // Its unassigned portion reads as open bus even on a large RAM profile.
  std::uint8_t mmio_read(std::uint64_t address) const noexcept {
    if (!decodes(address)) return 0xff;
    const auto offset = unsigned(address - bar_);
    return byte_at(device_register(offset & ~3U), offset);
  }

  void mmio_write(std::uint64_t address, std::uint8_t byte) {
    if (!decodes(address)) return;
    const auto offset = unsigned(address - bar_);
    switch (offset & ~3U) {
      case kSource: store_byte(source_, offset, byte); break;
      case kDestination: store_byte(destination_, offset, byte); break;
      case kCountLow: store_byte(count_low_, offset, byte); break;
      case kCountHigh: store_byte(count_high_, offset, byte); break;
      case kSourceHigh: store_byte(source_high_, offset, byte); break;
      case kDestinationHigh: store_byte(destination_high_, offset, byte); break;
      case kDoorbell:
        if (offset == kDoorbell) submit(byte);
        break;
    }
  }

 private:
  cosmic_pc::Cpu& cpu_;
  GpuVram vram_;
  std::uint32_t command_ = 0, bar_ = kWindowBase;
  std::uint32_t source_ = 0, source_high_ = 0, destination_ = 0, destination_high_ = 0;
  std::uint32_t count_low_ = 0, count_high_ = 0;
  Status status_ = Idle;
  std::uint64_t result_ = 0;
  std::vector<std::uint8_t> transfer_;

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
      case kAbi: return 2;
      case kCapabilities: return 0xf;
      case kStatus: return status_;
      case kSource: return source_;
      case kDestination: return destination_;
      case kCountLow: return count_low_;
      case kCountHigh: return count_high_;
      case kResultLow: return std::uint32_t(result_);
      case kResultHigh: return std::uint32_t(result_ >> 32);
      case kSourceHigh: return source_high_;
      case kDestinationHigh: return destination_high_;
      case kVramLow: return std::uint32_t(GpuVram::kBytes);
      case kVramHigh: return std::uint32_t(GpuVram::kBytes >> 32);
      case kTransferMax: return GpuVram::kTransferBytes;
      default: return 0;
    }
  }

  bool decodes(std::uint64_t address) const noexcept {
    return (command_ & 2U) && in_window(bar_) && address >= bar_ && address - bar_ < kBarBytes;
  }

  static std::uint64_t wide(std::uint32_t high, std::uint32_t low) noexcept {
    return (std::uint64_t(high) << 32) | low;
  }

  bool ordinary_ram(std::uint64_t address, std::uint64_t size) const {
    if (address > cpu_.memory.size() || size > cpu_.memory.size() - address ||
        address > cosmic_pc::PcMachine::kIoApicAddress ||
        size > cosmic_pc::PcMachine::kIoApicAddress - address) return false;
    if (!size) return true;
    const auto overlaps = [&](std::uint64_t start, std::uint64_t length) {
      return address < start + length && start < address + size;
    };
    return !overlaps(0xa0000, 0x60000) &&
           !overlaps(cosmic_pc::PcVideo::kFramebufferAddress, cosmic_pc::PcVideo::kApertureBytes) &&
           (!cpu_.memory.has_window() || !overlaps(cpu_.memory.window_base(), cpu_.memory.window_bytes())) &&
           (!cpu_.mmio_range_claim_hook || !cpu_.mmio_range_claim_hook(address, std::size_t(size)));
  }

  Status transfer(std::uint8_t operation, std::uint64_t source, std::uint64_t destination,
                  std::uint64_t count) {
    if (operation != Copy && !(command_ & 4U)) return Unavailable;
    const bool source_in_ram = operation == Upload;
    const bool destination_in_ram = operation == Download;
    if (count > GpuVram::kTransferBytes ||
        (!destination_in_ram && count > GpuVram::kTransferBytes - destination % GpuVram::kPageBytes) ||
        !(source_in_ram ? ordinary_ram(source, count) : GpuVram::valid_range(source, count)) ||
        !(destination_in_ram ? ordinary_ram(destination, count) : GpuVram::valid_range(destination, count)))
      return InvalidRange;
    if (!count) return Done;

    const auto bytes = std::size_t(count);
    if (destination_in_ram) {
      if (const auto* data = vram_.resident_read(source, bytes)) {
        cpu_.memory.write_block(destination, data, bytes);
        result_ = count;
        return Done;
      }
    }
    transfer_.resize(bytes);
    if (source_in_ram) {
      cpu_.memory.read_block(source, transfer_.data(), transfer_.size());
    } else if (vram_.read(source, transfer_.data(), transfer_.size()) != GpuVram::Result::Complete) {
      return Unavailable;
    }
    if (destination_in_ram) {
      cpu_.memory.write_block(destination, transfer_.data(), transfer_.size());
    } else if (vram_.write(destination, transfer_.data(), transfer_.size()) != GpuVram::Result::Complete) {
      return Unavailable;
    }
    result_ = count;
    return Done;
  }

  void submit(std::uint8_t operation) {
    const auto count = wide(count_high_, count_low_);
    result_ = 0;
    try {
      switch (operation) {
        case Copy: case Upload: case Download:
          status_ = transfer(operation, wide(source_high_, source_),
                             wide(destination_high_, destination_), count);
          return;
        case SumIndices:
          if (count > xgpu::domain_limit) {
            status_ = InvalidRange;
            return;
          }
          if (count) {
            xgpu::ValueReader indices(xgpu::field_root(xgpu::Field(count, xgpu::Mode({0, 1}))));
            result_ = indices.sum_unsigned(0, count).value;
          }
          status_ = Done;
          return;
        default:
          status_ = UnknownCommand;
      }
    } catch (const std::exception&) {
      status_ = Unavailable;
    }
  }
};

} // namespace reverse_windows
