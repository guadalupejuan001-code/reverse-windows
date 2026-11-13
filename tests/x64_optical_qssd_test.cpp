#include "c06_motherboard.hpp"
#include "qssd_disk.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using cosmic_pc::Cpu;
using cosmic_pc::PcMachine;
using reverse_windows::PcMotherboardC06;
using reverse_windows::QssdDisk;

constexpr std::uint32_t kEntry = 0xa000;
constexpr std::uint32_t kContinuation = 0xb000;
constexpr std::uint32_t kOpticalBuffer = 0x11000;
constexpr std::uint32_t kPciId = 0x9000;
constexpr std::uint32_t kStack = 0x190000;
constexpr std::uint32_t kBootMarker = 0x8000;
constexpr std::uint16_t kMarker = 0x4d57;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct TempDirectory {
  std::filesystem::path path;

  TempDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() / "reverse-x64-qssd-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("could not create test directory");
    path = pattern;
  }
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

class Program {
 public:
  void emit(std::initializer_list<std::uint8_t> bytes) {
    code_.insert(code_.end(), bytes.begin(), bytes.end());
  }

  void dword(std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) code_.push_back(std::uint8_t(value >> (8 * i)));
  }

  void port(std::uint16_t p) { emit({0x66, 0xba, std::uint8_t(p), std::uint8_t(p >> 8)}); }

  void out_byte(std::uint16_t p, std::uint8_t value) {
    port(p);
    emit({0xb0, value, 0xee});
  }

  void read_optical_sector() {
    out_byte(0x176, 0xa0);
    out_byte(0x171, 0);
    out_byte(0x174, 0);
    out_byte(0x175, 8);
    out_byte(0x177, 0xa0);
    port(0x170);
    // ATAPI READ(10), LBA 0, length 1, in six 16-bit port writes.
    for (const auto word : {0x0028U, 0U, 0U, 0U, 1U, 0U})
      emit({0x66, 0xb8, std::uint8_t(word), std::uint8_t(word >> 8), 0x66, 0xef});
    emit({0xbf}); dword(kOpticalBuffer);
    emit({0xb9}); dword(1024);
    emit({0xfc, 0xf3, 0x66, 0x6d}); // CLD; REP INSW.
  }

  void write_boot_sector() {
    out_byte(0x1f6, 0xe0); // ATA master, LBA 28.
    out_byte(0x1f2, 1);
    out_byte(0x1f3, 0);
    out_byte(0x1f4, 0);
    out_byte(0x1f5, 0);
    out_byte(0x1f7, 0x30); // WRITE SECTORS.
    port(0x1f0);
    emit({0xbe}); dword(kOpticalBuffer);
    emit({0xb9}); dword(256);
    emit({0xf3, 0x66, 0x6f}); // REP OUTSW.
    out_byte(0x1f7, 0xe7); // FLUSH CACHE.
  }

  void load(Cpu& cpu, std::uint32_t address) const {
    cpu.memory.write_block(address, code_.data(), code_.size());
  }

 private:
  std::vector<std::uint8_t> code_;
};

void verify() {
  TempDirectory temp;
  const auto volume = temp.path / "guest.rqvd";
  QssdDisk::create(volume, 64);
  std::array<std::uint8_t, 2048> optical{};
  for (unsigned i = 0; i < optical.size(); ++i) optical[i] = std::uint8_t(i * 29 + i / 512);
  constexpr std::array<std::uint8_t, 7> boot_code{
      0xb8, std::uint8_t(kMarker), std::uint8_t(kMarker >> 8), // MOV AX, marker.
      0xa3, std::uint8_t(kBootMarker), std::uint8_t(kBootMarker >> 8), // MOV [8000h], AX.
      0xf4};
  std::copy(boot_code.begin(), boot_code.end(), optical.begin());
  optical[510] = 0x55;
  optical[511] = 0xaa;

  {
    PcMachine pc;
    pc.boot_long_mode_demo();
    QssdDisk disk(volume);
    PcMotherboardC06 board(pc, &disk, [&](auto offset, auto* out, auto count) {
      if (offset > optical.size() || count > optical.size() - offset) return false;
      std::copy_n(optical.data() + offset, count, out);
      return true;
    }, optical.size());
    for (unsigned i = 0; i < 16 && !pc.cpu.is_long_mode(); ++i) pc.run(256);
    auto& cpu = pc.cpu;
    require(cpu.is_long_mode() && !cpu.fault, "BIOS guest did not enter x86-64");
    require(cpu.halted && cpu.regs[Cpu::R9] == cosmic_pc::kLongModeResult,
            "BIOS guest did not finish its paging and 64-bit arithmetic checks");

    Program entry;
    entry.emit({0xfa, 0x31, 0xc0, 0x66, 0x8e, 0xd0}); // CLI; XOR EAX,EAX; MOV SS,AX.
    entry.emit({0x6a, std::uint8_t(cpu.segments[Cpu::CS]), 0x68});
    entry.dword(kContinuation);
    entry.emit({0x48, 0xcb}); // RETFQ through two 64-bit slots.
    entry.load(cpu, kEntry);

    Program continuation;
    continuation.port(0xcf8);
    continuation.emit({0xb8}); continuation.dword(0x80001000);
    continuation.emit({0xef});
    continuation.port(0xcfc);
    continuation.emit({0xed, 0x89, 0x04, 0x25}); continuation.dword(kPciId);
    continuation.read_optical_sector();
    continuation.write_boot_sector();
    continuation.emit({0xf4});
    continuation.load(cpu, kContinuation);

    cpu.halted = false;
    cpu.eip = kEntry;
    cpu.regs[Cpu::ESP] = kStack;
    for (unsigned i = 0; i < 8 && !cpu.halted && !cpu.fault; ++i) pc.run(2048);
    if (cpu.fault) throw std::runtime_error(cpu.fault->describe());
    require(cpu.halted && cpu.is_long_mode() && cpu.segments[Cpu::SS] == 0 &&
                cpu.regs[Cpu::ESP] == kStack && cpu.read32(kPciId) == 0xc0711b36,
            "long-mode transfer did not reach the PCI IDE guest driver");
    std::array<std::uint8_t, 2048> readback{};
    cpu.memory.read_block(kOpticalBuffer, readback.data(), readback.size());
    require(readback == optical, "guest ATAPI read corrupted optical bytes");
    const auto stats = board.attached_ata()->counters();
    require(stats.write_sectors == 1 && stats.flushes == 1 && disk.stats().commits > 0,
            "guest ATA write and flush did not reach QSSD");
  }

  QssdDisk disk(volume, QssdDisk::Access::ReadOnly);
  PcMachine reboot;
  reboot.mount([&](auto offset, auto* out, auto count) {
    return disk.read_bytes(offset, out, count);
  }, disk.bytes(), "raw");
  reboot.run(32);
  require(reboot.cpu.halted && !reboot.cpu.fault &&
              reboot.cpu.read16(kBootMarker) == kMarker,
          "BIOS could not execute the boot sector persisted by the x86-64 guest");
  std::cout << "REVERSE_WINDOWS_X64_OPTICAL_QSSD_PASS bios=1 long64=1 retfq=1 pci=1"
               " atapi_bytes=2048 ata_write=1 ata_flush=1 qssd_reboot=1\n";
}
} // namespace

int main() {
  try {
    verify();
  } catch (const std::exception& error) {
    std::cerr << "REVERSE_WINDOWS_X64_OPTICAL_QSSD_FAIL " << error.what() << '\n';
    return 1;
  }
}
