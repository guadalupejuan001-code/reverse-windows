"""Compare qualified guest work at matched time and retired-step observations."""
import argparse
import json
from pathlib import Path

from summarize import WALL_OBSERVATIONS_MS, compare, pick


FIRST, LAST, INTERVAL = 10_000_000_000, 11_500_000_000, 50_000_000
TARGETS = range(FIRST, LAST + 1, INTERVAL)
FILL_FIELDS = ("boot_streaming_sse_fill_steps", "secondary_streaming_sse_fill_steps")
INPUTS = ("runner_architecture", "affinity_cpus", "host_available_mib", "host_threshold_mib",
          "probe_timeout_seconds", "optical_media_bytes", "optical_media_etag")
OBSERVATION_FIELDS = ("target_steps", "actual_steps", "elapsed_ns", "process_cpu_ns", "engine_window_ns",
                      "process_peak_rss_bytes", "status", "cpu_mode", "rip_hex", "started_vcpus",
                      "restarts", "windows_ready", "irq_delivered", "ata_device", "bios_reads", "bios_bytes",
                      "vcpu_steps", "vcpu_states", "vcpu_execution", "startup_ipis", "coverage", "native_arm64",
                      "ram", "machine_inputs", "qssd_commits", "tsc_access")
INTERVAL_FIELDS = ("from_actual_steps", "to_actual_steps", "actual_steps", "wall_ns", "optical_io_ns",
                   "process_cpu_ns", "guest_steps_per_wall_second", "bsp_steps_per_wall_second",
                   "irq_delivered_delta", "started_vcpus", "vcpu_steps", "native_arm64_delta", "coverage_delta",
                   "bios_read_delta", "ata_device_delta", "system_qssd_journal_delta", "ram_counter_delta",
                   "mode_changes_in_interval", "restarts_in_interval")


def fill_steps(point):
    counters = point.get("boot_loops", {})
    return {"bsp": counters.get(FILL_FIELDS[0]), "secondary_total": counters.get(FILL_FIELDS[1])}


def observation(point):
    return {**pick(point, OBSERVATION_FIELDS), "qualified_streaming_fill_steps": fill_steps(point)}


def interval(first, last):
    result = compare(first, last)
    counters = fill_steps({"boot_loops": result["boot_loop_delta"]})
    return {**pick(result, INTERVAL_FIELDS), "qualified_streaming_fill_steps": counters}


def load_role(directory, role):
    path = directory / f"{role}.json"
    if not path.is_file():
        return None, {}, {"role": role, "valid": False, "error": "No session summary was written"}
    run = json.loads(path.read_text())
    steps = {point["target_steps"]: point for point in run["checkpoints"]}
    terminal = run.get("terminal_observation", {})
    wall = run.get("wall_observations", {})
    core = run["mundo_x_sha"]
    observed = [terminal, *wall.values(), *(steps[target] for target in TARGETS if target in steps)]
    identity = all(point.get("mundo_x_source_sha") == core for point in observed)
    supported = all(point.get("native_arm64_supported") and point.get("native_arm64_enabled")
                    for point in observed)
    fill_reported = all(None not in fill_steps(point).values() for point in observed)
    valid = run["validated_measurement"] and identity and supported and (role != "candidate" or fill_reported)
    return run, steps, {
        "role": role,
        "valid": valid,
        "fill_counters_reported": fill_reported,
        **pick(run, INPUTS + ("product_sha", "mundo_x_sha", "probe_exit_code", "transport", "wall_probe")),
        "terminal": observation(terminal),
        "wall_checkpoints": {point: observation(row) for point, row in wall.items()},
        "step_checkpoints": [observation(steps[target]) for target in TARGETS if target in steps],
        "unreached_step_checkpoints": [target for target in TARGETS if target not in steps],
    }


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("evidence", type=Path)
    cli.add_argument("--candidate-only", action="store_true")
    args = cli.parse_args()
    candidate_run, candidate_steps, candidate = load_role(args.evidence, "candidate")
    (args.evidence / "candidate-progress.json").write_text(json.dumps(candidate, indent=2) + "\n")
    if args.candidate_only:
        return 0 if candidate["valid"] else 1

    reference_run, reference_steps, reference = load_role(args.evidence, "reference")
    (args.evidence / "reference-progress.json").write_text(json.dumps(reference, indent=2) + "\n")
    matched_inputs = {key: candidate.get(key) is not None and candidate[key] == reference.get(key) for key in INPUTS}
    observed_inputs = candidate.get("terminal", {}).get("machine_inputs", {})
    reference_inputs = reference.get("terminal", {}).get("machine_inputs", {})
    matched_inputs.update({f"guest.{key}": candidate_value is not None and candidate_value == reference_inputs.get(key)
                           for key, candidate_value in observed_inputs.items()})
    candidate_wall = candidate.get("wall_checkpoints", {})
    reference_wall = reference.get("wall_checkpoints", {})
    wall_pairs = {point: {"candidate": candidate_wall[point], "reference": reference_wall[point]}
                  for point in sorted(candidate_wall.keys() & reference_wall.keys(), key=int)}
    intervals = []
    for first, last in [*zip(TARGETS, list(TARGETS)[1:]), (FIRST, LAST)]:
        if {first, last}.issubset(candidate_steps) and {first, last}.issubset(reference_steps):
            intervals.append({"from_target_steps": first, "to_target_steps": last,
                              "candidate": interval(candidate_steps[first], candidate_steps[last]),
                              "reference": interval(reference_steps[first], reference_steps[last])})
    expected_inputs = {
        "runner_architecture": "ARM64",
        "host_available_mib": 749,
        "host_threshold_mib": 128,
        "probe_timeout_seconds": 1440,
        "optical_media_bytes": 5_550_497_792,
    }
    expected_guest = {"guest_tsc_hz": 4_500_000_000_000, "logical_processors": 16, "boot_kind": "iso",
                      "ram_usable_bytes": 64 << 30, "system_disk_bytes": 64 << 30, "data_disk_bytes": 0,
                      "ram_backend": "ether-state-ram", "ram_spill_backend": "qssd-transient"}
    valid = (candidate["valid"] and reference["valid"] and len(observed_inputs) > 0
             and all(matched_inputs.values()) and len(candidate.get("affinity_cpus", [])) == 2
             and all(candidate[key] == value for key, value in expected_inputs.items())
             and all(observed_inputs.get(key) == value for key, value in expected_guest.items())
             and set(candidate["wall_checkpoints"]) == set(map(str, WALL_OBSERVATIONS_MS)))
    summary = {"execution_order": ["candidate", "reference"], "same_physical_runner": True,
               "valid": valid, "matched_inputs": matched_inputs, "candidate_valid": candidate["valid"],
               "reference_valid": reference["valid"], "common_step_intervals": intervals,
               "common_wall_checkpoints_ms": wall_pairs,
               "reference_media": reference_run.get("optical_media") if reference_run else None,
               "candidate_media": candidate_run.get("optical_media") if candidate_run else None,
               "comparison_limit": "Matched retired-step windows do not by themselves establish equal guest work."}
    (args.evidence / "progress.json").write_text(json.dumps(summary, indent=2) + "\n")
    for run in (candidate, reference):
        last = run.get("terminal", {})
        print(f"{run['role']} valid={run['valid']} steps={last.get('actual_steps')} "
              f"qualified_fill={last.get('qualified_streaming_fill_steps')} rip={last.get('rip_hex')}")
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
