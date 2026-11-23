// This one translation unit owns the reviewed compatibility profile. Private
// namespaces prevent a different InfiniteCore/QuantumBlock definition from
// colliding with PcMachine, qSSD or REgraphics in the same Android binary.
#define REVERSE_WINDOWS_PRIVATE_QUANTUM_RAM 1
#define mundo_x reverse_windows_ram_mundo_x
#define mundo_x_bridge reverse_windows_ram_bridge
#define reverse_pc reverse_windows_ram_reverse_pc
#include "mundo_x/quantum_runtime.hpp"
#undef reverse_pc
#undef mundo_x_bridge
#undef mundo_x
#undef REVERSE_WINDOWS_PRIVATE_QUANTUM_RAM

#include "quantum_ram_authority.hpp"

#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace reverse_windows {
namespace {
namespace mx = reverse_windows_ram_mundo_x;
namespace state_io = reverse_windows_ram_reverse_pc::state_io;
using Runtime = mx::QuantumRuntime;
using Bindings = std::map<std::uint64_t, std::string>;
constexpr std::size_t kSnapshotBudget = 96U << 20;

mx::InfiniteCore::Limits core_limits(const QuantumRamAuthority::Limits& limits) {
  mx::InfiniteCore::Limits result;
  result.max_active_qubits = limits.max_active_qubits;
  result.max_component_qubits = limits.max_component_qubits;
  result.max_state_bytes = limits.max_state_bytes;
  result.max_reduced_components = limits.max_reduced_components;
  result.max_dense_block = limits.max_dense_qubits;
  return result;
}

mx::InverseMemory::Limits memory_limits(const QuantumRamAuthority::Limits& limits) {
  mx::InverseMemory::Limits result;
  result.max_quantum_bindings = limits.max_bound_bytes;
  // General basis-state bytes belong to EtherStateRam. This runtime contains
  // only explicit quantum views, never an authoritative parallel byte map.
  result.max_overrides = 0;
  return result;
}

std::string byte_end(std::string base) {
  for (unsigned bit = 0; bit < 7; ++bit) {
    std::size_t i = base.size();
    while (i && base[i - 1] == '9') base[--i] = '0';
    if (i) ++base[i - 1]; else base.insert(base.begin(), '1');
  }
  return base;
}

void reject_overlap(const Bindings& bindings, std::uint64_t address, const std::string& base) {
  const auto end = byte_end(base);
  const mx::infinite_detail::AddressLess less;
  for (const auto& binding : bindings) {
    if (binding.first == address) continue;
    if (!less(end, binding.second) && !less(byte_end(binding.second), base))
      throw std::invalid_argument("quantum RAM byte ranges overlap");
  }
}

std::uint8_t deterministic(const mx::InverseMemory::ReadResult& result) {
  if (!result.deterministic)
    throw std::runtime_error("quantum RAM byte requires an explicit measurement or inverse gates");
  return result.value;
}

Bindings recover_bindings(const Runtime& runtime, std::uint64_t capacity) {
  const auto stats = runtime.memory().stats();
  if (stats.background_byte != 0 || stats.overrides != 0)
    throw std::invalid_argument("quantum RAM snapshot contains a parallel classical memory");
  std::istringstream input(runtime.memory_snapshot());
  input.imbue(std::locale::classic());
  std::string magic;
  unsigned background;
  if (!(input >> magic >> background) || magic != "MXIM1" || background != 0)
    throw std::invalid_argument("quantum RAM memory snapshot header");
  Bindings result;
  for (std::string kind, address, base; input >> kind >> address >> base;) {
    if (kind != "Q") throw std::invalid_argument("quantum RAM snapshot has an unbound byte");
    const auto numeric = mx::infinite_program_detail::decimal<std::uint64_t>(address, "guest byte address");
    if (numeric >= capacity) throw std::out_of_range("quantum RAM binding exceeds guest capacity");
    reject_overlap(result, numeric, base);
    if (!result.emplace(numeric, base).second)
      throw std::invalid_argument("duplicate quantum RAM byte binding");
  }
  if (!input.eof() || result.size() != stats.quantum_bindings)
    throw std::invalid_argument("quantum RAM binding index is incomplete");
  return result;
}
}  // namespace

struct QuantumRamAuthority::Impl {
  explicit Impl(Limits requested)
      : limits(requested), empty(mx::InfiniteCore::Background::Zero,
                                 core_limits(requested), memory_limits(requested)),
        runtime(empty) {
    if (!limits.logical_bytes || !limits.max_bound_bytes)
      throw std::invalid_argument("quantum RAM logical capacity and binding budget must be positive");
  }
  Limits limits;
  mutable std::mutex mutex;
  // Reset uses this immutable empty owner; it performs no allocating rebuild.
  const Runtime empty;
  Runtime runtime;
  std::optional<Runtime> pinned_runtime;
  std::size_t scope_depth = 0, pinned_payload = 0;
  bool scope_restored = false;
  std::atomic<bool> scope_commit_required{false};
  Bindings bindings;
  std::atomic<bool> has_bindings{false};
  std::atomic<std::uint64_t> revision{0};
  std::atomic<std::size_t> payload_bytes{0};

  void publish_binding_index() noexcept {
    has_bindings.store(!bindings.empty(), std::memory_order_release);
  }

  void require_revision() const {
    if (revision.load(std::memory_order_relaxed) == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("quantum RAM owner revision overflow");
  }
  void committed() noexcept {
    payload_bytes.store(runtime.core().stats().tableau_bytes, std::memory_order_release);
    revision.fetch_add(1, std::memory_order_release);
    // Fault delivery can write again after restoring the same pinned scope.
    // Each new mutation must be eligible for the next full-state rollback.
    scope_restored = false;
  }
  void require_bound(std::uint64_t address) const {
    if (!bindings.count(address)) throw std::out_of_range("guest address is not bound to quantum RAM");
  }
  void require_unpinned() const {
    if (scope_depth) throw std::logic_error("quantum RAM ownership cannot change during a guest write scope");
  }

  // The authority mutex serializes the first mutation with scope boundaries.
  // An untouched scope already has the current runtime as its undo baseline.
  void save_scope_baseline() noexcept {
    static_assert(std::is_nothrow_copy_constructible<Runtime>::value,
                  "quantum RAM scope checkpoints must not allocate or throw");
    if (!scope_depth || pinned_runtime) return;
    pinned_runtime.emplace(runtime);
    pinned_payload = payload_bytes.load(std::memory_order_relaxed);
    scope_commit_required.store(true, std::memory_order_release);
  }

  void clear_scope_baseline() noexcept {
    pinned_runtime.reset();
    scope_restored = false;
    scope_commit_required.store(false, std::memory_order_release);
  }

  // A nested fabric operation can fail while an outer CPU instruction still
  // owns the write pin. Keep its local rollback independent of that CPU's
  // retirement baseline. The fabric retains the owner and joins readers for
  // the checkpoint's lifetime; bindings cannot change inside the write pin.
  struct Checkpoint final : QuantumRamAuthority::LocalCheckpoint {
    explicit Checkpoint(Impl& state)
        : owner(state), saved(state.runtime),
          payload(state.payload_bytes.load(std::memory_order_relaxed)) {}
    void restore() noexcept override {
      std::lock_guard<std::mutex> lock(owner.mutex);
      owner.save_scope_baseline();
      owner.runtime = saved;
      owner.payload_bytes.store(payload, std::memory_order_release);
      if (owner.revision.load(std::memory_order_relaxed) != std::numeric_limits<std::uint64_t>::max())
        owner.revision.fetch_add(1, std::memory_order_release);
      // Keep any earlier outer baseline and its rollback flag.
    }
    Impl& owner;
    Runtime saved;
    std::size_t payload;
  };
};

QuantumRamAuthority::QuantumRamAuthority() : QuantumRamAuthority(Limits{}) {}
QuantumRamAuthority::QuantumRamAuthority(Limits limits) : impl_(std::make_unique<Impl>(limits)) {}
QuantumRamAuthority::~QuantumRamAuthority() = default;

bool QuantumRamAuthority::contains(std::uint64_t address) const noexcept {
  // Ordinary RAM can reject an empty binding index without taking the
  // authority mutex. A nonempty index is always examined under the lock.
  if (!impl_->has_bindings.load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->bindings.count(address) != 0;
}

bool QuantumRamAuthority::intersects(std::uint64_t first, std::size_t count) const noexcept {
  if (!count || !impl_->has_bindings.load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->bindings.lower_bound(first);
  return it != impl_->bindings.end() && it->first - first < count;
}

std::vector<std::uint64_t> QuantumRamAuthority::bound_addresses(std::uint64_t first,
                                                               std::size_t count) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<std::uint64_t> result;
  for (auto it = impl_->bindings.lower_bound(first);
       it != impl_->bindings.end() && it->first - first < count; ++it)
    result.push_back(it->first);
  return result;
}

std::uint8_t QuantumRamAuthority::read(std::uint64_t address) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->require_bound(address);
  return deterministic(impl_->runtime.memory().read(std::to_string(address), impl_->runtime.core()));
}

void QuantumRamAuthority::write(const std::vector<std::pair<std::uint64_t, std::uint8_t>>& values) {
  if (values.empty()) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->require_revision();
  for (const auto& value : values) impl_->require_bound(value.first);
  impl_->save_scope_baseline();
  impl_->runtime.transact_bytes([&](Runtime::ByteAccess& bytes) {
    for (const auto& value : values) bytes.store(value.first, value.second);
  });
  impl_->committed();
}

std::uint64_t QuantumRamAuthority::revision() const noexcept {
  return impl_->revision.load(std::memory_order_acquire);
}

std::size_t QuantumRamAuthority::state_bytes() const noexcept {
  return impl_->payload_bytes.load(std::memory_order_acquire);
}

void QuantumRamAuthority::reset() noexcept {
  static_assert(std::is_nothrow_copy_assignable<Runtime>::value,
                "quantum RAM reset must not allocate or throw");
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->save_scope_baseline();
  impl_->runtime = impl_->empty;
  impl_->bindings.clear();
  impl_->publish_binding_index();
  impl_->payload_bytes.store(0, std::memory_order_release);
  if (impl_->revision.load(std::memory_order_relaxed) != std::numeric_limits<std::uint64_t>::max())
    impl_->revision.fetch_add(1, std::memory_order_release);
}

void QuantumRamAuthority::begin_write_scope() noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  ++impl_->scope_depth;
}

void QuantumRamAuthority::end_write_scope() noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->scope_depth && --impl_->scope_depth == 0 &&
      impl_->scope_commit_required.load(std::memory_order_relaxed))
    impl_->clear_scope_baseline();
}

void QuantumRamAuthority::commit_write_scope() noexcept {
  // A clean commit changes nothing. A concurrent mutation or restore must
  // publish the flag before it changes state, so a clean read can precede it.
  if (!impl_->scope_commit_required.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->scope_depth) impl_->clear_scope_baseline();
}

bool QuantumRamAuthority::restore_write_scope() noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->scope_depth) return false;
  if (!impl_->scope_restored) {
    impl_->scope_commit_required.store(true, std::memory_order_release);
    if (impl_->pinned_runtime) {
      impl_->runtime = *impl_->pinned_runtime;
      impl_->payload_bytes.store(impl_->pinned_payload, std::memory_order_release);
    }
    // Consumers may have observed a provisional write. Never reuse their
    // cached revision even though all quantum state is restored exactly.
    if (impl_->revision.load(std::memory_order_relaxed) != std::numeric_limits<std::uint64_t>::max())
      impl_->revision.fetch_add(1, std::memory_order_release);
    impl_->scope_restored = true;
  }
  return true;
}

std::unique_ptr<QuantumRamAuthority::LocalCheckpoint> QuantumRamAuthority::checkpoint() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->scope_depth)
    throw std::logic_error("quantum RAM local checkpoint requires a live write scope");
  return std::make_unique<Impl::Checkpoint>(*impl_);
}

void QuantumRamAuthority::bind(std::uint64_t address, const std::string& qbase,
                               std::uint8_t initial_value) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->require_unpinned();
  if (address >= impl_->limits.logical_bytes)
    throw std::out_of_range("quantum RAM binding exceeds guest capacity");
  impl_->require_revision();
  const auto base = mx::infinite_detail::canonical_address(
      qbase, impl_->runtime.core().limits().max_address_digits);
  reject_overlap(impl_->bindings, address, base);
  auto next_bindings = impl_->bindings;
  const auto previous = next_bindings.find(address);
  if (previous != next_bindings.end() && previous->second != base)
    throw std::invalid_argument("unbind the previous quantum RAM view before remapping it");
  next_bindings[address] = base;
  impl_->runtime.transact_bytes([&](Runtime::ByteAccess& bytes) {
    bytes.bind(address, base);
    bytes.store(address, initial_value);
  });
  impl_->bindings.swap(next_bindings);
  impl_->publish_binding_index();
  impl_->committed();
}

std::uint8_t QuantumRamAuthority::unbind(std::uint64_t address) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->require_unpinned();
  impl_->require_bound(address);
  impl_->require_revision();
  const auto value = deterministic(impl_->runtime.memory().read(std::to_string(address), impl_->runtime.core()));
  impl_->runtime.transact_bytes([&](Runtime::ByteAccess& bytes) { bytes.unbind(address); });
  impl_->bindings.erase(address);
  impl_->publish_binding_index();
  impl_->committed();
  return value;
}

std::string QuantumRamAuthority::execute_xi(const std::string& program) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (program.size() > mx::infinite_program_detail::max_program_bytes)
    throw std::length_error("quantum RAM XI program exceeds transport budget");
  // The same tokenizer/case rules as the actual parser make this restriction
  // independent of whitespace/comments. Prevalidate before executing any gate.
  std::istringstream input(program);
  for (std::string line; std::getline(input, line);) {
    const auto words = mx::infinite_program_detail::tokens(line);
    if (words.empty()) continue;
    const auto command = mx::infinite_program_detail::lower_ascii(words.front());
    if (command == "background" || command == "mem_reset" || command == "mem_bind" || command == "mem_unbind")
      throw std::invalid_argument("quantum RAM ownership changes require the typed binding/reset API");
    if (command == "mem_store" || command == "mem_read") {
      mx::infinite_program_detail::require_count(words, command == "mem_store" ? 3 : 2);
      impl_->require_bound(mx::infinite_program_detail::decimal<std::uint64_t>(words[1], "guest byte address"));
    }
    if (command == "mem_materialize") {
      mx::infinite_program_detail::require_count(words, 3);
      const auto first = mx::infinite_program_detail::decimal<std::uint64_t>(words[1], "guest byte address");
      const auto count = mx::infinite_program_detail::decimal<std::size_t>(words[2], "materialization count");
      if (count > impl_->runtime.memory().limits().max_materialize_bytes ||
          first > impl_->limits.logical_bytes || count > impl_->limits.logical_bytes - first)
        throw std::length_error("quantum RAM materialization range/budget");
      for (std::size_t i = 0; i < count; ++i) impl_->require_bound(first + i);
    }
  }
  impl_->require_revision();
  const auto old_core_revision = impl_->runtime.core().stats().revision;
  const auto old_memory_revision = impl_->runtime.memory().stats().revision;
  impl_->save_scope_baseline();
  auto output = impl_->runtime.execute(program, true);
  if (impl_->runtime.core().stats().revision != old_core_revision ||
      impl_->runtime.memory().stats().revision != old_memory_revision)
    impl_->committed();
  return output;
}

double QuantumRamAuthority::probability_one(const std::string& qaddress) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->runtime.core().probability_one(qaddress);
}

QuantumRamAuthority::Stats QuantumRamAuthority::stats() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto core = impl_->runtime.core().stats();
  const auto quantum = impl_->runtime.core().quantum_stats();
  Stats out;
  out.logical_bytes = impl_->limits.logical_bytes;
  out.revision = impl_->revision.load(std::memory_order_relaxed);
  out.core_revision = core.revision;
  out.bound_bytes = impl_->bindings.size();
  out.classical_overrides = impl_->runtime.memory().stats().overrides;
  out.active_qubits = core.active_qubits;
  out.clifford_components = quantum.clifford;
  out.adaptive_components = quantum.adaptive;
  out.reduced_components = quantum.reduced;
  out.mps_components = quantum.mps;
  out.coefficients = quantum.coefficients;
  out.factor_amplitudes = quantum.amplitudes;
  out.t_instructions = quantum.t_gates;
  out.payload_bytes = quantum.payload_bytes;
  out.norm = quantum.norm;
  return out;
}

std::string QuantumRamAuthority::snapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  state_io::Writer writer;
  writer.data = "RWQR1";
  writer.u64(impl_->limits.logical_bytes);
  writer.u64(impl_->revision.load(std::memory_order_relaxed));
  writer.string(impl_->runtime.core_snapshot());
  writer.string(impl_->runtime.memory_persistent_snapshot());
  auto result = writer.finish();
  if (result.size() > kSnapshotBudget) throw std::length_error("quantum RAM snapshot budget exceeded");
  return result;
}

void QuantumRamAuthority::restore(const std::string& saved) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->require_unpinned();
  impl_->require_revision();
  state_io::Reader reader(saved, kSnapshotBudget);
  reader.require(5);
  if (saved.compare(0, 5, "RWQR1") != 0) throw std::invalid_argument("quantum RAM snapshot version");
  reader.at = 5;
  if (reader.u64() != impl_->limits.logical_bytes)
    throw std::invalid_argument("quantum RAM snapshot capacity mismatch");
  (void)reader.u64();  // The owner revision remains monotonic across a restore.
  const auto core = reader.string(kSnapshotBudget);
  const auto memory = reader.string(kSnapshotBudget);
  reader.done();
  auto candidate = impl_->empty;
  candidate.restore(core, memory);
  auto bindings = recover_bindings(candidate, impl_->limits.logical_bytes);
  impl_->runtime = std::move(candidate);
  impl_->bindings.swap(bindings);
  impl_->publish_binding_index();
  impl_->committed();
}

}  // namespace reverse_windows
