#!/usr/bin/env python3
"""Compare paired runs of the same generated El Torito boot interval."""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

from c10_android_boot_iso import image


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


TIMING_ONLY = {"run_ns", "sampled_iterations"}


def guest_state(report):
    profile = report["boot_profile"]

    def state(fields):
        return {key: value for key, value in fields.items() if key not in TIMING_ONLY}

    return {
        "start": state(profile["start"]),
        "checkpoints": [state(p["state"]) for p in profile["checkpoints"]],
        "current": state(profile["current"]),
        "vga_text": report["vga_text"],
        "ide": report["ide_c07"],
        "board": report["motherboard_c06"],
        "disk_commits": report["qssd_commits"],
    }


def validate(report, timed, start, interval, stop):
    profile = report["boot_profile"]
    check(profile["start"]["steps"] == start, "profile start did not match")
    check(profile["checkpoint_interval_steps"] == interval, "wrong checkpoint interval")
    checkpoints = profile["checkpoints"]
    absolute_steps = [p["state"]["steps"] for p in checkpoints]
    steps = profile["current"]["steps"]
    if stop is None:
        check(report["machine"]["status"] == "waiting_key" and
              report["ide_c07"]["irq14_delivered"] == 1 and
              "ENTER: WRITE TEST MBR AND REBOOT" in report["vga_text"],
              "synthetic guest did not reach the C01 BIOS prompt after exercising ATA")
    else:
        check(report["profile_until_reached"] and steps == stop and report["machine"]["status"] == "running",
              "bounded interval did not stop at its requested guest state")
    check(steps == report["machine"]["instructions"] and
          profile["current"]["batched_steps"] > 0 and
          report["profile_initial_qssd_commits"] == report["qssd_commits"] == 0,
          "guest step count, batching or initial disk state is incorrect")
    completed = (steps - start) // interval
    check(profile["checkpoints_seen"] == completed and
          absolute_steps == [start + n * interval for n in range(max(1, completed - 7), completed + 1)],
          "checkpoint cadence or retention is incorrect")

    if not timed:
        check("boot_time_samples" not in report and "boot_code_regions" not in report,
              "the timing-free pass unexpectedly sampled the interpreter")
        return
    timing = report["boot_time_samples"]
    check(timing["sampled_windows"] > 0 and
          0 < timing["sampled_iterations"] <= timing["window_iterations"] * timing["sampled_windows"],
          "no valid timed windows")
    check(timing["sampled_scalar_ns"] ==
          sum(timing["sampled_scalar_" + part + "_ns"]
              for part in ("fetch_decode", "other_ram", "guest_io", "cpu_other")),
          "scalar samples overlap or lose time")
    check(timing["sampled_iteration_ns"] ==
          sum(timing["sampled_" + part + "_ns"]
              for part in ("device", "batch_dispatch", "batch_execute", "scalar", "loop_other")) and
          timing["run_ns"] >= timing["sampled_iteration_ns"],
          "device and CPU samples overlap or exceed the run")
    parts = ("fetch_decode", "other_ram", "guest_io") if stop is None else ("fetch_decode", "other_ram")
    for part in parts:
        check(timing["sampled_scalar_" + part + "_ns"] > 0, f"fixture did not sample {part}")
    for point in checkpoints:
        check(point["state"]["steps"] - point["from_steps"] == interval and
              point["timing_in_interval"]["run_ns"] > 0,
              "checkpoint is missing its interval timing")
    regions = report["boot_code_regions"]
    check(regions["region_bytes"] == 64 and
          regions["samples"] == regions["attributed_samples"] == timing["sampled_scalar_steps"],
          "code regions lost a retired scalar sample")
    loop = next((row for row in regions["hotspots"] if row["ip"] == 0x1400), None)
    check(loop is not None and loop["mode"] == "protected32" and
          loop["lower_bound_ns"] > regions["unlisted_upper_bound_ns"],
          "known synthetic loop is outside the reliable reported regions")
    for row in regions["hotspots"]:
        check(row["ip"] % regions["region_bytes"] == 0 and
              row["lower_bound_ns"] == row["estimated_sample_ns"] - row["max_error_ns"],
              "invalid region bounds")


def run_probe(probe, iso, disk, timed, start=32768, interval=32768, stop=None):
    args = [
        str(probe), "--iso", str(iso), "--qssd-disk", str(disk), "--create-qssd-gib", "64",
        "--profile-from", str(start), "--profile-every", str(interval),
        "--steps", "500000",
    ]
    if stop is not None:
        args.extend(("--profile-until", str(stop)))
    if not timed:
        args.append("--profile-counts-only")
    run = subprocess.run(args, capture_output=True, text=True, timeout=45)
    check(run.returncode == 0, f"native probe failed with {run.returncode}\n{run.stderr}")
    report = json.loads(run.stdout)
    validate(report, timed, start, interval, stop)
    return report


def sample_long_mode(probe):
    command = [
        str(probe), "--demo-x64", "--profile-from", "3140", "--profile-every", "4096", "--steps", "50000",
    ]
    def run_profile(args):
        run = subprocess.run(args, capture_output=True, text=True, timeout=10, check=True)
        return json.loads(run.stdout)

    control = run_profile(command + ["--profile-counts-only"])
    report = run_profile(command)
    check("boot_time_samples" not in control and "boot_code_regions" not in control and
          guest_state(report) == guest_state(control) and report["machine"] == control["machine"],
          "paging or guest state changed when timing was enabled in long mode")
    profile, timing = report["boot_profile"], report["boot_time_samples"]
    check(profile["start"]["mode"] == profile["current"]["mode"] == "long64" and
          report["machine"]["status"] == "halted" and
          profile["current"]["page_translation_hits"] > profile["start"]["page_translation_hits"] and
          timing["sampled_scalar_other_ram_ns"] > 0 and
          any(row["mode"] == "long64" for row in report["boot_code_regions"]["hotspots"]),
          "paged long-mode work did not register in the native profile")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("--repeat", type=int, default=3, choices=range(1, 22))
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve(strict=True)
    expected = None
    timed_ns, counts_ns, samples = [], [], []
    with tempfile.TemporaryDirectory(prefix="reverse-prime-boot-profile-") as temp:
        root = Path(temp)
        iso = root / "synthetic-boot.iso"
        fixture = image(profile=True)
        iso.write_bytes(fixture)
        for repeat in range(args.repeat):
            for timed in (True, False) if repeat % 2 else (False, True):
                disk = root / f"{repeat}-{int(timed)}.rqvd"
                report = run_probe(probe, iso, disk, timed)
                trace = guest_state(report)
                if expected is None:
                    expected = trace
                check(trace == expected, "timing changed guest registers, board or terminal state")
                if timed:
                    samples.append(report)
                    timed_ns.append(report["profile_wall_ns"])
                else:
                    counts_ns.append(report["profile_wall_ns"])
        point = expected["checkpoints"][0]
        shortened = run_probe(probe, iso, root / "cut.rqvd", True, stop=point["steps"])
        check(guest_state(shortened)["current"] == point,
              "short run did not terminate on its checkpoint")
        output_alias = root / "same-medium.json"
        output_alias.hardlink_to(iso)
        refused = subprocess.run([
            str(probe), "--iso", str(iso), "--evidence", str(output_alias), "--profile-every", "4096",
        ], capture_output=True, text=True, timeout=10)
        check(refused.returncode == 2 and "Evidence path aliases" in refused.stderr,
              "profiling evidence must not overwrite its input media")
        check(iso.read_bytes() == fixture, "run modified synthetic boot media")

    timing = {
        "counts_only_wall_ns": counts_ns, "instrumented_wall_ns": timed_ns,
        "counts_only_median_ns": int(statistics.median(counts_ns)),
        "instrumented_median_ns": int(statistics.median(timed_ns)),
    }
    evidence = {
        "schema": "reverse-windows-prime-boot-profile-v1",
        "fixture": "synthetic-el-torito",
        "fixture_sha256": hashlib.sha256(fixture).hexdigest(),
        "host_architecture": platform.machine(),
        "repeat_count": args.repeat,
        "matching_guest_states": True,
        "guest_state": expected,
        "timing": timing,
        "sample_reports": samples,
        "long_mode_sample": sample_long_mode(probe),
        "matching_long_mode_state": True,
    }
    if args.evidence:
        args.evidence.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print("PRIME_C02_BOOT_PROFILE_PASS", json.dumps({
        "host": evidence["host_architecture"], "repeats": args.repeat,
        "start": expected["start"]["steps"], "end": expected["current"]["steps"],
        "retained_checkpoints": len(expected["checkpoints"]), **timing,
    }, separators=(",", ":")))


if __name__ == "__main__":
    main()
