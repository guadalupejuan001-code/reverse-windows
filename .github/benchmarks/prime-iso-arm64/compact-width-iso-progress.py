"""Compare exact ARM64 products with the same Windows ISO and QSSD profile."""

import argparse
import json
from pathlib import Path

from summarize import compare, pick

REVISIONS = ("product_sha", "mundo_x_sha")
INPUTS = ("runner_architecture", "affinity_cpus", "host_available_mib", "host_threshold_mib",
          "probe_timeout_seconds", "optical_media_bytes", "optical_media_etag")
EXPECTED_INPUTS = {
    "runner_architecture": "ARM64", "host_available_mib": 749, "host_threshold_mib": 128,
    "probe_timeout_seconds": 1620, "optical_media_bytes": 5_550_497_792,
}
GUEST_INPUTS = {
    "boot_kind": "iso", "logical_processors": 16, "ram_usable_bytes": 64 << 30,
    "ram_backend": "ether-state-ram", "ram_spill_backend": "qssd-transient",
    "system_disk_bytes": 64 << 30, "system_disk_initial_commits": 0, "data_disk_bytes": 0,
    "guest_tsc_hz": 4_500_000_000_000,
}
WALL_PAIRS_MS = ((540000, 570000), (570000, 600000), (600000, 630000), (630000, 660000), (540000, 660000))
STEP_PAIRS = ((10_500_000_000, 11_500_000_000), (11_500_000_000, 12_500_000_000))


def save(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def conservative_wide_dispatch_minimum(counters):
    """A positive bound proves a longer nonrepeated block. Zero is inconclusive."""
    fields = ("guest_steps", "executed_blocks", "repeated_loop_steps", "repeated_loop_dispatches")
    if any(type(counters.get(field)) is not int for field in fields):
        return None
    steps = counters["guest_steps"] - counters["repeated_loop_steps"]
    dispatches = counters["executed_blocks"] - counters["repeated_loop_dispatches"]
    if min(steps, dispatches) < 0 or steps > 32 * dispatches:
        return None
    return (max(0, steps - 16 * dispatches) + 15) // 16


def load_role(evidence, role):
    path = evidence / f"{role}.json"
    if not path.is_file():
        return {"role": role, "valid": False, "error": "Missing session observation"}
    run = json.loads(path.read_text())
    terminal = run.get("terminal_observation", {})
    pins = json.loads((evidence / "targets.json").read_text())[role]
    expected_limits = {"block_max_instructions": 32 if role == "candidate" else 16,
                       "block_max_bytes": 128 if role == "candidate" else 64}
    live_limits = terminal.get("native_arm64_limits", {})
    # The immutable reference predates the two diagnostic fields.
    width_verified = live_limits == expected_limits or (role == "reference" and not live_limits)
    valid = bool(run["validated_measurement"] and terminal.get("status") == "running"
                 and terminal.get("native_arm64_supported") and terminal.get("native_arm64_enabled")
                 and not terminal["restarts"] and terminal["ram"]["resident_ceiling_bytes"] == 256 << 20
                 and len(run["affinity_cpus"]) == 2
                 and all(run[key] == pins[key] for key in REVISIONS)
                 and terminal.get("mundo_x_source_sha") == pins["mundo_x_sha"] and width_verified
                 and all(run[key] == value for key, value in EXPECTED_INPUTS.items())
                 and all(terminal["machine_inputs"].get(key) == value for key, value in GUEST_INPUTS.items()))
    progress = {
        **pick(run, INPUTS + REVISIONS + ("probe_exit_code", "transport", "continuation", "wall_probe")),
        "role": role, "valid": valid, "terminal": terminal,
        "expected_native_arm64_limits": expected_limits, "runtime_native_arm64_limits": live_limits,
        "bsp_wide_nonrepeated_dispatch_minimum": conservative_wide_dispatch_minimum(terminal.get("native_arm64", {})),
    }
    save(evidence / f"{role}-progress.json", progress)
    print(f"{role}: valid={valid} steps={terminal.get('actual_steps')} "
          f"native_limits={live_limits or expected_limits} "
          f"started={terminal.get('started_vcpus')} fault={run['continuation'].get('cpu_fault')}")
    return progress


def observations(evidence, role):
    path = evidence / f"{role}.json"
    if not path.is_file():
        return {}, {}
    run = json.loads(path.read_text())
    return ({point["target_steps"]: point for point in run["checkpoints"]},
            {int(elapsed): point for elapsed, point in run.get("wall_observations", {}).items()})


def collect_intervals(first, last, pairs):
    for start, end in pairs:
        item = {"start": start, "end": end, "reference": None, "candidate": None}
        for role, records in (("reference", first), ("candidate", last)):
            if start not in records or end not in records:
                continue
            record_start, record_end = records[start], records[end]
            if record_end["actual_steps"] > record_start["actual_steps"]:
                result = compare(record_start, record_end)
                result["bsp_wide_nonrepeated_dispatch_minimum"] = conservative_wide_dispatch_minimum(
                    result["native_arm64_delta"])
                item[role] = result
            else:
                item[role] = {"no_positive_progress": True, "start": record_start, "end": record_end}
        yield item


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("evidence", type=Path)
    cli.add_argument("--candidate-only", action="store_true")
    args = cli.parse_args()
    candidate = load_role(args.evidence, "candidate")
    if args.candidate_only:
        return 0 if candidate["valid"] else 1
    reference = load_role(args.evidence, "reference")
    matched = {key: candidate.get(key) is not None and candidate[key] == reference.get(key)
               for key in INPUTS}
    ref_inputs = reference.get("terminal", {}).get("machine_inputs")
    cand_inputs = candidate.get("terminal", {}).get("machine_inputs")
    matched["guest_configuration"] = bool(ref_inputs and ref_inputs == cand_inputs)
    ref_steps, ref_wall = observations(args.evidence, "reference")
    cand_steps, cand_wall = observations(args.evidence, "candidate")
    primary_window = all(target in records and records[target]["cpu_mode"] == "long64"
                         for target in STEP_PAIRS[0] for records in (ref_steps, cand_steps))
    valid = reference["valid"] and candidate["valid"] and all(matched.values()) and primary_window
    save(args.evidence / "progress.json", {
        "valid": valid, "execution_order": ["candidate", "reference"], "same_runner": True,
        "matched_primary_long64_step_window": primary_window,
        "matched_inputs": matched, "reference": reference, "candidate": candidate,
        "retired_step_intervals": list(collect_intervals(ref_steps, cand_steps, STEP_PAIRS)),
        "monotonic_wall_intervals_ms": list(collect_intervals(ref_wall, cand_wall, WALL_PAIRS_MS)),
        "statement_of_limits": ("Matched time or step boundaries do not establish identical Windows work. "
                                "A zero minimum does not determine if Windows executed larger blocks."),
    })
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
