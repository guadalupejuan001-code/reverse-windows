#pragma once

#include "block_disk.hpp"
#include "pc_machine.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

namespace reverse_windows {

// Adds a separate BIOS hard disk (DL=80h) without forking or replacing the
// canonical Mundo X x86 interpreter. The existing ISO remains DL=E0h and
// continues through the original PcMachine BIOS handler, unmodified.
class BiosHardDisk {
 public:
  BiosHardDisk(cosmic_pc::PcMachine& pc, BlockDisk& disk)
      : pc_(pc), disk_(disk), previous_(pc.cpu.interrupt_hook) {
    if (!previous_) throw std::invalid_argument("PcMachine BIOS handler is missing");
    pc_.cpu.interrupt_hook = [this](cosmic_pc::Cpu& cpu, std::uint8_t vector) {
      if (vector == 0x13 && cpu.reg8(cosmic_pc::Cpu::EDX) == 0x80) {
        handle(cpu);
        return true;
      }
      return previous_(cpu, vector);
    };
  }
  BiosHardDisk(const BiosHardDisk&) = delete;
  BiosHardDisk& operator=(const BiosHardDisk&) = delete;
  ~BiosHardDisk() { pc_.cpu.interrupt_hook = previous_; }

 private:
  static constexpr std::uint16_t kHeads = 255;
  static constexpr std::uint16_t kSectorsPerTrack = 63;
  static constexpr std::uint16_t kMaxTransferSectors = 127;

  static std::uint8_t ah(const cosmic_pc::Cpu& cpu) { return cpu.reg8(4); }
  static void ah(cosmic_pc::Cpu& cpu, std::uint8_t value) { cpu.set_reg8(4, value); }
  static void success(cosmic_pc::Cpu& cpu) {
    ah(cpu, 0); cpu.set_flag(cosmic_pc::Cpu::CF, false);
  }
  void error(cosmic_pc::Cpu& cpu, std::uint8_t code) {
    last_error_ = code; ah(cpu, code); cpu.set_flag(cosmic_pc::Cpu::CF, true);
  }
  static std::uint64_t seg_off(std::uint16_t segment, std::uint16_t offset) {
    return (std::uint64_t(segment) << 4U) + offset;
  }
  static bool guest_span(const cosmic_pc::Cpu& cpu, std::uint64_t start, std::uint64_t count) {
    return start <= cpu.memory.size() && count <= cpu.memory.size() - start;
  }

  bool transfer(cosmic_pc::Cpu& cpu, bool is_write, std::uint64_t lba,
                std::uint16_t count, std::uint64_t guest_physical) {
    if (count == 0 || count > kMaxTransferSectors ||
        lba > disk_.sectors() || count > disk_.sectors() - lba ||
        !guest_span(cpu, guest_physical, std::uint64_t(count) * 512))
      return false;

    std::vector<std::uint8_t> bytes(std::size_t(count) * 512);
    if (is_write) {
      for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = cpu.read8(guest_physical + i);
      return disk_.write(lba, count, bytes.data());
    }
    if (!disk_.read(lba, count, bytes.data())) return false;
    // The CPU's write journal preserves guest RAM if the current INT step faults.
    // No data is invented: every byte came from the physical disk image.
    for (std::size_t i = 0; i < bytes.size(); ++i)
      cpu.write8(guest_physical + i, bytes[i]);
    return true;
  }

  void chs(cosmic_pc::Cpu& cpu, bool is_write) {
    const auto sector = unsigned(cpu.reg8(cosmic_pc::Cpu::ECX)) & 63U;
    const auto cylinder = unsigned(cpu.reg8(5)) |
        ((unsigned(cpu.reg8(cosmic_pc::Cpu::ECX)) & 0xc0U) << 2U);
    const auto head = unsigned(cpu.reg8(6));
    const auto count = cpu.reg8(cosmic_pc::Cpu::EAX);
    const auto address = seg_off(cpu.segments[cosmic_pc::Cpu::ES], cpu.reg16(cosmic_pc::Cpu::EBX));
    if (sector == 0 || head >= kHeads || cylinder >= 1024) {
      error(cpu, 4); cpu.set_reg8(cosmic_pc::Cpu::EAX, 0); return;
    }
    const std::uint64_t lba = (std::uint64_t(cylinder) * kHeads + head) *
                              kSectorsPerTrack + sector - 1;
    if (!transfer(cpu, is_write, lba, count, address)) {
      error(cpu, 4); cpu.set_reg8(cosmic_pc::Cpu::EAX, 0); return;
    }
    success(cpu);
    cpu.set_reg8(cosmic_pc::Cpu::EAX, count);
    last_error_ = 0;
  }

  void extended(cosmic_pc::Cpu& cpu, bool is_write) {
    const auto dap = seg_off(cpu.segments[cosmic_pc::Cpu::DS], cpu.reg16(cosmic_pc::Cpu::ESI));
    if (!guest_span(cpu, dap, 16) || cpu.read8(dap) < 16 || cpu.read8(dap+1) != 0) {
      error(cpu, 1); return;
    }
    const auto count = cpu.read16(dap + 2);
    const auto offset = cpu.read16(dap + 4), segment = cpu.read16(dap + 6);
    const std::uint64_t lba = std::uint64_t(cpu.read32(dap + 8)) |
        (std::uint64_t(cpu.read32(dap + 12)) << 32);
    // Reject EDD flat-address form until its full 24-byte packet is supported.
    if (offset == 0xffff && segment == 0xffff ||
        !transfer(cpu, is_write, lba, count, seg_off(segment, offset))) {
      cpu.write16(dap + 2, 0);
      error(cpu, 4);
      return;
    }
    success(cpu);
    last_error_ = 0;
  }

  void drive_parameters(cosmic_pc::Cpu& cpu) {
    const auto physical = seg_off(cpu.segments[cosmic_pc::Cpu::DS], cpu.reg16(cosmic_pc::Cpu::ESI));
    if (!guest_span(cpu, physical, 26) || cpu.read16(physical) < 26) {
      error(cpu, 1); return;
    }
    const auto cylinders = std::min<std::uint64_t>(
        1024, (disk_.sectors() + (std::uint64_t(kHeads) * kSectorsPerTrack) - 1) /
            (std::uint64_t(kHeads) * kSectorsPerTrack));
    cpu.write16(physical, 26);
    cpu.write16(physical + 2, 2); // geometry available; fixed-media device
    cpu.write32(physical + 4, static_cast<std::uint32_t>(cylinders));
    cpu.write32(physical + 8, kHeads);
    cpu.write32(physical + 12, kSectorsPerTrack);
    cpu.write64(physical + 16, disk_.sectors());
    cpu.write16(physical + 24, 512);
    success(cpu); last_error_ = 0;
  }

  void handle(cosmic_pc::Cpu& cpu) {
    // PcMachine's CPU only routes software INT to BIOS in real mode.
    switch (ah(cpu)) {
      case 0x00: // Reset disk controller
        success(cpu); last_error_ = 0; return;
      case 0x01: {
        const auto previous = last_error_;
        ah(cpu, previous); cpu.set_flag(cosmic_pc::Cpu::CF, previous != 0); return;
      }
      case 0x02: chs(cpu, false); return;
      case 0x03: chs(cpu, true); return;
      case 0x08: {
        const auto cylinders = std::min<std::uint64_t>(
            1024, (disk_.sectors() + std::uint64_t(kHeads)*kSectorsPerTrack-1) /
                (std::uint64_t(kHeads)*kSectorsPerTrack));
        const auto last = static_cast<std::uint16_t>(std::max<std::uint64_t>(1, cylinders)-1);
        cpu.set_reg8(5, last & 0xff);
        cpu.set_reg8(cosmic_pc::Cpu::ECX, (kSectorsPerTrack & 63) | ((last >> 2) & 0xc0));
        cpu.set_reg8(6, kHeads - 1);
        cpu.set_reg8(cosmic_pc::Cpu::EDX, 1);
        success(cpu); last_error_ = 0; return;
      }
      case 0x15: {
        cpu.set_reg8(4, 3); // Fixed disk
        const auto n = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(disk_.sectors(), 0xffffffffULL));
        cpu.set_reg16(cosmic_pc::Cpu::ECX, n >> 16);
        cpu.set_reg16(cosmic_pc::Cpu::EDX, n & 0xffff);
        cpu.set_flag(cosmic_pc::Cpu::CF, false);
        last_error_ = 0; return;
      }
      case 0x41:
        if (cpu.reg16(cosmic_pc::Cpu::EBX) != 0x55aa) {
          error(cpu, 1); return;
        }
        cpu.set_reg16(cosmic_pc::Cpu::EBX, 0xaa55);
        cpu.set_reg16(cosmic_pc::Cpu::ECX, 1); // EDD transfer services only
        ah(cpu, 0x30); // EDD 3.x version query, supported subset is explicit
        cpu.set_flag(cosmic_pc::Cpu::CF, false);
        last_error_ = 0; return;
      case 0x42: extended(cpu, false); return;
      case 0x43: extended(cpu, true); return;
      case 0x48: drive_parameters(cpu); return;
      default:
        error(cpu, 1); return; // Not a fake success or an arbitrary disk fallback.
    }
  }

  cosmic_pc::PcMachine& pc_;
  BlockDisk& disk_;
  std::function<bool(cosmic_pc::Cpu&, std::uint8_t)> previous_;
  std::uint8_t last_error_ = 0;
};
}  // namespace reverse_windows
