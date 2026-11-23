"""Export counters from an ephemeral ISO run without recording any guest data."""
import argparse
import json
import os
import re
from pathlib import Path


COVERAGE = ("translated_steps", "batched_steps", "projected_steps", "interpreted_steps")
NATIVE = ("translated", "executed_blocks", "cache_hits", "data_fallbacks", "guest_steps",
          "invalidated", "compiled_evictions", "code_byte_checks", "ram_read_callbacks",
          "resident_read_hits", "off_bus_register_steps", "repeated_loop_steps", "repeated_loop_dispatches",
          "staged_ram_blocks", "staged_ram_steps", "coalesced_ram_writes", "ram_add_guest_steps",
          "on_bus_register_steps", "register_bus_reacquisitions", "register_bus_reacquire_samples",
          "register_bus_reacquire_contentions", "register_bus_reacquire_wait_ns")
BOOT_LOOPS = ("boot_scan_loop_steps", "boot_forward_copy_loop_steps", "boot_zero_loop_steps",
              "boot_rep_elements", "boot_rep_direct_page_copy_bytes", "boot_counter_loop_steps")
RAM = ("usable_bytes", "resident_bytes", "resident_metadata_bytes", "backing_budget_bytes",
       "spill_limit_bytes", "spilled_bytes", "spill_file_payload_bytes", "spill_reads", "spill_writes",
       "spill_write_calls", "spill_cache_bytes", "spill_cache_capacity_bytes", "spill_grouped_pages",
       "spill_cache_hits", "spill_read_us", "spill_write_us", "compaction_passes", "compaction_pages_scanned",
       "pressure_status", "pressure_error", "spill_error", "resident_ceiling_bytes", "resident_working_set_bytes",
       "resident_payload_bytes", "resident_cache_bytes", "pressure_target_bytes", "pressure_maintenance_us",
       "pressure_reclaimed_bytes", "pressure_released_resident_bytes", "compaction_pages", "compaction_interval_steps",
       "spill_deferred_enabled", "spill_deferred_staged_pages", "spill_deferred_packed_pages")
ACCESS = ("forced_reclaims", "forced_failures", "forced_reclaim_ns",
          "selection_ns", "backing_read_attempts", "backing_read_failures", "backing_read_ns",
          "requested_pages", "reclaimed_bytes", "write_promotion_attempts", "write_promotions")
JOURNAL = ("pread_calls", "pread_bytes", "pread_ns", "pwrite_calls", "pwrite_bytes", "pwrite_ns",
           "fsync_calls", "fsync_ns")
PARALLEL_IO = ("read_runs", "write_transactions", "read_bytes", "write_bytes")
ATA = ("reads", "writes", "flushes", "read_ns", "write_ns", "flush_ns", "batched_pio_bytes")
ATA_DEVICE = ("commands", "identify_data_transfers", "read_sectors", "write_sectors", "flushes")
SMP = ("started", "host_workers", "host_workers_capacity", "host_workers_peak_dispatch",
       "threaded_dispatches", "threaded_guest_steps", "off_bus_ram_blocks", "off_bus_ram_steps",
       "ram_view_conflicts", "sampled_bus_grants", "sampled_bus_contentions", "sampled_bus_wait_ns",
       "parallel_register_steps", "parallel_ram_steps", "parallel_mixed_batches",
       "threaded_pause_yields", "threaded_pause_turn_steps", "threaded_pause_admissions")
RTC = ("register_a", "register_b", "irq8_assertions", "irq8_acknowledgements", "irq8_delivered")
IRQ_VECTOR = ("deliveries", "eois", "entry_to_eoi_ticks", "entry_to_eoi_steps")
IRQ_CLOCK = ("clock_source", "virtual_clock_hz", "active_clock_elapsed_ns", "active_clock_ticks")
BOOT_PROFILE_STATE = ("steps", "total_processor_steps", "ether_arm64_steps", "started_vcpus",
                      "rtc_irq8_delivered", "host_elapsed_ns", "sipi_targets_applied", "mode",
                      "ip_hex", "run_ns", "irq_delivered", "spill_reads", "spill_writes", "registers", "tsc_access")
EARLY_LONG64 = tuple(range(750_000_000, 1_000_000_001, 50_000_000))
LATE_LONG64 = tuple(range(8_200_000_000, 8_650_000_001, 50_000_000))
AFTER_8_65B = (8_650_000_000, 9_000_000_000, 9_250_000_000, 9_500_000_000)
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
    storage = vm["storage_io"]
    topology = vm["cpu_topology"]
    irq = vm["irq"]
    profile = vm.get("boot_profile", {})
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
        "started_vcpus": topology["started"],
        "guest_tsc_hz": topology.get("guest_tsc_hz"),
        "tsc_access": topology.get("tsc_access"),
        "tsc_access_by_logical_processor": topology.get("tsc_access_by_logical_processor"),
        "smp": pick(topology, SMP),
        "startup_ipis": topology.get("startup_ipis", {}),
        "vcpu_states": topology.get("states_by_logical_processor", []),
        "vcpu_steps": topology.get("steps_by_logical_processor", []),
        "vcpu_arm64_steps": topology.get("ether_arm64_steps_by_logical_processor", []),
        "vcpu_execution": topology.get("execution_by_logical_processor", []),
        "rtc": pick(vm.get("rtc", {}), RTC),
        "irq_clock": pick(irq, IRQ_CLOCK),
        "irq_counters": pick(irq, ("irq64_bulk_frames",)),
        "irq_vectors": {str(vector["vector"]): pick(vector, IRQ_VECTOR)
                        for vector in irq.get("lapic_vectors", [])},
        "boot_profile_current": pick(profile.get("current", {}), BOOT_PROFILE_STATE),
        "first_ap_start_observed": profile.get("first_ap_start_observed"),
        "windows_ready": machine["windows_ready"],
        "qssd_commits": vm["disk"]["commits"],
        "system_qssd_journal": pick(storage.get("qssd_journal", {}), JOURNAL),
        "system_qssd_parallel_io": pick(storage.get("qssd_parallel_io", {}), PARALLEL_IO + ("peak_workers",)),
        "ata_backend": pick(storage.get("ata_backend", {}), ATA),
        "ata_device": pick(vm.get("ata", {}), ATA_DEVICE),
        "coverage": pick(vm["ether_coverage"], COVERAGE),
        "native_arm64": pick(arm64, NATIVE),
        "boot_loops": pick(vm.get("ether_next", {}), BOOT_LOOPS),
        "native_arm64_supported": arm64["host_supported"],
        "native_arm64_enabled": arm64["enabled"],
        "mundo_x_source_sha": arm64["mundo_x_source_sha"],
        "ram": pick(vm["ram_profile"], RAM),
        "spill_codec": {key: value for key, value in vm["ram_profile"].get("spill_codec", {}).items()
                        if type(value) is int},
        "ram_access_pressure": pick(vm["ram_profile"].get("access_pressure", {}),
                                    ACCESS + ("forced_reclaim_max_ns",)),
    }


def compute_delta(first, last, fields):
    return {key: last[key] - first[key] for key in fields
            if type(first.get(key)) is int and type(last.get(key)) is int}


def compare(first, last):
    steps = last["actual_steps"] - first["actual_steps"]
    wall_ns = last["elapsed_ns"] - first["elapsed_ns"]
    optical_ns = (last["iso_read_us"] - first["iso_read_us"]) * 1000
    cpu_ns = last["process_cpu_ns"] - first["process_cpu_ns"]
    engine_ns = last["engine_window_ns"] - first["engine_window_ns"]
    if steps <= 0 or wall_ns <= 0 or optical_ns < 0:
        raise ValueError("Matched checkpoints must have increasing steps and time")
    remaining_ns = wall_ns - optical_ns
    native_counts = compute_delta(first["native_arm64"], last["native_arm64"], NATIVE)
    native_blocks = native_counts.get("executed_blocks", 0)
    native_steps = native_counts.get("guest_steps")
    vcpu_steps = [b - a for a, b in zip(first["vcpu_steps"], last["vcpu_steps"])]
    vcpu_arm64_steps = [b - a for a, b in zip(first["vcpu_arm64_steps"], last["vcpu_arm64_steps"])]
    irq_vectors = {vector: compute_delta(
        {key: first["irq_vectors"].get(vector, {}).get(key, 0) for key in IRQ_VECTOR},
        counters, IRQ_VECTOR) for vector, counters in last["irq_vectors"].items()}
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
        "bsp_steps_per_wall_second": vcpu_steps[0] * 1e9 / wall_ns if vcpu_steps else None,
        "vcpu_steps": vcpu_steps,
        "vcpu_arm64_steps": vcpu_arm64_steps,
        "started_vcpus": [first["started_vcpus"], last["started_vcpus"]],
        "smp_delta": compute_delta(first["smp"], last["smp"], SMP[4:]),
        "startup_ipis_delta": compute_delta(first["startup_ipis"], last["startup_ipis"], last["startup_ipis"]),
        "irq_vectors_delta": irq_vectors,
        "rtc_delta": compute_delta(first["rtc"], last["rtc"], RTC[2:]),
        "irq_clock_delta": compute_delta(first["irq_clock"], last["irq_clock"], IRQ_CLOCK[2:]),
        "irq_counters_delta": compute_delta(first["irq_counters"], last["irq_counters"],
                                            ("irq64_bulk_frames",)),
        "guest_steps_per_second_excluding_measured_optical_io":
            steps * 1e9 / remaining_ns if remaining_ns > 0 else None,
        "native_arm64_delta": native_counts,
        "native_guest_steps_per_executed_block": (native_steps / native_blocks
                                                   if native_steps is not None and native_blocks > 0 else None),
        "coverage_delta": compute_delta(first["coverage"], last["coverage"], COVERAGE),
        "boot_loop_delta": compute_delta(first["boot_loops"], last["boot_loops"], BOOT_LOOPS),
        "bios_read_delta": compute_delta(first, last, ("bios_reads", "bios_bytes")),
        "irq_delivered_delta": last["irq_delivered"] - first["irq_delivered"],
        "qssd_commits_delta": last["qssd_commits"] - first["qssd_commits"],
        "system_qssd_journal_delta": compute_delta(first["system_qssd_journal"], last["system_qssd_journal"], JOURNAL),
        "system_qssd_parallel_io_delta": compute_delta(first["system_qssd_parallel_io"], last["system_qssd_parallel_io"], PARALLEL_IO),
        "ata_backend_delta": compute_delta(first["ata_backend"], last["ata_backend"], ATA),
        "ata_device_delta": compute_delta(first["ata_device"], last["ata_device"], ATA_DEVICE),
        "mode_changes_in_interval": last["mode_changes"] - first["mode_changes"],
        "ram_counter_delta": compute_delta(first["ram"], last["ram"], RAM),
        "spill_codec_delta": compute_delta(first["spill_codec"], last["spill_codec"], last["spill_codec"]),
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


def first_ap_start_snapshot(run_dir):
    snapshots = run_dir / "snapshots.jsonl"
    if not snapshots.is_file():
        return None
    with snapshots.open() as stream:
        for line in stream:
            if not line.endswith("\n"):
                break
            record = json.loads(line)
            vm = record["session"]
            topology = vm["cpu_topology"]
            startup = topology.get("startup_ipis", {})
            if topology["started"] < 2 or not startup.get("sipi_targets_applied"):
                continue
            return {
                "elapsed_ms": record["elapsed_ms"],
                "total_processor_steps": vm["boot_progress"]["session_steps"],
                "primary_steps": vm["machine"]["instructions"],
                "started_vcpus": topology["started"],
                "startup_ipis": startup,
                "vcpu_steps": topology.get("steps_by_logical_processor", []),
                "rtc_irq8_delivered": vm.get("rtc", {}).get("irq8_delivered"),
                "lapic_irq_delivered": vm["irq"]["delivered"],
            }
    return None


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
        "affinity_cpu": int(os.environ["PROBE_CPU"].split(",")[0]),
        "affinity_cpus": [int(cpu) for cpu in os.environ["PROBE_CPU"].split(",")],
        "host_available_mib": int(os.environ["AVAILABLE_MIB"]),
        "host_threshold_mib": int(os.environ["THRESHOLD_MIB"]),
        "probe_exit_code": args.exit_code,
        "probe_timeout_seconds": int(os.environ.get("PROBE_TIMEOUT_SECONDS", "600")),
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
    summary["first_ap_start_snapshot"] = first_ap_start_snapshot(args.run)
    summary["early_long64_per_50m"] = long64_intervals(checkpoints, EARLY_LONG64)
    summary["late_long64_per_50m"] = long64_intervals(checkpoints, LATE_LONG64)
    summary["continuation_after_8_65b"] = long64_intervals(checkpoints, AFTER_8_65B)
    late_smp_targets = tuple(range(9_500_000_000, min(11_500_000_000, stop_target) + 1, 50_000_000))
    late_smp_targets += tuple(range(12_000_000_000, stop_target + 1, 500_000_000))
    summary["sustained_long64_per_50m"] = long64_intervals(checkpoints, late_smp_targets)
    summary["observed_two_live_vcpus"] = any(
        min(interval["started_vcpus"]) >= 2
        and len(interval["vcpu_steps"]) > 1 and min(interval["vcpu_steps"][:2]) > 0
        and interval["smp_delta"].get("threaded_guest_steps", 0) > 0
        for interval in summary["sustained_long64_per_50m"])
    for name, bounds in (("early_long64_control", (EARLY_LONG64[0], EARLY_LONG64[-1])),
                         ("late_long64_8_2_to_8_6", (LATE_LONG64[0], 8_600_000_000)),
                         ("late_long64_total", (LATE_LONG64[0], LATE_LONG64[-1])),
                         ("continuation_8_65_to_9_5", (AFTER_8_65B[0], AFTER_8_65B[-1]))):
        if valid_targets(bounds):
            summary[name] = long64_interval(checkpoints[bounds[0]], checkpoints[bounds[-1]])
    required_targets = EARLY_LONG64 if stop_target == EARLY_LONG64[-1] else EARLY_LONG64 + LATE_LONG64
    required_intervals = summary["early_long64_per_50m"]
    if stop_target != EARLY_LONG64[-1]:
        required_intervals = required_intervals + summary["late_long64_per_50m"]
    if stop_target >= AFTER_8_65B[-1]:
        required_targets += AFTER_8_65B[1:]
        required_intervals = required_intervals + summary["continuation_after_8_65b"]
    if stop_target > AFTER_8_65B[-1]:
        required_targets += late_smp_targets[1:]
    summary["validated_measurement"] = bool(
        args.exit_code == 0 and valid_source and summary["continuation"]["complete"]
        and valid_targets(required_targets)
        and all(interval["long64_endpoints_no_restart"] for interval in required_intervals)
        and all(interval["restarts_in_interval"] == 0 for interval in summary["sustained_long64_per_50m"])
    )
    wall_completion = args.run / "wall-completion.json"
    if wall_completion.is_file():
        wall = json.loads(wall_completion.read_text())
        vm = json.loads((args.run / "session.json").read_text())
        summary["wall_probe"] = wall
        summary["terminal_observation"] = load_checkpoint({
            "target_steps": vm["boot_progress"]["session_steps"],
            "elapsed_ns": wall["elapsed_ms"] * 1_000_000,
            "process_cpu_ns": wall["process_cpu_ns"],
            "engine_window_ns": wall["engine_window_ns"],
            "process_peak_rss_bytes": wall["process_peak_rss_bytes"],
            "session": vm,
        })
        summary["validated_measurement"] = bool(
            args.exit_code == 0 and valid_source
            and wall["elapsed_ms"] >= wall["requested_wall_ms"]
            and summary["terminal_observation"]["mundo_x_source_sha"] == core_sha
            and all((args.run / f"wall-{point}.json").is_file() for point in (720000, 1080000))
        )
    args.output.write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    return 0 if summary["validated_measurement"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
