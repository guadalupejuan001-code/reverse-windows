#include "bios_hard_disk.hpp"
#include "pc_machine.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
}

int main() {
  auto path = std::filesystem::temp_directory_path() /
      ("reverse-windows-c01-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) + ".rvdisk");
  try {
    reverse_windows::DiskImage::create(path, 1);
    require(std::filesystem::file_size(path) == reverse_windows::DiskImage::kGiB,
            "Disk has wrong logical capacity");

    {
      reverse_windows::DiskImage disk(path);
      cosmic_pc::PcMachine pc(2048ULL*1024*1024, 256ULL*1024*1024);
      pc.boot_demo();
      reverse_windows::BiosHardDisk bridge(pc, disk);
      // BIOS EDD Disk Address Packet: 1 x 512-byte sector at guest 0000:9000,
      // sourced from LBA 7 of the independent writable HDD (DL 80h).
      const std::array<std::uint8_t, 16> dap{
          0x10, 0x00, 0x01, 0x00, 0x00, 0x90, 0x00, 0x00,
          0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
      pc.cpu.memory.write_block(0x8100, dap.data(), dap.size());

      // Real x86 machine code: write 0xBEEF to RAM; issue INT 13h AH=43;
      // clear RAM; issue INT 13h AH=42; halt. No test double calls the API.
      const std::array<std::uint8_t, 33> guest{
          0xb8,0xef,0xbe,       // MOV AX,BEEF
          0xa3,0x00,0x90,       // MOV [9000],AX
          0xbe,0x00,0x81,       // MOV SI,8100
          0xb2,0x80,            // MOV DL,80
          0xb4,0x43,            // MOV AH,43 (EDD WRITE)
          0xcd,0x13,            // INT 13h
          0xb8,0x00,0x00,       // MOV AX,0000
          0xa3,0x00,0x90,       // MOV [9000],AX
          0xbe,0x00,0x81,       // MOV SI,8100
          0xb2,0x80,            // MOV DL,80
          0xb4,0x42,            // MOV AH,42 (EDD READ)
          0xcd,0x13,            // INT 13h
          0xf4,                 // HLT
          0x90,0x90,0x90,0x90   // padding
      };
      pc.cpu.memory.write_block(0x7c00, guest.data(), guest.size());
      pc.cpu.reset(0, 0x7c00);
      pc.cpu.set_reg16(cosmic_pc::Cpu::ESP, 0x7b00);
      pc.run(200);
      require(!pc.cpu.fault && pc.cpu.halted, "Real x86 INT13 disk write/read guest failed");
      require(!pc.cpu.flag(cosmic_pc::Cpu::CF), "INT13 reported a disk error");
      require(pc.cpu.memory[0x9000] == 0xef && pc.cpu.memory[0x9001] == 0xbe,
              "Guest bytes read back from HDD do not match");
      require(pc.cpu.instructions >= 10, "Guest CPU did not execute real instructions");
      std::cout << "REVERSE_WINDOWS_BIOS_HDD_REAL_X86_PASS instructions="
                << pc.cpu.instructions << " lba=7 write=1 read=1\n";
    }

    // Persistence is verified across destruction/re-opening of the HDD.
    {
      reverse_windows::DiskImage reopen(path);
      std::array<std::uint8_t, 512> sector{}, first{};
      require(reopen.read(7, 1, sector.data()), "Cannot reopen written HDD sector");
      require(sector[0] == 0xef && sector[1] == 0xbe, "HDD did not persist guest writes");
      require(reopen.read(0, 1, first.data()), "Cannot read untouched HDD sector");
      for (auto c : first) require(c == 0, "Unwritten sparse HDD byte must stay zero");
      require(!reopen.read(reopen.sectors(), 1, sector.data()),
              "Out-of-bounds sector read accepted");
      require(!reopen.write(reopen.sectors(), 1, sector.data()),
              "Out-of-bounds sector write accepted");
    }

    std::filesystem::remove(path);
    std::cout << "REVERSE_WINDOWS_DISK_PERSISTENCE_PASS isowrites=0\n";
    return 0;
  } catch (const std::exception& e) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::cerr << "REVERSE_WINDOWS_DISK_FAIL: " << e.what() << '\n';
    return 1;
  }
}
