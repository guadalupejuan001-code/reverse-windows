#include "pc_machine.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>

// Run the same two GS loads observed in Windows, using each release's normal
// dispatch. The APIC and disk are outside this instruction-throughput fixture.
int main() {
  using namespace cosmic_pc;
  PcMachine pc;
  pc.boot_long_mode_demo();
  pc.run(4000);
  auto& cpu = pc.cpu;
  if (!cpu.is_long_mode() || !cpu.halted || cpu.fault) return 1;
  constexpr unsigned code = 0xa000, base = 0x18000, loops = 50000;
  constexpr std::uint8_t program[] = {
    0x65,0x8b,0x0c,0x25,0xa4,1,0,0,
    0x65,0x48,0x8b,0x04,0x25,0x20,0,0,0,
    0x48,0x8d,0x04,0x08,0xff,0xca,0x75,0xe7,0xf4};
  cpu.memory.write_block(code, program, sizeof(program));
  cpu.write32(base+0x1a4, 0xabc);
  cpu.write64(base+0x20, 0x1234567890ULL);
  cpu.enable_page_translation_cache();
  cpu.enable_ether_arm64_blocks(true);
  pc.enable_boot_acceleration();
  cpu.halted = false;
  cpu.eip = code;
  cpu.gs_base = base;
  cpu.regs[Cpu::EDX] = loops;
  const auto before = cpu.instructions;
  const auto native_before = cpu.ether_arm64_stats().steps;
  const auto start = std::chrono::steady_clock::now();
  constexpr unsigned steps = loops*5 + 1;
  for (unsigned left=steps; left;) {
    const auto grant = std::min(left, PcMachine::kDefaultRunSteps);
    pc.run(grant);
    left -= grant;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-start).count();
  if (!cpu.halted || cpu.fault || cpu.instructions-before != steps || cpu.regs[Cpu::EDX] ||
      cpu.regs[Cpu::EAX] != 0x123456834cULL || cpu.regs[Cpu::ECX] != 0xabc)
    throw std::runtime_error("GS boot loop changed guest state");
  std::cout << "PRIME_BOOT_GS_PASS steps=" << steps << " native=" << cpu.ether_arm64_stats().steps-native_before
            << " checksum=" << cpu.regs[Cpu::EAX] << " elapsed_ns=" << elapsed << '\n';
}
