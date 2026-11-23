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
    "probe_timeout_seconds": 1680, "optical_media_bytes": 5_550_497_792,
}
GUEST_INPUTS = {
    "boot_kind": "iso", "logical_processors": 16, "ram_usable_bytes": 64 << 30,
    "ram_backend": "ether-state-ram", "ram_spill_backend": "qssd-transient",
    "system_disk_bytes": 64 << 30, "guest_tsc_hz": 4_500_000_000_000,
}
WALL_PAIRS_MS = ((540000, 570000), (570000, 600000), (600000, 630000), (630000, 660000), (540000, 660000))
STEP_PAIRS = ((10_500_000_000, 11_500_000_000), (11_500_000_000, 12_500_000_000))


def save(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def prepared_progress_after_sleep(checkpoints):
    for before, after in zip(checkpoints, checkpoints[1:]):
        steps = (after["smp"].get("parallel_register_steps", 0)
                 - before["smp"].get("parallel_register_steps", 0))
        stopped = [apic for apic, state in enumerate(before["vcpu_states"]) if state == "halted"]
        if stopped and steps > 0:
            return {"before": pick(before, ("actual_steps", "elapsed_ns", "smp", "vcpu_states")),
                    "after": pick(after, ("actual_steps", "elapsed_ns", "smp", "vcpu_states")),
                    "halted_apic_ids_at_start": stopped, "new_prepared_register_steps": steps}
    return None


def load_role(evidence, role):
    path = evidence / f"{role}.json"
    if not path.is_file():
        return {"role": role, "valid": False, "error": "Missing session observation"}
    run = json.loads(path.read_text())
    terminal = run.get("terminal_observation", {})
    checkpoints = run["checkpoints"]
    valid = bool(run["validated_measurement"] and terminal.get("status") == "running"
                 and terminal.get("native_arm64_supported") and terminal.get("native_arm64_enabled")
                 and not terminal["restarts"] and terminal["ram"]["resident_ceiling_bytes"] == 256 << 20
                 and len(run["affinity_cpus"]) == 2
                 and all(run[key] == value for key, value in EXPECTED_INPUTS.items())
                 and all(terminal["machine_inputs"].get(key) == value for key, value in GUEST_INPUTS.items()))
    progress = {
        **pick(run, INPUTS + REVISIONS + ("probe_exit_code", "transport", "continuation", "wall_probe")),
        "role": role, "valid": valid, "terminal": terminal,
        "first_sampled_prepared_register_progress_after_halted_vcpu": prepared_progress_after_sleep(checkpoints),
    }
    save(evidence / f"{role}-progress.json", progress)
    print(f"{role}: valid={valid} steps={terminal.get('actual_steps')} "
          f"prepared={terminal.get('smp', {}).get('parallel_register_steps')} "
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
                item[role] = compare(record_start, record_end)
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
               for key in INPUTS + ("mundo_x_sha",)}
    ref_inputs = reference.get("terminal", {}).get("machine_inputs")
    cand_inputs = candidate.get("terminal", {}).get("machine_inputs")
    matched["guest_configuration"] = bool(ref_inputs and ref_inputs == cand_inputs)
    ref_steps, ref_wall = observations(args.evidence, "reference")
    cand_steps, cand_wall = observations(args.evidence, "candidate")
    valid = reference["valid"] and candidate["valid"] and all(matched.values())
    save(args.evidence / "progress.json", {
        "valid": valid, "execution_order": ["candidate", "reference"], "same_runner": True,
        "matched_inputs": matched, "reference": reference, "candidate": candidate,
        "retired_step_intervals": list(collect_intervals(ref_steps, cand_steps, STEP_PAIRS)),
        "monotonic_wall_intervals_ms": list(collect_intervals(ref_wall, cand_wall, WALL_PAIRS_MS)),
        "statement_of_limits": "Matched time or step boundaries do not establish identical Windows work.",
    })
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
