#!/usr/bin/env python3
"""Prepare the private, reviewed QuantumRuntime extension for guest RAM.

The pinned checkouts are never modified.  The five generated headers are used
only by quantum_ram_authority.cpp, whose namespaces are isolated from the
ordinary PcMachine/InfiniteSession ABI.  The vector representations themselves
come from the current pinned REVERSE-PC QuantumBlock implementation.
"""

import argparse
import difflib
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

# Loading the pinned review script must not create __pycache__ in its checkout.
sys.dont_write_bytecode = True


EXPECTED = {
    "exact_t_experiment.hpp": "89be6b63b9787d9ef9fe9ace7d9b507b1586a14e887cc99a42234d1b46488590",
    "infinite_core.hpp": "d912ee54835da568f5d8042f647a0e5d2a5227a03782813cf273d2b07174c4e5",
    "quantum_runtime.hpp": "ed721c2e6d45037ac34784437abfd61265d62917fb8e45bd67fbd92aa5fda64f",
    "infinite_program.hpp": "32c652d6e6e9f4ff93d5f8a7b84ff5f0f3be0533ad54a2c78b29eaa32c3be890",
    "inverse_memory.hpp": "3f070acc007b9d48de04bbb97a51d76ed9439e8f3b09d5283c36d2a7c40d11bf",
}
REVERSE_PC_COMMIT = "70baf3c778bdb51336362fe5e68ed1c95b9d5897"
INTEGRATION_SHA256 = "3b3b1ad5f64e4756b5ea43edc66cd3286dbfb1e71b2580e45aaa4aeb39f901a9"


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def checked_bytes(path, digest):
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != digest:
        raise SystemExit(f"Unreviewed quantum RAM source: {path}")
    return raw


def patches(reverse_pc):
    changes = {
        "exact_t_experiment.hpp": [
            ("    ReducedExactTRepresentation out(product.size());",
             "    ReducedExactTRepresentation out(product.size(), max_components);"),
            ("#include <array>",
             '#include <array>\n#include <memory>\n#include <mutex>\n#include "integration/state_io.hpp"'),
            ("  std::size_t n_;\n  std::size_t max_components_;",
             '#include "integration/reduced_state_extensions.inc"\n  std::size_t n_;\n  std::size_t max_components_;'),
            ("    CompactCore next_base = base_;",
             "    auto next_integration_cache = std::make_shared<IntegrationBasisCache>();\n    CompactCore next_base = base_;"),
            ("    base_ = std::move(next_base);",
             "    base_ = std::move(next_base);\n    integration_basis_ = std::move(next_integration_cache);"),
        ],
        "infinite_core.hpp": [
            # The current owner has per-batch pullback caching and work counters.
            # Retain them in REVERSE-PC's unpublished-candidate fast path.
            ("    const auto normalized = normalize_gates(gates);\n    require_next_revision();\n    auto candidate = std::make_shared<State>(*state_);",
             """    const auto normalized = normalize_gates(gates);
    require_next_revision();
    if (program_candidate_ && state_.unique()) {
      PullbackCache pullbacks;
      for (const auto& gate : normalized) apply_physical_gate(*state_, gate, pullbacks);
      ++state_->revision;
      gate_work_stats_ = {gates.size(), normalized.size(), gates.size() - normalized.size(),
                          pullbacks.builds, pullbacks.hits};
      return;
    }
    auto candidate = std::make_shared<State>(*state_);"""),
            ("  Background background_;",
             "  friend class QuantumRuntime;\n  bool program_candidate_ = false;\n  Background background_;"),
        ],
        "quantum_runtime.hpp": [
            ("    auto candidate_core = core_;",
             "    auto candidate_core = core_;\n    candidate_core.program_candidate_ = true;"),
            ("        candidate_core, &candidate_memory, program, require_matching_background);",
             "        candidate_core, &candidate_memory, program, require_matching_background);\n    candidate_core.program_candidate_ = false;"),
        ],
    }
    integration = reverse_pc / "scripts/ether_integration.py"
    checked_bytes(integration, INTEGRATION_SHA256)
    spec = importlib.util.spec_from_file_location("reviewed_ram_ether_integration", integration)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    upstream_expected = {}
    module.configure(upstream_expected, changes)
    # T and T-dagger are not involutions.  The newer Mundo X optimizer originally
    # had only S to exclude; extending its grammar must extend this predicate.
    changes["infinite_core.hpp"].append((
        "      if (gate.op != 'S' && first != none &&",
        "      if (gate.op != 'S' && gate.op != 'T' && gate.op != 'A' && first != none &&"))
    changes["quantum_runtime.hpp"].append((
        "  // Restore both authorities transactionally. A bad core or memory snapshot",
        """  // Typed access to the same unpublished runtime candidate. A failed
  // multi-byte operation discards every quantum flip and binding change.
  class ByteAccess {
   public:
    ByteAccess(const ByteAccess&) = delete;
    ByteAccess& operator=(const ByteAccess&) = delete;
    InverseMemory::ReadResult read(std::uint64_t address) const {
      return memory_.read(std::to_string(address), core_);
    }
    void store(std::uint64_t address, std::uint8_t value) {
      memory_.store(std::to_string(address), value, core_);
    }
    void bind(std::uint64_t address, const std::string& base) {
      memory_.bind_quantum_byte(std::to_string(address), base, core_);
    }
    void unbind(std::uint64_t address) {
      memory_.unbind_quantum_byte(std::to_string(address));
    }
   private:
    friend class QuantumRuntime;
    ByteAccess(InfiniteCore& core, InverseMemory& memory) : core_(core), memory_(memory) {}
    InfiniteCore& core_;
    InverseMemory& memory_;
  };
  template<class Action> void transact_bytes(Action action) {
    auto candidate_core = core_;
    candidate_core.program_candidate_ = true;
    auto candidate_memory = memory_;
    ByteAccess bytes(candidate_core, candidate_memory);
    action(bytes);
    candidate_core.program_candidate_ = false;
    core_ = std::move(candidate_core);
    memory_ = std::move(candidate_memory);
  }

  // Restore both authorities transactionally. A bad core or memory snapshot"""))
    return changes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mundo-x", required=True, type=Path)
    parser.add_argument("--reverse-pc", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    mundo_x = args.mundo_x.resolve()
    reverse_pc = args.reverse_pc.resolve()
    reverse_revision = git(reverse_pc, "rev-parse", "HEAD")
    if reverse_revision != REVERSE_PC_COMMIT:
        raise SystemExit("Unreviewed REVERSE-PC revision for private quantum RAM compatibility")
    subprocess.run(["git", "-C", str(reverse_pc), "diff", "--quiet", "--no-ext-diff", "HEAD", "--",
                    "src/adaptive_reduced.hpp", "src/integration", "scripts/ether_integration.py"], check=True)
    source = mundo_x / "native/include/mundo_x"
    replacements = patches(reverse_pc)
    generated = {}
    differences = []
    # Validate the complete patch set before publishing any generated header.
    for name, digest in EXPECTED.items():
        original = checked_bytes(source / name, digest).decode("utf-8")
        revised = original
        for old, new in replacements[name]:
            if revised.count(old) != 1:
                raise SystemExit(f"Ambiguous quantum RAM compatibility anchor in {name}: {old[:80]!r}")
            revised = revised.replace(old, new)
        generated[name] = (
            "#ifndef REVERSE_WINDOWS_PRIVATE_QUANTUM_RAM\n"
            '#error "Quantum RAM compatibility headers are private to its isolated translation unit"\n'
            "#endif\n" + revised
        )
        differences.extend(difflib.unified_diff(original.splitlines(True), revised.splitlines(True),
                                                fromfile=f"pinned/{name}", tofile=f"private-quantum-ram/{name}"))
    target = args.output / "mundo_x"
    target.mkdir(parents=True, exist_ok=True)
    for name, contents in generated.items():
        destination = target / name
        if not destination.exists() or destination.read_text() != contents:
            destination.write_text(contents)
    (args.output / "changes.diff").write_text("".join(differences))
    provenance = {
        "schema": "reverse-windows-quantum-ram-compat-v1",
        "mundo_x_revision": git(mundo_x, "rev-parse", "HEAD"),
        "reverse_pc_revision": reverse_revision,
        "upstream_header_sha256": EXPECTED,
        "integration_script_sha256": INTEGRATION_SHA256,
        "authority": "QuantumRuntime/InverseMemory/QuantumBlock",
        "namespace": "reverse_windows_ram_mundo_x",
        "host_allocation": "finite state payload, metadata and transaction candidates",
    }
    (args.output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    print(f"Verified private quantum RAM compatibility: {target}")


if __name__ == "__main__":
    main()
