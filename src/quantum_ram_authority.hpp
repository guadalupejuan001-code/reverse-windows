#pragma once

#include "ether_state_ram.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace reverse_windows {

// Selected guest bytes are views of this owner's QuantumRuntime/InverseMemory.
// EtherStateRam stores no second authoritative byte for a bound address. The
// finite quantum component may use a tableau, adaptive vectors, reduced state
// or MPS; representation changes transfer ownership, not state synchronization.
// All physical computation and finite allocations still run on the host.
class QuantumRamAuthority final : public cosmic_pc::EtherStateRam::QuantumAuthority {
 public:
  static constexpr std::uint64_t kLogicalBytes = 64ULL << 30;
  struct Limits {
    std::uint64_t logical_bytes = kLogicalBytes;
    std::size_t max_bound_bytes = 4096;
    std::size_t max_active_qubits = 32768;
    std::size_t max_component_qubits = 1024;
    std::size_t max_state_bytes = 64U << 20;
    std::size_t max_reduced_components = 65536;
    std::size_t max_dense_qubits = 8;
  };
  struct Stats {
    std::uint64_t logical_bytes = 0, revision = 0, core_revision = 0;
    std::size_t bound_bytes = 0, classical_overrides = 0, active_qubits = 0;
    std::size_t clifford_components = 0, adaptive_components = 0;
    std::size_t reduced_components = 0, mps_components = 0;
    std::size_t coefficients = 0, factor_amplitudes = 0, t_instructions = 0;
    // Committed quantum payload only; excludes index metadata, candidates,
    // allocator overhead and the rest of the Android process.
    std::size_t payload_bytes = 0;
    double norm = 1.;
  };

  QuantumRamAuthority();
  explicit QuantumRamAuthority(Limits limits);
  ~QuantumRamAuthority() override;
  QuantumRamAuthority(const QuantumRamAuthority&) = delete;
  QuantumRamAuthority& operator=(const QuantumRamAuthority&) = delete;

  bool contains(std::uint64_t address) const noexcept override;
  bool intersects(std::uint64_t first, std::size_t count) const noexcept override;
  std::vector<std::uint64_t> bound_addresses(std::uint64_t first, std::size_t count) const override;
  std::uint8_t read(std::uint64_t address) const override;
  void write(const std::vector<std::pair<std::uint64_t, std::uint8_t>>& values) override;
  std::uint64_t revision() const noexcept override;
  std::size_t state_bytes() const noexcept override;
  void reset() noexcept override;
  void begin_write_scope() noexcept override;
  void commit_write_scope() noexcept override;
  void end_write_scope() noexcept override;
  bool restore_write_scope() noexcept override;
  std::unique_ptr<LocalCheckpoint> checkpoint() override;

  // Bind initializes eight disjoint qubit addresses from the previous guest
  // byte. It rejects a quantum initial state instead of measuring implicitly.
  // The caller transfers the classic byte out of EtherStateRam on success.
  void bind(std::uint64_t address, const std::string& qbase, std::uint8_t initial_value);
  // Returns the deterministic value for an explicit transfer back to base RAM.
  // The caller must publish that value before removing its RAM-side binding.
  std::uint8_t unbind(std::uint64_t address);

  // Standard XI gates, observations and explicit measurements operate on the
  // same runtime. Binding/reset commands must use this typed owner API; memory
  // reads/stores are accepted only for bound guest addresses. No implicit copy.
  std::string execute_xi(const std::string& program);
  double probability_one(const std::string& qaddress) const;
  Stats stats() const;
  std::string snapshot() const;
  void restore(const std::string& saved);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace reverse_windows
