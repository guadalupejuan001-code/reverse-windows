"""Export counters from an ephemeral ISO run without recording any guest data."""
import argparse
import json
import os
import re
from pathlib import Path


COVERAGE = ("translated_steps", "batched_steps", "projected_steps", "interpreted_steps")
NATIVE = ("translated", "executed_blocks", "cache_hits", "data_fallbacks", "guest_steps")
RAM = ("usable_bytes", "resident_bytes", "resident_metadata_bytes", "backing_budget_bytes",
       "spill_limit_bytes", "spilled_bytes", "spill_file_payload_bytes", "spill_reads", "spill_writes",
       "spill_write_calls", "spill_cache_bytes", "spill_cache_capacity_bytes", "spill_grouped_pages")
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
        "status": vm["status"],
        "cpu_mode": machine["cpu_mode"],
        "iso_read_us": vm["iso_read_us"],
        "bios_reads": machine["disk_reads"],
        "bios_bytes": vm["storage_io"]["bios_boot_media"]["read_bytes"],
        "irq_delivered": vm["irq"]["delivered"],
        "started_vcpus": vm["cpu_topology"]["started"],
        "coverage": pick(vm["ether_coverage"], COVERAGE),
        "native_arm64": pick(arm64, NATIVE),
        "native_arm64_supported": arm64["host_supported"],
        "native_arm64_enabled": arm64["enabled"],
        "mundo_x_source_sha": arm64["mundo_x_source_sha"],
        "ram": pick(vm["ram_profile"], RAM),
    }


def compute_delta(first, last, fields):
    return {key: last[key] - first[key] for key in fields
            if isinstance(first.get(key), int) and isinstance(last.get(key), int)}


def compare(first, last):
    steps = last["actual_steps"] - first["actual_steps"]
    wall_ns = last["elapsed_ns"] - first["elapsed_ns"]
    optical_ns = (last["iso_read_us"] - first["iso_read_us"]) * 1000
    if steps <= 0 or wall_ns <= 0 or optical_ns < 0:
        raise ValueError("Matched checkpoints must have increasing steps and time")
    remaining_ns = wall_ns - optical_ns
    return {
        "actual_steps": steps,
        "wall_ns": wall_ns,
        "optical_io_ns": optical_ns,
        "guest_steps_per_wall_second": steps * 1e9 / wall_ns,
        "guest_steps_per_second_excluding_measured_optical_io":
            steps * 1e9 / remaining_ns if remaining_ns > 0 else None,
        "native_arm64_delta": compute_delta(first["native_arm64"], last["native_arm64"], NATIVE),
        "coverage_delta": compute_delta(first["coverage"], last["coverage"], COVERAGE),
        "bios_read_delta": compute_delta(first, last, ("bios_reads", "bios_bytes")),
        "restarts_in_interval": last["restarts"] - first["restarts"],
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
        for line in milestones.read_text().splitlines():
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
    valid_checkpoints = all(
        target in checkpoints and checkpoints[target]["native_arm64_supported"]
        and checkpoints[target]["native_arm64_enabled"]
        and checkpoints[target]["mundo_x_source_sha"] == core_sha
        for target in (350_000_000, 400_000_000, 700_000_000)
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
        "transport": verified,
        "step_accounting": "Guest steps include elements of repeated instructions.",
        "checkpoints": list(checkpoints.values()),
        "validated_measurement": False,
    }
    if valid_checkpoints:
        summary["warmup"] = compare(checkpoints[350_000_000], checkpoints[400_000_000])
        summary["measured_segment"] = compare(checkpoints[400_000_000], checkpoints[700_000_000])
        summary["validated_measurement"] = bool(
            args.exit_code == 0 and valid_source and not summary["measured_segment"]["restarts_in_interval"]
        )
    args.output.write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    return 0 if summary["validated_measurement"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
