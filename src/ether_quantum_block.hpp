#pragma once

#include "c08_gpu_protocol.h"
#include "pc_machine.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace reverse_windows {
inline unsigned apply_quantum_block(mundo_x::InfiniteSession& session, const std::uint8_t* records,
                                    std::size_t bytes) {
  constexpr auto bits = RWGPU_LOGICAL_BYTES * 8;
  constexpr auto base = cosmic_pc::PcMachine::kEtherMemoryLogicalByteBase * 8;
  if (!records || !bytes || bytes % sizeof(RwGpuQuantumGate) || bytes / sizeof(RwGpuQuantumGate) > RWGPU_QUANTUM_MAX_GATES)
    return RWGPU_INVALID;
  std::vector<mundo_x::InfiniteSession::Gate> gates;
  gates.reserve(bytes / sizeof(RwGpuQuantumGate));
  for (std::size_t at = 0; at < bytes; at += sizeof(RwGpuQuantumGate)) {
    RwGpuQuantumGate gate;
    std::memcpy(&gate, records + at, sizeof(gate));
    if (gate.reserved || gate.first >= bits) return RWGPU_INVALID;
    std::string target;
    switch (gate.opcode) {
      case RWGPU_GATE_CX: case RWGPU_GATE_CZ:
        if (gate.second >= bits || gate.first == gate.second) return RWGPU_INVALID;
        target = std::to_string(base + gate.second);
        break;
      case RWGPU_GATE_H: case RWGPU_GATE_S: case RWGPU_GATE_X: case RWGPU_GATE_Y: case RWGPU_GATE_Z:
        if (gate.second) return RWGPU_INVALID;
        break;
      default: return RWGPU_INVALID;
    }
    gates.push_back({static_cast<char>(gate.opcode), std::to_string(base + gate.first), std::move(target)});
  }
  // InfiniteCore validates the complete circuit and commits once. A CPU observation,
  // DMA fence or MSI can only see the state before or after this command.
  try {
    session.apply(gates);
  } catch (const std::length_error&) {
    return RWGPU_QUANTUM_UNAVAILABLE;
  } catch (const std::runtime_error&) {
    return RWGPU_QUANTUM_UNAVAILABLE;
  }
  return RWGPU_OK;
}
} // namespace reverse_windows
