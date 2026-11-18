#pragma once

#include "c06_motherboard.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace reverse_windows {

struct EtherX86Check {
  unsigned owners = 0, requested_jobs = 0, completed_jobs = 0, host_workers = 0;
  std::uint64_t guest_steps = 0, projected_steps = 0, elapsed_ns = 0;
  std::string error;

  bool passed() const { return requested_jobs != 0 && completed_jobs == requested_jobs && error.empty(); }
};

// The hardware check uses the same independent jobs as the ARM64 performance
// fixture. Every job reads and writes its own private x86 RAM.
inline EtherX86Check check_ether_x86_sessions(PcMotherboardC06& board) {
  using cosmic_pc::Cpu;
  using Clock = std::chrono::steady_clock;
  constexpr unsigned owners = 16, per_owner = 2, count = owners * per_owner;
  constexpr std::uint8_t code[] = {
      0x48,0x8b,0x04,0x25,0,0x90,0,0, // MOV RAX,[9000h]
      0x48,0x01,0xf0,                 // ADD RAX,RSI
      0x48,0xd3,0xcb,                 // ROR RBX,CL
      0x48,0xff,0xc6,                 // INC RSI
      0x83,0xe9,1,                    // SUB ECX,1
      0x75,0xf2,                      // JNZ back to ADD
      0x48,0x89,0x04,0x25,0,0x90,0,0, // MOV [9000h],RAX
      0xf4                            // HLT
  };
  EtherX86Check check;
  check.owners = owners;
  check.requested_jobs = count;
  try {
    board.configure_ether_x86_sessions(count, per_owner);
    check.host_workers = board.ether_x86_session_workers();
    if (check.host_workers != count)
      throw std::runtime_error("No se pudieron reservar 32 trabajadores para la prueba.");

    std::array<EtherX86Sessions::Job, count> jobs;
    std::array<std::array<std::uint64_t, 16>, count> expected;
    std::array<unsigned, count> expected_steps;
    for (unsigned i = 0; i < count; ++i) {
      auto& job = jobs[i];
      const auto key = i + 1;
      const unsigned rounds = 1048576 + 19 * key + 1048576 * (i % per_owner);
      const std::uint64_t base = 241 * key;
      const std::uint64_t initial = 0x5ad33000ULL + key;
      const std::uint64_t rotor = 0xa735f090cd0071cdULL ^ key;
      job.code.assign(code, code + sizeof(code));
      job.data.resize(8);
      for (unsigned byte = 0; byte < job.data.size(); ++byte)
        job.data[byte] = std::uint8_t(initial >> (8 * byte));
      job.registers[Cpu::ESI] = base;
      job.registers[Cpu::ECX] = rounds;
      job.registers[Cpu::EBX] = rotor;
      job.step_limit = expected_steps[i] = 5 * rounds + 3;
      auto& registers = expected[i];
      registers = job.registers;
      registers[Cpu::EAX] = initial + base * rounds + std::uint64_t(rounds) * (rounds - 1) / 2;
      registers[Cpu::ESI] = base + rounds;
      registers[Cpu::ECX] = 0;
      const auto angle = unsigned(std::uint64_t(rounds) * (rounds + 1) / 2) & 63U;
      registers[Cpu::EBX] = angle ? (rotor >> angle) | (rotor << (64 - angle)) : rotor;
    }

    std::array<std::optional<EtherX86Sessions::Ticket>, count> tickets;
    const auto start = Clock::now();
    for (unsigned i = 0; i < count; ++i) {
      tickets[i] = board.submit_ether_x86_session(i / per_owner, std::move(jobs[i]));
      if (!tickets[i]) throw std::runtime_error("No se pudo iniciar una sesión Éter x86.");
    }
    for (unsigned i = 0; i < count; ++i) {
      const auto result = tickets[i]->result.get();
      if (!result.fault.empty()) throw std::runtime_error(result.fault);
      if (!result.halted || result.cancelled || result.steps != expected_steps[i] ||
          result.rip != EtherX86Sessions::kCodeAddress + sizeof(code) || result.flags != 0x46 ||
          result.registers != expected[i] || result.data.size() != 8)
        throw std::runtime_error("El estado x86 de una sesión no coincide con el resultado esperado.");
      std::uint64_t word = 0;
      for (unsigned byte = 0; byte < result.data.size(); ++byte)
        word |= std::uint64_t(result.data[byte]) << (8 * byte);
      if (word != expected[i][Cpu::EAX])
        throw std::runtime_error("El resultado en la RAM privada no coincide con el esperado.");
      ++check.completed_jobs;
      check.guest_steps += result.steps;
      check.projected_steps += result.projected_steps;
    }
    check.elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
  } catch (const std::exception& error) {
    check.error = error.what();
  }
  return check;
}

} // namespace reverse_windows
