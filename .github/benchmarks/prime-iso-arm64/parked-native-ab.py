"""Run both C06 scheduler variants on the same native host and match guest work."""

import json
from pathlib import Path
import statistics
import subprocess
import sys


def run(builds, evidence, role, label):
    executable = builds / f"build-{role}" / "reverse_windows_smp_guest_test"
    log = evidence / f"{label}-{role}.log"
    with log.open("w") as stream:
        subprocess.run(
            [executable, "--measure-parked-native"], stdout=stream,
            stderr=subprocess.STDOUT, check=True, timeout=60,
        )
    samples = {}
    for line in log.read_text().splitlines():
        if line.startswith("C06_PARKED_NATIVE_SAMPLE "):
            values = {key: int(value) for key, value in
                      (field.split("=", 1) for field in line.split()[1:])}
            timer = values["timer"]
            if timer in samples:
                raise ValueError(f"Duplicate sample for timer={timer} in {log}")
            samples[timer] = values
    if samples.keys() != {0, 1}:
        raise ValueError(f"Expected both timer cases in {log}")
    for timer, sample in samples.items():
        if sample["irq"] != timer or sample["eoi"] != timer:
            raise ValueError(f"Unexpected interrupt or EOI in {log}: {sample}")
        if min(sample["guest_steps"], sample["elapsed_us"]) <= 0:
            raise ValueError(f"Nonpositive work or elapsed time in {log}: {sample}")
    return samples


def main(builds, evidence):
    run(builds, evidence, "reference", "warmup")
    run(builds, evidence, "candidate", "warmup")
    rounds = []
    for index in range(6):
        order = ["reference", "candidate"] if index % 2 == 0 else ["candidate", "reference"]
        rounds.append({"order": order, "samples": {
            role: run(builds, evidence, role, f"round-{index + 1}") for role in order
        }})
    summary = {}
    for timer in (0, 1):
        reference = [entry["samples"]["reference"][timer] for entry in rounds]
        candidate = [entry["samples"]["candidate"][timer] for entry in rounds]
        if any(old["guest_steps"] != new["guest_steps"] for old, new in zip(reference, candidate)):
            raise ValueError(f"Guest work differs in a paired sample for timer={timer}")
        summary[timer] = {
            "guest_steps_per_pair": [sample["guest_steps"] for sample in reference],
            "reference_median_elapsed_us": statistics.median(sample["elapsed_us"] for sample in reference),
            "candidate_median_elapsed_us": statistics.median(sample["elapsed_us"] for sample in candidate),
            "median_paired_speedup": statistics.median(
                old["elapsed_us"] / new["elapsed_us"] for old, new in zip(reference, candidate)),
            "reference_median_parallel_steps": statistics.median(sample["parallel_steps"] for sample in reference),
            "candidate_median_parallel_steps": statistics.median(sample["parallel_steps"] for sample in candidate),
        }
    report = json.dumps({"rounds": rounds, "summary": summary}, indent=2) + "\n"
    (evidence / "parked-native-ab.json").write_text(report)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main(*map(Path, sys.argv[1:]))
