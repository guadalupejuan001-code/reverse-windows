#include "virtual_disk.hpp"
#include "bios_hard_disk.hpp"
#include "pc_machine.hpp"
#include "qssd_disk.hpp"

#include <algorithm>
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

void verify_fixed_disk_during_cd_boot(const std::filesystem::path& raw_path) {
  auto path = raw_path;
  path.replace_extension(".rqvd");
  struct RemoveDisk {
    const std::filesystem::path& path;
    ~RemoveDisk() { std::error_code ignored; std::filesystem::remove(path, ignored); }
  } cleanup{path};
  reverse_windows::QssdDisk::create(path, 64);
  reverse_windows::QssdDisk disk(path);

  constexpr unsigned cd_sector = cosmic_pc::BootMedia::kCdSectorSize;
  constexpr unsigned catalog = 20 * cd_sector, boot = 21 * cd_sector;
  std::array<std::uint8_t,22 * cd_sector> iso{};
  const std::string cd_id = "CD001", el_torito = "EL TORITO SPECIFICATION";
  for (unsigned sector : {16U,17U,18U}) {
    std::copy(cd_id.begin(), cd_id.end(), iso.begin() + sector * cd_sector + 1);
    iso[sector * cd_sector + 6] = 1;
  }
  iso[16 * cd_sector] = 1;
  iso[18 * cd_sector] = 255;
  std::copy(el_torito.begin(), el_torito.end(), iso.begin() + 17 * cd_sector + 7);
  iso[17 * cd_sector + 0x47] = 20;
  iso[catalog] = 1;
  iso[catalog + 28] = 0xaa; iso[catalog + 29] = 0x55;
  iso[catalog + 30] = 0x55; iso[catalog + 31] = 0xaa;
  iso[catalog + 32] = 0x88;
  iso[catalog + 38] = 1;
  iso[catalog + 40] = 21;
  iso[boot] = 0xcd; iso[boot + 1] = 0x13; // INT 13h
  iso[boot + 2] = 0xf4; // HLT

  cosmic_pc::PcMachine pc(16ULL * 1024 * 1024);
  pc.mount([&](auto off, auto* dest, auto count) {
    std::copy_n(iso.data() + off, count, dest);
    return true;
  }, iso.size(), "iso");
  auto& cpu = pc.cpu;
  require(cpu.read8(0x475) == 0, "CD alone must leave BDA fixed-disk count zero");
  using Cpu = cosmic_pc::Cpu;
  auto call = [&](std::uint8_t drive, std::uint8_t fn) {
    cpu.eip = 0x7c00;
    cpu.set_reg16(Cpu::EDX, drive);
    cpu.set_reg8(4, fn);
    require(cpu.step() == cosmic_pc::StepResult::Executed && !cpu.fault,
            "BIOS disk request failed to return to the guest");
  };
  {
    reverse_windows::BiosHardDisk bridge(pc, disk);
    require(cpu.read8(0x475) == 1 && cpu.reg8(Cpu::EDX) == 0xe0,
            "Fixed disk registration changed the optical boot drive or missed the BDA");
    constexpr unsigned params = 0x8300;
    cpu.set_reg16(Cpu::ESI, params);
    cpu.set_reg16(Cpu::EBX, 0x55aa);
    call(0x80, 0x41);
    require(!cpu.flag(Cpu::CF) && cpu.reg8(4) == 0x30 &&
            cpu.reg16(Cpu::EBX) == 0xaa55 && cpu.reg16(Cpu::ECX) == 1,
            "EDD discovery of the fixed QSSD returned an invalid signature");
    cpu.write16(params, 25);
    call(0x80, 0x48);
    require(cpu.flag(Cpu::CF) && cpu.read16(params) == 25 && cpu.read8(0x474) == 1,
            "Short EDD parameter buffer did not fail atomically");
    call(0x80, 0x01);
    require(cpu.flag(Cpu::CF) && cpu.reg8(4) == 1, "Fixed disk lost its last BIOS status");
    call(0xe0, 0x08);
    require(cpu.flag(Cpu::CF) && cpu.reg8(4) == 1, "Optical CD must reject legacy CHS query");
    call(0xe0, 0x15);
    require(!cpu.flag(Cpu::CF) && cpu.reg8(4) == 1 && cpu.reg8(Cpu::EDX) == 0xe0 &&
            cpu.read8(0x474) == 1, "Optical probe changed fixed-disk BIOS status");
    cpu.write16(params, 26);
    call(0x80, 0x48);
    require(!cpu.flag(Cpu::CF) && cpu.read16(params) == 26 && cpu.read16(params+2) == 2 &&
            cpu.read64(params+16) == 134217728 && cpu.read16(params+24) == 512,
            "BIOS did not enumerate 64 GiB of 512-byte QSSD sectors");
    call(0x80, 0x7f);
    require(cpu.flag(Cpu::CF) && cpu.read8(0x474) == 1, "Expected a pending fixed-disk error");
    pc.reset();
    require(cpu.read8(0x475) == 1 && cpu.reg8(Cpu::EDX) == 0xe0,
            "Reset lost the registered fixed disk or optical boot drive");
    call(0x80, 0x01);
    require(!cpu.flag(Cpu::CF) && cpu.reg8(4) == 0, "Reset retained fixed-disk BIOS error");
  }
  require(cpu.read8(0x475) == 0, "Removing the bridge left stale BIOS fixed-disk count");
  require(disk.stats().commits == 0, "BIOS disk discovery wrote to the QSSD");
  std::cout << "C03_BIOS_CD_QSSD_DISCOVERY_PASS boot_drive=e0 fixed_drive=80 sectors=134217728"
               " sector_bytes=512 reset_preserved=1 commits=0\n";
}
}

int main() {
  auto path = std::filesystem::temp_directory_path() /
      ("reverse-windows-c01-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) + ".rvdisk");
  try {
    verify_fixed_disk_during_cd_boot(path);
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
      const std::array<std::uint8_t, 35> guest{
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

    // Simulate the reboot phase after installation: an MBR written into the
    // persistent HDD is fetched through the SAME original PcMachine raw
    // boot path, and actual x86 executes it rather than displaying fake UI.
    {
      reverse_windows::DiskImage image(path);
      std::array<std::uint8_t,512> mbr{};
      mbr[0] = 0xb8; mbr[1] = 0x34; mbr[2] = 0x12; // MOV AX,1234
      mbr[3] = 0xa3; mbr[4] = 0x00; mbr[5] = 0x91; // MOV [9100],AX
      mbr[6] = 0xf4;                              // HLT
      mbr[510] = 0x55; mbr[511] = 0xaa;
      require(image.write(0, 1, mbr.data()), "Cannot persist MBR boot sector");

      cosmic_pc::PcMachine pc(2048ULL*1024*1024, 256ULL*1024*1024);
      pc.mount([&image](std::uint64_t offset, std::uint8_t* out, std::size_t length) {
        return image.read_bytes(offset, out, length);
      }, image.bytes(), "raw");
      reverse_windows::BiosHardDisk bridge(pc, image);
      pc.run(100);
      require(!pc.cpu.fault && pc.cpu.halted && pc.boot_kind == "raw",
              "HDD did not boot original x86 PC in real mode");
      require(pc.cpu.memory[0x9100] == 0x34 && pc.cpu.memory[0x9101] == 0x12,
              "Guest MBR machine code did not execute from persistent HDD");
      std::cout << "REVERSE_WINDOWS_HDD_REBOOT_REAL_X86_PASS"
                << " instructions=" << pc.cpu.instructions << '\n';
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
