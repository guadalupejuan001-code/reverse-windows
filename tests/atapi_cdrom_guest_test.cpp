#include "c06_motherboard.hpp"
#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using cosmic_pc::Cpu;

void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

void protected_mode(cosmic_pc::PcMachine& machine) {
  machine.boot_long_mode_demo();
  for (unsigned i = 0; i < 10000; ++i) {
    if (machine.cpu.step() == cosmic_pc::StepResult::Faulted)
      throw std::runtime_error(machine.cpu.fault->describe());
    if (std::string(machine.cpu.mode_name()) == "protected32" &&
        machine.cpu.segments[Cpu::SS] == 0x10) return;
  }
  throw std::runtime_error("Guest did not enter protected32");
}

class Program {
 public:
  void emit(std::initializer_list<std::uint8_t> code) {
    bytes.insert(bytes.end(), code.begin(), code.end());
  }
  void dword(std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) emit({std::uint8_t(value >> (8 * i))});
  }
  void port(std::uint16_t port) { emit({0x66, 0xba, std::uint8_t(port), std::uint8_t(port >> 8)}); }
  void out8(std::uint16_t target, std::uint8_t value) { port(target); emit({0xb0, value, 0xee}); }
  void in8(std::uint16_t source, std::uint32_t address) {
    port(source); emit({0xec, 0xa2}); dword(address);
  }
  void packet(std::array<std::uint8_t,12> cdb, unsigned transfer_limit) {
    out8(0x171, 0);
    out8(0x174, std::uint8_t(transfer_limit));
    out8(0x175, std::uint8_t(transfer_limit >> 8));
    out8(0x177, 0xa0);
    port(0x170);
    for (std::size_t i = 0; i < cdb.size(); i += 2)
      emit({0x66, 0xb8, cdb[i], cdb[i + 1], 0x66, 0xef});
  }
  void words(unsigned count, std::uint32_t address) {
    port(0x170);
    emit({0xbf}); dword(address); // EDI
    emit({0xb9}); dword(count);   // ECX
    emit({0xfc, 0xf3, 0x66, 0x6d}); // CLD; REP INSW
  }
  std::vector<std::uint8_t> bytes;
};
} // namespace

int main() {
  try {
    cosmic_pc::PcMachine machine(16 * 1024 * 1024);
    protected_mode(machine);
    auto& cpu = machine.cpu;
    std::array<std::uint8_t,4096> cd{};
    for (unsigned i = 0; i < cd.size(); ++i) cd[i] = std::uint8_t(i * 29 + i / 2048);
    reverse_windows::PcMotherboardC06 board(machine, nullptr,
        [&cd](auto offset, auto* target, auto count) {
          if (offset > cd.size() || count > cd.size() - offset) return false;
          std::copy_n(cd.data() + offset, count, target);
          return true;
        }, cd.size());

    // Remap both PICs. IRQ15 runs a real protected-mode ISR and returns by IRETD.
    cpu.idtr = {0x1000, 0x15000};
    cpu.regs[Cpu::ESP] = 0x190000;
    cpu.write64(cpu.idtr.base + 0x2f * 8, 0x00018e0000083000ULL);
    const std::array<std::uint8_t,14> isr{
      0xff,0x05,0x40,0x90,0,0, // INC dword [9040h]
      0x50,0xb0,0x20,0xe6,0xa0,0xe6,0x20,0x58}; // save EAX, EOI, restore
    cpu.memory.write_block(0x13000, isr.data(), isr.size());
    cpu.write8(0x13000 + isr.size(), 0xcf);

    Program program;
    program.out8(0x20,0x11); program.out8(0xa0,0x11);
    program.out8(0x21,0x20); program.out8(0xa1,0x28);
    program.out8(0x21,0x04); program.out8(0xa1,0x02);
    program.out8(0x21,0x01); program.out8(0xa1,0x01);
    program.out8(0x21,0xfb); program.out8(0xa1,0x7f);
    program.emit({0xfb}); // STI
    program.out8(0x176,0xa0);
    program.in8(0x174,0x9000); program.in8(0x175,0x9001);
    program.out8(0x177,0xa1);
    program.words(256,0x10000);
    program.packet({0x25},2048);
    program.words(4,0x10200);
    program.packet({0x28,0,0,0,0,0,0,0,2},512); // READ (10), two sectors.
    program.in8(0x172,0x9002); program.in8(0x175,0x9003);
    program.words(2048,0x11000);
    program.in8(0x177,0x9004);
    program.packet({0x2a,0,0,0,0,0,0,0,1},2048);
    program.in8(0x177,0x9005);
    program.packet({0x03,0,0,0,18},2048);
    program.words(9,0x10210);
    program.emit({0xfa,0xf4});
    cpu.memory.write_block(0xa000,program.bytes.data(),program.bytes.size());
    cpu.eip = 0xa000;
    cpu.halted = false;
    machine.run(10000);
    if (cpu.fault) throw std::runtime_error(cpu.fault->describe());
    require(cpu.halted, "Protected-mode optical guest did not finish");
    require(cpu.read16(0x9000) == 0xeb14, "ATAPI reset signature not visible");
    require(cpu.read16(0x10000) == 0x8580, "IDENTIFY PACKET missing removable CD");
    require(cpu.read32(0x10200) == 0x01000000 && cpu.read32(0x10204) == 0x00080000,
            "READ CAPACITY data wrong");
    require(cpu.read16(0x9002) == 0x0202, "PIO data phase or 512-byte limit wrong");
    for (unsigned i = 0; i < cd.size(); ++i)
      require(cpu.read8(0x11000 + i) == cd[i], "Optical read changed a sector byte");
    require(cpu.read8(0x9004) == 0x40, "READ (10) did not finish");
    require((cpu.read8(0x9005) & 1) && cpu.read8(0x10212) == 7 &&
            cpu.read8(0x1021c) == 0x27, "ISO write lacked DATA PROTECT sense");
    require(cpu.read32(0x9040) >= 8, "IRQ15 did not reach the guest IDT");
    std::cout << "REVERSE_WINDOWS_ATAPI_PROTECTED_GUEST_PASS bytes=" << cd.size()
              << " irq15=" << cpu.read32(0x9040) << " readonly=true\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "REVERSE_WINDOWS_ATAPI_PROTECTED_GUEST_FAIL " << error.what() << '\n';
    return 1;
  }
}
