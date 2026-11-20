#include "quantum_ram_authority.hpp"
// This is intentionally the unextended public Mundo X runtime. The private
// RAM profile must coexist with it without changing its ABI or behavior.
#include "mundo_x/quantum_runtime.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Authority = reverse_windows::QuantumRamAuthority;
void check(bool okay, const char* message) {
  if (!okay) throw std::runtime_error(message);
}
void close(double actual, double expected, const char* message) {
  if (std::abs(actual - expected) > 2e-12) throw std::runtime_error(message);
}
template<class Action> void rejects(Action action, const char* message) {
  try { action(); } catch (const std::exception&) { return; }
  throw std::runtime_error(message);
}
template<class Action> void run(const char* name, Action action) {
  try { action(); }
  catch (const std::exception& error) { throw std::runtime_error(std::string(name) + ": " + error.what()); }
}
bool same_quantum_payload(const std::string& a, const std::string& b) {
  // RWQR1 header, capacity, monotonic owner revision, two authority records,
  // checksum. A rollback changes only the invalidation revision/checksum.
  return a.size() >= 29 && a.size() == b.size() &&
      a.compare(0, 13, b, 0, 13) == 0 &&
      a.compare(21, a.size() - 29, b, 21, b.size() - 29) == 0;
}

void capacity_and_bytes() {
  Authority owner;
  check(owner.stats().logical_bytes == (64ULL << 30), "logical RAM capacity");
  check(owner.state_bytes() == 0 && owner.stats().active_qubits == 0,
        "logical capacity must not allocate all guest bytes");
  const auto last = Authority::kLogicalBytes - 1;
  owner.bind(last, "9000", 41);
  owner.bind(7, "16", 0xa5);
  check(owner.contains(last) && !owner.contains(last - 1), "binding index");
  check(owner.intersects(last - 3, 4) && !owner.intersects(last - 3, 3), "range boundary");
  check(!owner.intersects(last, 0), "empty range");
  check(owner.bound_addresses(last - 2, 3) == std::vector<std::uint64_t>{last}, "bound range projection");
  check(owner.read(last) == 41 && owner.read(7) == 0xa5, "initial quantum basis byte");
  owner.write({{last, 42}, {7, 0xff}});
  check(owner.read(last) == 42 && owner.read(7) == 0xff, "typed byte transaction");
  check(owner.stats().classical_overrides == 0, "no second classical byte authority");
  rejects([&] { owner.bind(Authority::kLogicalBytes, "32", 0); }, "capacity bound was ignored");
  rejects([&] { owner.bind(8, "18", 0); }, "overlapping quantum byte ranges were allowed");
  const auto snapshot = owner.snapshot();
  rejects([&] { owner.write({{7, 3}, {8, 2}}); }, "unbound byte store was allowed");
  check(owner.snapshot() == snapshot, "failed byte transaction changed state");
  check(owner.unbind(last) == 42 && !owner.contains(last), "explicit unbind projection");
  owner.restore(snapshot);
  check(owner.read(last) == 42, "restored binding index/value");
  owner.reset();
  check(!owner.contains(7) && owner.state_bytes() == 0, "reset retained a guest authority");
}

void vectors_and_involution_optimizer() {
  Authority owner;
  owner.bind(0, "0", 0);
  owner.execute_xi("begin\nh 0\nt 0\nh 0\nend\n");
  // Direct analytic oracle for H*T*H|0>, independent of the compact engine.
  close(owner.probability_one("0"), std::pow(std::sin(std::acos(-1.) / 8.), 2),
        "H T H phase interference");
  const auto vector_stats = owner.stats();
  check(vector_stats.adaptive_components > 0 && vector_stats.factor_amplitudes >= 2,
        "non-Clifford operation never transferred to vector authority");
  close(vector_stats.norm, 1., "state-vector norm");
  rejects([&] { (void)owner.read(0); }, "quantum byte was projected without measurement");
  owner.execute_xi("begin\nh 0\ntdg 0\nh 0\nend\n");
  check(owner.read(0) == 0, "inverse gates did not restore byte");

  owner.reset();
  owner.bind(0, "0", 0);
  owner.execute_xi("begin\nh 0\nt 0\nt 0\nh 0\nend\n");
  close(owner.probability_one("0"), .5, "T T was incorrectly cancelled as an involution");
  check(owner.stats().t_instructions == 2, "T instructions disappeared from current component");
  owner.execute_xi("begin\nh 0\ntdg 0\ntdg 0\nh 0\nend\n");
  check(owner.read(0) == 0, "T-dagger pair semantics");
  owner.execute_xi("begin\nh 0\nt 0\ntdg 0\nh 0\nend\n");
  check(owner.read(0) == 0, "T and T-dagger are not inverse");
}

void transactions_and_ownership() {
  Authority owner;
  owner.bind(11, "0", 5);
  owner.bind(12, "8", 0);
  owner.execute_xi("h 8\nt 8\n");
  const auto before = owner.snapshot();
  const auto revision = owner.revision();
  rejects([&] { owner.write({{11, 99}, {12, 2}}); }, "superposed byte accepted a classical store");
  check(owner.read(11) == 5 && owner.snapshot() == before && owner.revision() == revision,
        "failed multi-byte quantum store committed a prefix");
  rejects([&] { owner.execute_xi("x 0\nthis_command_does_not_exist\n"); }, "invalid XI program succeeded");
  check(owner.snapshot() == before, "failed XI transaction committed gates");
  rejects([&] { owner.execute_xi("x 0\n  MeM_BiNd 77 99 # indirect binding mutation\n"); },
          "XI bypassed the numeric binding index");
  rejects([&] { owner.execute_xi("mem_store 999 17\n"); }, "XI created an unbound classical override");
  rejects([&] { owner.execute_xi("mem_read 999\n"); }, "XI misrepresented an unbound guest byte as zero");
  rejects([&] { owner.execute_xi("mem_materialize 11 3\n"); }, "XI crossed from quantum into base RAM without its authority");
  check(owner.snapshot() == before, "rejected ownership change mutated state");
  owner.execute_xi("tdg 8\nh 8\nmem_store 11 7\n");
  check(owner.read(11) == 7 && owner.read(12) == 0, "XI and typed bytes have different authorities");

  const auto journal_start = owner.snapshot();
  owner.begin_write_scope();
  owner.write({{11, 91}, {12, 2}});
  owner.execute_xi("h 8\nt 8\n");
  const auto provisional_revision = owner.revision();
  rejects([&] { owner.bind(13, "32", 0); }, "binding changed inside pinned guest write scope");
  check(owner.restore_write_scope() && owner.restore_write_scope(), "runtime journal checkpoint unavailable");
  owner.write({{11, 55}});
  check(owner.restore_write_scope() && owner.read(11) == 7, "second rollback left a post-fault store committed");
  owner.end_write_scope();
  check(same_quantum_payload(owner.snapshot(), journal_start), "journal rollback lost phases, RNG or byte values");
  check(owner.revision() > provisional_revision, "journal rollback reused an observed cache revision");
  check(!owner.restore_write_scope(), "journal checkpoint leaked outside its lifetime");

  owner.begin_write_scope();
  owner.write({{11, 31}});
  owner.commit_write_scope();
  owner.write({{11, 88}});
  check(owner.restore_write_scope() && owner.read(11) == 31,
        "fault rollback reverted a previously retired guest store");
  owner.end_write_scope();
}

void entanglement_checkpoint_and_rng() {
  Authority owner;
  owner.bind(0, "0", 0);
  owner.bind(1, "8", 0);
  owner.execute_xi("seed 5678\nh 0\ncx 0 8\n");
  close(owner.probability_one("0"), .5, "Bell left marginal");
  close(owner.probability_one("8"), .5, "Bell right marginal");
  rejects([&] { (void)owner.read(0); }, "entangled byte silently measured");
  const auto saved = owner.snapshot();
  const auto first = owner.execute_xi("measure 0\n");
  check(owner.read(0) == owner.read(1), "Bell correlation was lost");
  const auto revision = owner.revision();
  owner.restore(saved);
  check(owner.revision() > revision, "restore did not invalidate owner consumers");
  check(owner.execute_xi("measure 0\n") == first, "checkpoint lost RNG or correlation state");
  const auto before_bad_restore = owner.snapshot();
  auto corrupt = saved;
  corrupt[corrupt.size() / 2] ^= 1;
  rejects([&] { owner.restore(corrupt); }, "corrupt snapshot was accepted");
  check(owner.snapshot() == before_bad_restore, "failed restore changed live authority");
}

void large_component_and_address() {
  Authority owner;
  owner.bind(0, "0", 0);
  owner.bind(1, "8", 0);
  std::string circuit = "h 0\nt 0\n";
  for (unsigned bit = 1; bit <= 8; ++bit) circuit += "cx 0 " + std::to_string(bit) + "\n";
  owner.execute_xi(circuit);
  check(owner.stats().reduced_components > 0, "component did not transfer above the dense-block limit");
  close(owner.probability_one("8"), .5, "reduced component marginal");
  const auto saved = owner.snapshot();
  owner.restore(saved);
  std::string inverse;
  for (unsigned bit = 8; bit; --bit) inverse += "cx 0 " + std::to_string(bit) + "\n";
  owner.execute_xi(inverse + "tdg 0\nh 0\n");
  close(owner.probability_one("0"), 0., "reduced snapshot lost phase interference");
  close(owner.probability_one("8"), 0., "reduced snapshot lost correlated inverse");
  // Reduced coefficients use finite floating arithmetic. A remaining nonzero
  // residual must not be silently rounded into a guest byte; request the
  // projection explicitly before performing a classical load.
  std::string projection;
  for (unsigned bit = 0; bit < 16; ++bit) projection += "postselect " + std::to_string(bit) + " 0\n";
  owner.execute_xi(projection);
  check(owner.read(0) == 0 && owner.read(1) == 0, "reduced snapshot lost phase/correlation");
  owner.reset();
  const std::string huge = "1" + std::string(300, '0');
  owner.bind(Authority::kLogicalBytes - 1, huge, 0);
  owner.execute_xi("x " + huge + "\n");
  check(owner.read(Authority::kLogicalBytes - 1) == 1, "decimal Ether address was truncated");
}

void independent_public_runtime() {
  mundo_x::QuantumRuntime ordinary;
  rejects([&] { ordinary.execute("t 0\n"); }, "private RAM profile replaced the public runtime ABI");
  ordinary.execute("h 0\n");
  close(ordinary.core().probability_one("0"), .5, "public runtime was changed by private profile");
}
}  // namespace

int main() {
  try {
    run("capacity_and_bytes", capacity_and_bytes);
    run("vectors_and_involution_optimizer", vectors_and_involution_optimizer);
    run("transactions_and_ownership", transactions_and_ownership);
    run("entanglement_checkpoint_and_rng", entanglement_checkpoint_and_rng);
    run("large_component_and_address", large_component_and_address);
    run("independent_public_runtime", independent_public_runtime);
    std::cout << "{\"quantum_ram_authority\":\"passed\",\"logical_bytes\":68719476736,"
                 "\"phase_interference\":true,\"T_pair_preserved\":true,"
                 "\"atomic_byte_stores\":true,\"checkpoint_rng\":true,"
                 "\"adaptive_and_reduced\":true,\"private_abi\":true}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "quantum RAM authority: " << error.what() << '\n';
    return 1;
  }
}
