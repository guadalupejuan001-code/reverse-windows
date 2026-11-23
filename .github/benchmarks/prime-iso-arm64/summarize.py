"""Export counters from an ephemeral ISO run without recording any guest data."""
import argparse
import json
import os
import re
from pathlib import Path


COVERAGE = ("translated_steps", "batched_steps", "projected_steps", "interpreted_steps")
NATIVE = ("translated", "executed_blocks", "cache_hits", "data_fallbacks", "guest_steps",
          "invalidated", "compiled_evictions", "code_byte_checks", "ram_read_callbacks",
          "resident_read_hits", "off_bus_register_steps", "repeated_loop_steps", "repeated_loop_dispatches")
BOOT_LOOPS = ("boot_scan_loop_steps", "boot_forward_copy_loop_steps", "boot_zero_loop_steps",
              "boot_rep_elements", "boot_rep_direct_page_copy_bytes", "boot_counter_loop_steps")
RAM = ("usable_bytes", "resident_bytes", "resident_metadata_bytes", "backing_budget_bytes",
       "spill_limit_bytes", "spilled_bytes", "spill_file_payload_bytes", "spill_reads", "spill_writes",
       "spill_write_calls", "spill_cache_bytes", "spill_cache_capacity_bytes", "spill_grouped_pages",
       "spill_cache_hits", "spill_read_us", "spill_write_us", "compaction_passes", "compaction_pages_scanned",
       "pressure_status", "pressure_error", "spill_error")
ACCESS = ("forced_reclaims", "forced_failures", "forced_reclaim_ns", "selection_ns", "backing_read_attempts")
EARLY_LONG64 = tuple(range(750_000_000, 1_000_000_001, 50_000_000))
LATE_LONG64 = tuple(range(8_200_000_000, 8_650_000_001, 50_000_000))
RANGES = re.compile(
    r"REMOTE_ISO requests=(\d+) verified_ranges=(\d+) downloaded=(\d+) delivered=(\d+) failed=(\d+)"
    r" first_offset=(\d+) last_byte=(\d+)"
)


def pick(source, fields):
    return {field: source[field] for field in fields if field in source}


def load_checkpoint(record):
    vm = record["session"]
    boot = vm["boot_progress"]
    arm64 = vm["ether_arm64"]
    machine = vm["machine"]
    return {
        "target_steps": record["target_steps"],
        "actual_steps": boot["session_steps"],
        "restarts": boot["restarts"],
        "elapsed_ns": record["elapsed_ns"],
        "process_cpu_ns": record["process_cpu_ns"],
        "engine_window_ns": record["engine_window_ns"],
        "process_peak_rss_bytes": record["process_peak_rss_bytes"],
        "status": vm["status"],
        "cpu_mode": machine["cpu_mode"],
        "cs": machine["cs"],
        "rip_hex": machine["rip_hex"],
        "mode_changes": boot["mode_changes"],
        "iso_read_us": vm["iso_read_us"],
        "bios_reads": machine["disk_reads"],
        "bios_bytes": vm["storage_io"]["bios_boot_media"]["read_bytes"],
        "irq_delivered": vm["irq"]["delivered"],
        "started_vcpus": vm["cpu_topology"]["started"],
        "qssd_commits": vm["disk"]["commits"],
        "coverage": pick(vm["ether_coverage"], COVERAGE),
        "native_arm64": pick(arm64, NATIVE),
        "boot_loops": pick(vm.get("ether_next", {}), BOOT_LOOPS),
        "native_arm64_supported": arm64["host_supported"],
        "native_arm64_enabled": arm64["enabled"],
        "mundo_x_source_sha": arm64["mundo_x_source_sha"],
        "ram": pick(vm["ram_profile"], RAM),
        "ram_access_pressure": pick(vm["ram_profile"].get("access_pressure", {}), ACCESS),
    }


def compute_delta(first, last, fields):
    return {key: last[key] - first[key] for key in fields
            if isinstance(first.get(key), int) and isinstance(last.get(key), int)}


def compare(first, last):
    steps = last["actual_steps"] - first["actual_steps"]
    wall_ns = last["elapsed_ns"] - first["elapsed_ns"]
    optical_ns = (last["iso_read_us"] - first["iso_read_us"]) * 1000
    cpu_ns = last["process_cpu_ns"] - first["process_cpu_ns"]
    engine_ns = last["engine_window_ns"] - first["engine_window_ns"]
    if steps <= 0 or wall_ns <= 0 or optical_ns < 0:
        raise ValueError("Matched checkpoints must have increasing steps and time")
    remaining_ns = wall_ns - optical_ns
    return {
        "from_target_steps": first["target_steps"],
        "to_target_steps": last["target_steps"],
        "from_actual_steps": first["actual_steps"],
        "to_actual_steps": last["actual_steps"],
        "from_cpu_mode": first["cpu_mode"],
        "to_cpu_mode": last["cpu_mode"],
        "actual_steps": steps,
        "wall_ns": wall_ns,
        "process_cpu_ns": cpu_ns,
        "engine_window_ns": engine_ns,
        "guest_steps_per_engine_second": steps * 1e9 / engine_ns if engine_ns > 0 else None,
        "process_peak_rss_bytes_at_end": last["process_peak_rss_bytes"],
        "optical_io_ns": optical_ns,
        "guest_steps_per_wall_second": steps * 1e9 / wall_ns,
        "guest_steps_per_second_excluding_measured_optical_io":
            steps * 1e9 / remaining_ns if remaining_ns > 0 else None,
        "native_arm64_delta": compute_delta(first["native_arm64"], last["native_arm64"], NATIVE),
        "coverage_delta": compute_delta(first["coverage"], last["coverage"], COVERAGE),
        "boot_loop_delta": compute_delta(first["boot_loops"], last["boot_loops"], BOOT_LOOPS),
        "bios_read_delta": compute_delta(first, last, ("bios_reads", "bios_bytes")),
        "irq_delivered_delta": last["irq_delivered"] - first["irq_delivered"],
        "qssd_commits_delta": last["qssd_commits"] - first["qssd_commits"],
        "mode_changes_in_interval": last["mode_changes"] - first["mode_changes"],
        "ram_counter_delta": compute_delta(first["ram"], last["ram"], RAM),
        "ram_access_pressure_delta": compute_delta(first["ram_access_pressure"], last["ram_access_pressure"], ACCESS),
        "restarts_in_interval": last["restarts"] - first["restarts"],
    }


def long64_interval(first, last):
    interval = compare(first, last)
    endpoints = interval["from_cpu_mode"] == interval["to_cpu_mode"] == "long64"
    no_restart = interval["restarts_in_interval"] == 0
    continuous = endpoints and no_restart and interval["mode_changes_in_interval"] == 0
    interval["long64_endpoints_no_restart"] = endpoints and no_restart
    interval["continuous_long64_observed"] = continuous
    interval["mode_classification"] = (
        "continuous_long64_observed" if continuous else
        "mixed_modes_with_long64_endpoints" if endpoints and no_restart else
        "other_mode_or_restart"
    )
    return interval


def long64_intervals(checkpoints, targets):
    return [long64_interval(checkpoints[first], checkpoints[last])
            for first, last in zip(targets, targets[1:]) if first in checkpoints and last in checkpoints]


def continuation_record(run_dir, target):
    report = run_dir / "session.json"
    if not report.is_file():
        return {"target_steps": target, "complete": False}
    vm = json.loads(report.read_text())
    machine = vm["machine"]
    boot = vm["boot_progress"]
    phases = [entry["state"] for entry in vm.get("boot_profile", {}).get("checkpoints", [])
              if entry["state"]["steps"] >= 700_000_000]
    return {
        "target_steps": target,
        "actual_steps": boot["session_steps"],
        "complete": boot["session_steps"] >= target,
        "status": vm["status"],
        "cpu_mode": machine["cpu_mode"],
        "cs": machine["cs"],
        "rip_hex": machine["rip_hex"],
        "pending": machine["pending"],
        "message": machine["message"],
        "long64_observed": machine["cpu_mode"] == "long64" or any(p["mode"] == "long64" for p in phases),
        "restarts": boot["restarts"],
        "bios_reads": machine["disk_reads"],
        "iso_read_us": vm["iso_read_us"],
        "phase_samples": [pick(phase, ("steps", "mode")) for phase in phases],
    }


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--run", type=Path, required=True)
    cli.add_argument("--transport-log", type=Path, required=True)
    cli.add_argument("--output", type=Path, required=True)
    cli.add_argument("--exit-code", type=int, required=True)
    args = cli.parse_args()

    milestones = args.run / "milestones.jsonl"
    checkpoints = {}
    if milestones.is_file():
        with milestones.open() as stream:
            for line in stream:
                if not line.endswith("\n"):
                    break
                row = load_checkpoint(json.loads(line))
                checkpoints[row["target_steps"]] = row

    transport_log = args.transport_log.read_text() if args.transport_log.is_file() else ""
    transport = RANGES.search(transport_log)
    verified = None
    if transport:
        fields = ("requests", "verified_ranges", "downloaded_bytes", "delivered_bytes", "failed",
                  "first_fetched_offset", "last_fetched_byte")
        verified = dict(zip(fields, map(int, transport.groups())))

    product_sha = os.environ["PRODUCT_SHA"]
    core_sha = os.environ["MUNDO_X_SHA"]
    stop_target = int(os.environ.get("STOP_TARGET", "700000000"))
    def valid_targets(targets):
        return all(
            target in checkpoints and checkpoints[target]["native_arm64_supported"]
            and checkpoints[target]["native_arm64_enabled"]
            and checkpoints[target]["mundo_x_source_sha"] == core_sha
            for target in targets
        )
    valid_source = verified and verified["requests"] == verified["verified_ranges"] > 0 and not verified["failed"]
    summary = {
        "product_sha": product_sha,
        "mundo_x_sha": core_sha,
        "runner_architecture": os.environ.get("RUNNER_ARCH"),
        "affinity_cpu": int(os.environ["PROBE_CPU"]),
        "host_available_mib": int(os.environ["AVAILABLE_MIB"]),
        "host_threshold_mib": int(os.environ["THRESHOLD_MIB"]),
        "probe_exit_code": args.exit_code,
        "optical_media": "Microsoft Windows 10 22H2 Enterprise Evaluation x64, verified HTTPS 206 ranges",
        "optical_media_bytes": 5_550_497_792,
        "optical_media_etag": "0x10B3D1548F58FA2A3866686CB570F06A16F6734DD99BBA1C7F0542A849EDD8E4",
        "transport": verified,
        "boundary_accounting": "First completed engine window crossing each requested milestone. Rates use actual retired guest steps.",
        "step_accounting": "Guest steps include elements of repeated instructions.",
        "checkpoints": list(checkpoints.values()),
        "validated_measurement": False,
    }
    if valid_targets((350_000_000, 400_000_000, 700_000_000)):
        summary["protected32_warmup"] = compare(checkpoints[350_000_000], checkpoints[400_000_000])
        summary["protected32_control"] = compare(checkpoints[400_000_000], checkpoints[700_000_000])
    summary["continuation"] = continuation_record(args.run, stop_target)
    summary["early_long64_per_50m"] = long64_intervals(checkpoints, EARLY_LONG64)
    summary["late_long64_per_50m"] = long64_intervals(checkpoints, LATE_LONG64)
    for name, bounds in (("early_long64_control", (EARLY_LONG64[0], EARLY_LONG64[-1])),
                         ("late_long64_8_2_to_8_6", (LATE_LONG64[0], 8_600_000_000)),
                         ("late_long64_total", (LATE_LONG64[0], LATE_LONG64[-1]))):
        if valid_targets(bounds):
            summary[name] = long64_interval(checkpoints[bounds[0]], checkpoints[bounds[-1]])
    required_targets = EARLY_LONG64 if stop_target == EARLY_LONG64[-1] else EARLY_LONG64 + LATE_LONG64
    required_intervals = summary["early_long64_per_50m"]
    if stop_target != EARLY_LONG64[-1]:
        required_intervals = required_intervals + summary["late_long64_per_50m"]
    summary["validated_measurement"] = bool(
        args.exit_code == 0 and valid_source and summary["continuation"]["complete"]
        and valid_targets(required_targets)
        and all(interval["long64_endpoints_no_restart"] for interval in required_intervals)
    )
    args.output.write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    return 0 if summary["validated_measurement"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
