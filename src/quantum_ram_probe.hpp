#pragma once

#include "memory_fabric.hpp"
#include "quantum_ram_authority.hpp"
#include "x86_cpu.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>

namespace reverse_windows {

struct QuantumRamProbeResult {
  bool passed = false;
  bool phase_interference = false;
  bool rejects_unmeasured = false;
  std::string backend;
  std::uint64_t logical_bytes = 0;
  std::uint64_t guest_steps = 0;
  unsigned initial_value = 0, loaded_value = 0, stored_value = 0;
  std::size_t bound_bytes = 0, state_bytes = 0, adaptive_components = 0;

  void write_json(std::ostream& out) const {
    out << "{\"passed\":" << (passed ? "true" : "false")
        << ",\"memory_backend\":\"" << backend << "\""
        << ",\"logical_bytes\":" << logical_bytes
        << ",\"phase_interference\":" << (phase_interference ? "true" : "false")
        << ",\"rejects_unmeasured\":" << (rejects_unmeasured ? "true" : "false")
        << ",\"phase_adaptive_components\":" << adaptive_components
        << ",\"guest_steps\":" << guest_steps
        << ",\"initial_value\":" << initial_value
        << ",\"loaded_value\":" << loaded_value
        << ",\"stored_value\":" << stored_value
        << ",\"bound_bytes\":" << bound_bytes
        << ",\"state_bytes\":" << state_bytes << '}';
  }
};

// The fabric preflights the destination, transfers its previous byte to the
// provider and removes its old basis value only after binding succeeds.
inline void bind_quantum_ram_byte(cosmic_pc::MemoryFabric& memory,
                                 const std::shared_ptr<QuantumRamAuthority>& authority,
                                 std::uint64_t address, const std::string& qbase) {
  memory.bind_quantum_byte(address, [&](std::uint8_t previous) {
    authority->bind(address, qbase, previous);
  });
}

// A fresh, bounded machine makes this suitable for Android's hardware check.
// No installed guest, disk or running session is read or modified. The test
// measures functional semantics, never a physical frequency or host capacity.
inline QuantumRamProbeResult run_quantum_ram_guest_probe() {
  using cosmic_pc::Cpu;
  using cosmic_pc::MemoryFabric;
  using cosmic_pc::StepResult;
  const auto require = [](bool okay, const char* reason) {
    if (!okay) throw std::runtime_error(reason);
  };
  MemoryFabric memory(QuantumRamAuthority::kLogicalBytes, 16 * MemoryFabric::kPageSize);
  auto authority = std::make_shared<QuantumRamAuthority>();
  memory.attach_quantum_authority(authority);

  QuantumRamProbeResult result;
  result.backend = memory.backend_name();
  result.logical_bytes = memory.size();
  require(result.backend == "ether-state-ram", "quantum RAM probe selected a different backend");

  constexpr std::uint64_t source = 0x9000, destination = source + 1, code = 0x7c00;
  result.initial_value = 40;
  memory.write_byte(source, std::uint8_t(result.initial_value));
  bind_quantum_ram_byte(memory, authority, source, "0");
  bind_quantum_ram_byte(memory, authority, destination, "8");

  // H*T*H|0> has P(1)=sin(pi/8)^2. This exercises the adaptive vector
  // representation and its relative phase before returning to a readable byte.
  authority->execute_xi("h 0\nt 0\nh 0\n");
  const auto phase = authority->stats();
  const auto expected = std::pow(std::sin(std::acos(-1.) / 8.), 2);
  result.phase_interference = std::abs(authority->probability_one("0") - expected) < 2e-12 &&
                              std::abs(phase.norm - 1.) < 2e-12;
  result.adaptive_components = phase.adaptive_components;
  try { (void)memory.read_byte(source); }
  catch (const std::runtime_error&) { result.rejects_unmeasured = true; }
  require(result.phase_interference && result.adaptive_components != 0 && result.rejects_unmeasured,
          "quantum RAM probe failed phase or explicit-measurement semantics");
  authority->execute_xi("h 0\ntdg 0\nh 0\nx 0\n");

  // Real x86 instructions load the changed authoritative byte, calculate and
  // store through MemoryFabric into the second quantum-backed guest byte.
  constexpr std::uint8_t program[]{
      0xa0, 0x00, 0x90,  // MOV AL,[9000h]
      0x04, 0x01,        // ADD AL,1
      0xa2, 0x01, 0x90,  // MOV [9001h],AL
      0xf4};             // HLT
  memory.write_block(code, program, sizeof(program));
  Cpu cpu(memory);
  cpu.reset(0, code);
  require(cpu.step() == StepResult::Executed, "quantum RAM x86 load failed");
  result.loaded_value = unsigned(cpu.regs[Cpu::EAX] & 0xffU);
  require(cpu.step() == StepResult::Executed && cpu.step() == StepResult::Executed &&
              cpu.step() == StepResult::Halted && !cpu.fault,
          "quantum RAM x86 arithmetic/store failed");
  result.guest_steps = cpu.instructions;
  result.stored_value = authority->read(destination);
  const auto final = authority->stats();
  result.bound_bytes = final.bound_bytes;
  result.state_bytes = authority->state_bytes();
  result.passed = result.loaded_value == 41 && result.stored_value == 42 &&
                  memory.read_byte(destination) == 42 && final.classical_overrides == 0 &&
                  result.bound_bytes == 2 && result.guest_steps == 4;
  require(result.passed, "x86 did not load and store the authoritative quantum bytes");
  return result;
}

}  // namespace reverse_windows
