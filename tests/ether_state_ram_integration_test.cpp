#include "quantum_ram_probe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using cosmic_pc::Cpu;
using cosmic_pc::MemoryFabric;
using cosmic_pc::StepResult;
using reverse_windows::QuantumRamAuthority;
using reverse_windows::bind_quantum_ram_byte;
constexpr std::uint64_t kLogical = 64ULL << 30;
constexpr std::size_t kPage = MemoryFabric::kPageSize;
unsigned checks = 0;

void check(bool okay, const char* reason) {
  ++checks;
  if (!okay) throw std::runtime_error(reason);
}
template<class Action> void rejects(Action action, const char* reason) {
  bool rejected = false;
  try { action(); } catch (const std::exception&) { rejected = true; }
  check(rejected, reason);
}

struct Fixture {
  MemoryFabric memory{kLogical, 32 * kPage};
  std::shared_ptr<QuantumRamAuthority> quantum = std::make_shared<QuantumRamAuthority>();
  Fixture() { memory.attach_quantum_authority(quantum); }
  void bind(std::uint64_t address, const char* qbase) {
    bind_quantum_ram_byte(memory, quantum, address, qbase);
  }
};

void ordinary_ram_oracle() {
  MemoryFabric memory(kLogical, 32 * kPage);
  check(std::string(memory.backend_name()) == "ether-state-ram", "default RAM selected the old backend");
  check(memory.size() == kLogical && memory.read_byte(kLogical - 1) == 0,
        "64 GiB logical boundary is not addressable or zero initialized");
  check(memory.allocated_bytes() < kPage, "empty logical RAM allocated its full capacity");
  std::vector<std::uint8_t> expected(6 * kPage, 0), actual(expected.size());
  std::uint64_t rng = 0x5f13dc82b9a74e06ULL;
  const auto next = [&]() {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
  };
  for (unsigned operation = 0; operation < 512; ++operation) {
    const auto address = std::size_t(next() % expected.size());
    const auto count = std::min(std::size_t(1 + next() % 97), expected.size() - address);
    if (operation % 3 == 0) {
      const auto value = std::uint8_t(next());
      memory.fill(address, value, count);
      std::fill_n(expected.begin() + address, count, value);
    } else {
      std::array<std::uint8_t, 97> data{};
      for (std::size_t i = 0; i < count; ++i) data[i] = std::uint8_t(next());
      memory.write_block(address, data.data(), count);
      std::copy_n(data.begin(), count, expected.begin() + address);
    }
  }
  memory.compact_repeating_pages(32);
  memory.read_block(0, actual.data(), actual.size());
  check(actual == expected, "basis RAM diverged from an independent byte-array oracle");
  for (std::size_t address = 0; address < expected.size(); address += 257)
    check(memory.read_byte(address) == expected[address], "scalar projection differs from block RAM");

  const std::array<std::uint8_t, 5> boundary{{0x71, 0x24, 0xb6, 0x09, 0xe3}};
  memory.write_block(kLogical - boundary.size(), boundary.data(), boundary.size());
  std::array<std::uint8_t, 5> recovered{};
  memory.read_block(kLogical - recovered.size(), recovered.data(), recovered.size());
  check(recovered == boundary, "high guest addresses were truncated to 32 bits");
  rejects([&] { memory.write_block(kLogical - 2, boundary.data(), boundary.size()); },
          "out-of-bounds block write was accepted");
  memory.read_block(kLogical - recovered.size(), recovered.data(), recovered.size());
  check(recovered == boundary, "failed range validation committed part of a write");
  memory.read_block(kLogical, nullptr, 0);
  memory.write_block(kLogical, nullptr, 0);
  check(memory.allocated_bytes() <= memory.budget_bytes(), "basis payload exceeded its resident budget");
}

void authoritative_read_and_shortcuts() {
  Fixture f;
  constexpr auto address = kPage + 63;
  f.memory.fill(kPage, 0x30, kPage);
  f.memory.prepare_write(kPage, kPage);
  check(f.memory.resident_ram_span(address - 2, 8) != nullptr, "dense fixture has no direct span");
  f.bind(address, "0");
  f.quantum->execute_xi("x 0\n");
  check(f.memory.read_byte(address) == 0x31 && f.quantum->read(address) == 0x31,
        "external gate was hidden by an old basis byte");
  check(f.memory.resident_ram_span(address - 2, 8) == nullptr,
        "resident span exposed an obsolete quantum-backed byte");
  check(f.memory.resident_ram_span(address + 1, 8) != nullptr,
        "unbound resident bytes unnecessarily lost their direct read path");
  const std::array<std::uint8_t, 8> expected{{0x30, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30}};
  std::array<std::uint8_t, 8> normal{}, parallel{};
  f.memory.read_block(address - 2, normal.data(), normal.size());
  f.memory.read_parallel_ram_block(address - 2, parallel.data(), parallel.size());
  check(normal == expected && parallel == expected, "block readers bypassed the quantum authority");
  const auto scanned = f.memory.scan_ram_until_byte(address - 2, 8, 0x31);
  check(scanned.examined == 3 && scanned.last == 0x31, "scan bypassed the changed quantum byte");

  std::vector<std::future<void>> readers;
  for (unsigned lane = 0; lane < 4; ++lane) readers.push_back(std::async(std::launch::async, [&] {
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
      std::array<std::uint8_t, 8> bytes{};
      f.memory.read_parallel_ram_block(address - 2, bytes.data(), bytes.size());
      if (bytes != expected) throw std::runtime_error("concurrent reader saw a different quantum byte");
    }
  }));
  for (auto& reader : readers) reader.get();
  check(true, "stable authority supports joined parallel readers");

  f.memory.prepare_write(3 * kPage, kPage);
  check(!f.memory.try_copy_resident_block(address - 2, 3 * kPage, 8) &&
            f.memory.read_byte(3 * kPage) == 0,
        "resident copy bypassed a quantum source");
  check(!f.memory.try_fill_repeating_page(kPage, 0x1111111111111111ULL) &&
            f.memory.read_byte(address) == 0x31,
        "compact fill bypassed a quantum destination");
  check(!f.memory.try_copy_to_implicit_page(kPage, 4 * kPage) &&
            f.memory.read_byte(4 * kPage) == 0,
        "implicit-page copy bypassed a quantum source");
  f.bind(5 * kPage, "8");
  f.quantum->execute_xi("x 8\n");
  f.memory.compact_repeating_page(5 * kPage);
  check(!f.memory.try_copy_compact_page(5 * kPage, 6 * kPage) &&
            f.memory.read_byte(6 * kPage) == 0,
        "zero-basis compact copy discarded its live quantum cell");
  f.memory.write_byte(7 * kPage, 0xa5);
  rejects([&] { f.bind(7 * kPage, "0"); }, "overlapping quantum address ranges were accepted");
  check(!f.quantum->contains(7 * kPage) && f.memory.read_byte(7 * kPage) == 0xa5,
        "failed binding discarded its original basis byte");

  f.quantum->execute_xi("h 0\n");
  const auto saved = f.quantum->snapshot();
  rejects([&] {
    f.memory.unbind_quantum_byte(address, [&] { return f.quantum->unbind(address); });
  }, "unbind silently measured a coherent byte");
  normal.fill(0xe9);
  rejects([&] { f.memory.read_block(address - 2, normal.data(), normal.size()); },
          "block read silently measured a superposed byte");
  check(std::all_of(normal.begin(), normal.end(), [](auto b) { return b == 0xe9; }),
        "rejected coherent read exposed a partial operand");
  rejects([&] { f.memory.read_parallel_ram_block(address - 2, normal.data(), normal.size()); },
          "parallel read silently measured a superposed byte");
  check(f.memory.scan_ram_until_byte(address - 2, 8, 0x30).examined == 1,
        "scan accessed a coherent byte after an earlier match");
  rejects([&] { (void)f.memory.scan_ram_until_byte(address - 2, 8, 0x31); },
          "scan silently measured a byte that it actually reached");
  const std::array<std::uint8_t, 8> updates{{1, 2, 3, 4, 5, 6, 7, 8}};
  rejects([&] { f.memory.write_block(address - 2, updates.data(), updates.size()); },
          "classical block store silently collapsed quantum data");
  rejects([&] { f.memory.fill(address - 2, 0x88, 8); },
          "classical fill silently collapsed quantum data");
  check(f.memory.read_byte(address - 2) == 0x30 && f.memory.read_byte(address + 5) == 0x30 &&
            f.quantum->snapshot() == saved,
        "failed mixed quantum/basis operation committed a prefix");
  f.quantum->execute_xi("h 0\n");
  const auto before_budget = f.quantum->snapshot();
  const std::vector<std::uint8_t> excessive_write(33 * kPage, 0x19);
  bool budget_rejected = false;
  try { f.memory.write_block(address - 2, excessive_write.data(), excessive_write.size()); }
  catch (const MemoryFabric::BudgetExceeded&) { budget_rejected = true; }
  check(budget_rejected && f.quantum->snapshot() == before_budget &&
            f.memory.read_byte(address - 2) == 0x30,
        "ordinary RAM budget failure committed a quantum prefix");
  f.memory.fill(kPage, 0x66, kPage);
  f.quantum->execute_xi("x 0\n");
  check(f.memory.read_byte(address) == 0x67 && f.quantum->read(address) == 0x67 &&
            f.memory.read_byte(address + 1) == 0x66,
        "full-page fill bypassed its bound quantum byte");
  f.memory.unbind_quantum_byte(address, [&] { return f.quantum->unbind(address); });
  check(!f.quantum->contains(address) && f.memory.read_byte(address) == 0x67,
        "unbind revived the discarded original basis byte");
  f.memory.reset_zero();
  check(f.quantum->stats().bound_bytes == 0 && f.memory.read_byte(address) == 0 &&
            f.memory.read_byte(5 * kPage) == 0,
        "RAM reset retained quantum state or bindings");
}

void revision_invalidates_consumers() {
  Fixture f;
  constexpr std::uint64_t address = 0x5000;
  f.memory.write_byte(address, 0x10);
  f.bind(address, "0");
  f.memory.enable_code_page_versions(true);
  f.memory.enable_write_watch(address - 2, 8, kPage);
  const auto arm = [&] {
    f.memory.notify_quantum_state_changed();
    const auto token = f.memory.watch_code_page(address);
    f.memory.watch_page_table(address);
    f.memory.acknowledge_write_watch();
    check(token != 0 && f.memory.page_table_epoch() != 0 && !f.memory.write_watch()->pending(),
          "cache/watch fixture did not arm");
    return token;
  };
  const auto token = arm();
  f.memory.write_byte(address + 3 * kPage, 0x77);
  check(f.memory.code_page_version(address) == token,
        "unrelated ordinary RAM write invalidated a different code page");
  f.quantum->execute_xi("x 0\n");
  check(f.memory.code_page_version(address) == 0 && f.memory.page_table_epoch() == 0 &&
            f.memory.write_watch()->pending(),
        "external gate left code, translation or video caches valid");
  arm();
  f.quantum->execute_xi("x 0\n");
  check(f.memory.write_watch()->pending(),
        "write-watch lookup alone failed to observe external quantum revision");
  arm();
  f.quantum->execute_xi("x 0\n");
  check(f.memory.page_table_epoch() == 0,
        "translation lookup alone failed to observe external quantum revision");
}

void shared_cpus_and_mmio_exclusion() {
  Fixture f;
  constexpr std::uint64_t address = 0x9000, mmio = address + 1;
  f.memory.write_byte(address, 17);
  f.memory.write_byte(mmio, 5);
  f.bind(address, "0");
  f.bind(mmio, "8");
  Cpu primary(f.memory), secondary(f.memory);
  secondary.write8(address, 99);
  check(&primary.memory == &secondary.memory && primary.read8(address) == 99 &&
            f.quantum->read(address) == 99,
        "secondary CPU used a separate or stale RAM authority");
  unsigned reads = 0, writes = 0;
  std::uint8_t device_value = 0;
  primary.mmio_claim_hook = [](auto at) { return at == mmio; };
  primary.mmio_range_claim_hook = [](auto at, auto bytes) { return at <= mmio && mmio - at < bytes; };
  primary.mmio_read_hook = [&](auto) -> std::optional<std::uint8_t> { ++reads; return 0xe7; };
  primary.mmio_write_hook = [&](auto, auto value) { ++writes; device_value = value; return true; };
  check(primary.read8(mmio) == 0xe7 && reads == 1, "RAM intercepted an MMIO read");
  primary.write8(mmio, 0xbe);
  check(writes == 1 && device_value == 0xbe && f.quantum->read(mmio) == 5,
        "MMIO write mutated the underlying quantum RAM");
  std::array<std::uint8_t, 2> data{{0xee, 0xee}};
  check(!primary.try_read_ram_block(address, data.data(), data.size()) && data[0] == 0xee,
        "DMA fast read crossed MMIO as ordinary RAM");
  check(!primary.try_write_ram_block(address, data.data(), data.size()) &&
            f.quantum->read(address) == 99 && f.quantum->read(mmio) == 5,
        "DMA fast write crossed MMIO as ordinary RAM");
}

void failed_instruction_restores_quantum_and_basis_ram() {
  Fixture f;
  constexpr std::uint64_t code = 0x7c00, stack = 0x9000;
  f.memory.fill(stack, 0x5a, 16);
  f.bind(stack, "0");
  f.bind(stack + 14, "8");
  f.quantum->execute_xi("h 0\n");
  f.memory.write_byte(code, 0x60);  // PUSHA: eight independent 16-bit stores.
  Cpu cpu(f.memory);
  cpu.reset(0, code);
  for (unsigned reg = 0; reg < 8; ++reg) cpu.regs[reg] = 0x1122U + 0x111U * reg;
  cpu.regs[Cpu::ESP] = stack + 16;
  const auto registers = cpu.regs;
  const auto flags = cpu.eflags;
  // The first push modifies the deterministic quantum byte at stack+14.
  // The last push reaches the unresolved byte at stack and must fault.
  check(cpu.step() == StepResult::Faulted && cpu.fault &&
            cpu.fault->code == cosmic_pc::FaultCode::HostError,
        "unresolved quantum stack operand did not become a bounded CPU fault");
  check(cpu.eip == code && cpu.regs == registers && cpu.eflags == flags,
        "failed multi-store instruction committed architectural state");
  for (std::uint64_t address = stack + 1; address < stack + 16; ++address)
    check(f.memory.read_byte(address) == 0x5a, "CPU rollback left a modified basis/quantum stack byte");
  check(f.quantum->read(stack + 14) == 0x5a &&
            std::abs(f.quantum->probability_one("0") - .5) < 2e-12 &&
            !f.memory.has_pinned_writes(),
        "rollback lost the quantum value, unresolved state or write-pin lifetime");
  f.quantum->execute_xi("h 0\n");
  check(f.memory.read_byte(stack) == 0x5a, "rollback measured or damaged the original quantum phase");
}

void retired_input_survives_failed_debug_delivery() {
  Fixture f;
  constexpr std::uint64_t code = 0x7c00, destination = 0x9000, stack = 0xa000;
  f.memory.write_byte(destination, 17);
  f.memory.fill(stack, 0x5a, 6);
  f.bind(destination, "0");
  f.bind(stack + 4, "8");
  f.bind(stack + 2, "16");
  f.quantum->execute_xi("h 16\n");
  constexpr std::uint8_t program[]{
      0x0f, 0x23, 0xc0,  // MOV DR0,EAX
      0x0f, 0x23, 0xf8,  // MOV DR7,EAX
      0x6c,              // INSB: committed before the I/O debug trap.
      0xf4};
  f.memory.write_block(code, program, sizeof(program));
  Cpu cpu(f.memory);
  cpu.reset(0, code);
  cpu.cr4 |= 8U;  // CR4.DE enables I/O debug breakpoints.
  cpu.regs[Cpu::EAX] = 0x80;
  check(cpu.step() == StepResult::Executed, "could not install DR0 for the retired-store regression");
  cpu.regs[Cpu::EAX] = 0x00020401;
  check(cpu.step() == StepResult::Executed, "could not install the I/O breakpoint in DR7");
  cpu.write16(4, 0x7000);  // Nonempty real-mode #DB vector.
  cpu.write16(6, 0);
  cpu.regs[Cpu::EDX] = 0x80;
  cpu.regs[Cpu::EDI] = destination;
  cpu.regs[Cpu::ESP] = stack + 6;
  unsigned reads = 0;
  cpu.in_hook = [&](std::uint16_t port, unsigned width) -> std::optional<std::uint32_t> {
    if (port != 0x80 || width != 1) return std::nullopt;
    ++reads;
    return 0xac;
  };
  const auto retired = cpu.instructions;
  // INSB retires its data store and DI advance, then #DB attempts its frame.
  // FLAGS changes the quantum byte at stack+4; CS reaches a superposed byte
  // at stack+2 and fails. Only the incomplete interrupt frame may roll back.
  check(cpu.step() == StepResult::Faulted && cpu.fault &&
            cpu.fault->code == cosmic_pc::FaultCode::DebugException,
        "failed #DB delivery did not preserve its terminal trap");
  check(reads == 1 && cpu.instructions == retired + 1 && cpu.eip == code + 7 &&
            cpu.regs[Cpu::EDI] == destination + 1 && cpu.regs[Cpu::ESP] == stack + 6,
        "debug delivery rolled back or repeated a retired input instruction");
  check(f.memory.read_byte(destination) == 0xac && f.quantum->read(destination) == 0xac,
        "interrupt rollback erased an already retired quantum RAM store");
  check(f.memory.read_byte(stack + 4) == 0x5a && f.memory.read_byte(stack + 5) == 0x5a &&
            std::abs(f.quantum->probability_one("16") - .5) < 2e-12,
        "failed debug delivery retained a partial frame or measured its operand");
}
}  // namespace

int main() {
  try {
    ordinary_ram_oracle();
    authoritative_read_and_shortcuts();
    revision_invalidates_consumers();
    shared_cpus_and_mmio_exclusion();
    failed_instruction_restores_quantum_and_basis_ram();
    retired_input_survives_failed_debug_delivery();
    const auto probe = reverse_windows::run_quantum_ram_guest_probe();
    check(probe.passed, "real x86 quantum RAM probe failed");
    std::cout << "{\"ether_state_ram_integration\":\"passed\",\"checks\":" << checks
              << ",\"basis_oracle\":true,\"shared_vcpu\":true,\"mmio_exclusion\":true,"
                 "\"revision_invalidation\":true,\"quantum_rollback\":true,"
                 "\"retired_store_preserved\":true,\"probe\":";
    probe.write_json(std::cout);
    std::cout << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Ether RAM integration: " << error.what() << '\n';
    return 1;
  }
}
