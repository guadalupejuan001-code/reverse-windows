"""Compare paired physical ARM64 sessions with matching official media and inputs."""
import argparse
import json
from pathlib import Path

CONTROLS = ("protected32_control", "early_long64_control", "late_long64_8_2_to_8_6", "late_long64_total",
            "continuation_8_65_to_9_5")
MATCHED_INPUTS = ("optical_media_bytes", "optical_media_etag", "affinity_cpu", "host_available_mib",
                  "host_threshold_mib", "runner_architecture", "probe_timeout_seconds")
SPAN_FIELDS = ("actual_steps", "wall_ns", "process_cpu_ns", "engine_window_ns", "optical_io_ns",
               "process_peak_rss_bytes_at_end", "guest_steps_per_wall_second", "mode_classification",
               "from_cpu_mode", "to_cpu_mode", "mode_changes_in_interval", "restarts_in_interval",
               "irq_delivered_delta", "qssd_commits_delta", "bios_read_delta",
               "coverage_delta", "native_arm64_delta", "native_guest_steps_per_executed_block",
               "boot_loop_delta", "ram_counter_delta", "spill_codec_delta",
               "ram_access_pressure_delta", "system_qssd_journal_delta", "system_qssd_parallel_io_delta",
               "ata_backend_delta")


def pick(record, fields):
    return {key: record[key] for key in fields if key in record}


def compare(reference, candidate):
    targets = (reference["from_target_steps"], reference["to_target_steps"])
    if targets != (candidate["from_target_steps"], candidate["to_target_steps"]):
        raise ValueError("The two physical runs reported different interval bounds")
    old = reference["guest_steps_per_wall_second"]
    new = candidate["guest_steps_per_wall_second"]
    mode_path_equal = all(reference[key] == candidate[key] for key in
                          ("from_cpu_mode", "to_cpu_mode", "mode_changes_in_interval", "restarts_in_interval"))
    media_reads_equal = reference["bios_read_delta"] == candidate["bios_read_delta"]
    return {"from_target_steps": targets[0], "to_target_steps": targets[1],
            "relative_change_percent": (new / old - 1) * 100 if mode_path_equal and media_reads_equal else None,
            "observed_mode_path_equal": mode_path_equal,
            "observed_bios_media_reads_equal": media_reads_equal,
            "reference": pick(reference, SPAN_FIELDS), "candidate": pick(candidate, SPAN_FIELDS)}


def compare_series(reference, candidate, key):
    old = {(row["from_target_steps"], row["to_target_steps"]): row for row in reference[key]}
    new = {(row["from_target_steps"], row["to_target_steps"]): row for row in candidate[key]}
    return [compare(old[span], new[span]) for span in sorted(old.keys() & new.keys())]


def terminal(run):
    latest = run["checkpoints"][-1] if run["checkpoints"] else None
    identity = pick(run, ("product_sha", "mundo_x_sha", "probe_exit_code", "transport", "continuation"))
    if latest:
        identity["last_checkpoint"] = pick(latest, ("actual_steps", "cpu_mode", "rip_hex", "irq_delivered",
            "started_vcpus", "bios_reads", "qssd_commits", "process_peak_rss_bytes", "ram",
            "ram_access_pressure", "spill_codec", "system_qssd_journal", "system_qssd_parallel_io", "ata_backend"))
    return identity


cli = argparse.ArgumentParser(description=__doc__)
cli.add_argument("reference", type=Path)
cli.add_argument("candidate", type=Path)
cli.add_argument("output", type=Path)
args = cli.parse_args()
reference = json.loads(args.reference.read_text())
candidate = json.loads(args.candidate.read_text())
matched = {key: key in reference and reference[key] == candidate.get(key) for key in MATCHED_INPUTS}
controls = {key: compare(reference[key], candidate[key]) for key in CONTROLS if key in reference and key in candidate}
summary = {
    "execution_order": ["reference", "candidate"],
    "same_physical_runner": True,
    "matched_inputs": matched,
    "reference_valid": reference["validated_measurement"],
    "candidate_valid": candidate["validated_measurement"],
    "reference": terminal(reference),
    "candidate": terminal(candidate),
    "controls": controls,
    "early_long64_per_50m": compare_series(reference, candidate, "early_long64_per_50m"),
    "late_long64_per_50m": compare_series(reference, candidate, "late_long64_per_50m"),
    "continuation_after_8_65b": compare_series(reference, candidate, "continuation_after_8_65b"),
}
args.output.write_text(json.dumps(summary, indent=2) + "\n")
for key, result in controls.items():
    old = result["reference"]["guest_steps_per_wall_second"]
    new = result["candidate"]["guest_steps_per_wall_second"]
    gain = result["relative_change_percent"]
    comparison = f"{gain:+.2f}%" if gain is not None else "mode path or BIOS media reads differ"
    print(f"{key} {old:,.0f} -> {new:,.0f} steps/s ({comparison})")
valid = (summary["reference_valid"] and summary["candidate_valid"] and all(matched.values())
         and len(controls) == len(CONTROLS) and len(summary["late_long64_per_50m"]) == 9
         and len(summary["early_long64_per_50m"]) == 5 and len(summary["continuation_after_8_65b"]) == 3)
raise SystemExit(0 if valid else 1)
