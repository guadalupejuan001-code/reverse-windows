"""Compare reference and RAM-only official ISO probes on one physical ARM64 runner."""
import argparse
import csv
import json
from pathlib import Path


def interval_result(probe, span):
    by_target = {point["target_steps"]: point for point in probe["checkpoints"]}
    first = by_target[span["from_target_steps"]]
    last = by_target[span["to_target_steps"]]
    spill = span["ram_counter_delta"]
    access = span["ram_access_pressure_delta"]
    return {
        "actual_steps": span["actual_steps"],
        "wall_ns": span["wall_ns"],
        "elapsed_at_end_ns": last["elapsed_ns"],
        "steps_per_wall_second": span["guest_steps_per_wall_second"],
        "mode_changes": span["mode_changes_in_interval"],
        "rip_start": first["rip_hex"],
        "rip_end": last["rip_hex"],
        "irq_delivered": span["irq_delivered_delta"],
        "optical_ns": span["optical_io_ns"],
        "forced_reclaim_ns": access["forced_reclaim_ns"],
        "spill_writes": spill["spill_writes"],
        "spill_reads": spill["spill_reads"],
        "spill_read_us": spill["spill_read_us"],
        "spill_write_us": spill["spill_write_us"],
        "spill_file_payload_delta": spill["spill_file_payload_bytes"],
        "arm64_delta": span["native_arm64_delta"],
        "coverage_delta": span["coverage_delta"],
    }


def compare(reference, candidate, before, after):
    boundaries = ("from_target_steps", "to_target_steps")
    if any(before[field] != after[field] for field in boundaries):
        raise ValueError("Different guest progress boundaries")
    old, new = interval_result(reference, before), interval_result(candidate, after)
    return {**{field: before[field] for field in boundaries}, "reference": old, "candidate": new,
            "gain_percent": (new["steps_per_wall_second"] / old["steps_per_wall_second"] - 1) * 100}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("sources", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    reference = json.loads(args.reference.read_text())
    candidate = json.loads(args.candidate.read_text())
    with args.sources.open(newline="") as file:
        sources = {row["role"]: row for row in csv.DictReader(file, delimiter="\t")}
    ref_src, cand_src = sources["reference"], sources["candidate"]
    config_fields = ("product_sha", "optical_media_bytes", "optical_media_etag", "affinity_cpu",
                     "host_available_mib", "host_threshold_mib", "runner_architecture")
    config = {field: reference[field] == candidate[field] for field in config_fields}
    provenance_verified = (
        ref_src["product_commit"] == cand_src["product_commit"] == reference["product_sha"]
        and ref_src["original_tree"] == cand_src["original_tree"] == ref_src["prepared_tree"]
        and cand_src["prepared_tree"] != ref_src["prepared_tree"]
        and cand_src["mundo_x_commit"] == candidate["mundo_x_sha"]
        and ref_src["mundo_x_commit"] == reference["mundo_x_sha"]
        and ref_src["mundo_x_commit"] != cand_src["mundo_x_commit"]
    )
    controls = ("protected32_control", "early_long64_control", "late_long64_8_2_to_8_6", "late_long64_total")
    intervals = list(zip(reference["late_long64_per_50m"], candidate["late_long64_per_50m"]))
    valid = (provenance_verified and all(config.values()) and len(intervals) == 9
             and reference["validated_measurement"] and candidate["validated_measurement"])
    summary = {
        "validated": valid,
        "same_arm64_runner_and_cpu": True,
        "config_matches": config,
        "sources": sources,
        "transport": {"reference": reference["transport"], "candidate": candidate["transport"]},
        "controls": {key: compare(reference, candidate, reference[key], candidate[key])
                     for key in controls if key in reference and key in candidate},
        "late_long64_per_50m": [compare(reference, candidate, old, new) for old, new in intervals],
    }
    args.output.write_text(json.dumps(summary, indent=2) + "\n")
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
