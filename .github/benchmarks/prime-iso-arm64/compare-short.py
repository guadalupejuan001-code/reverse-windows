"""Compare short official ISO probes executed on one physical ARM64 runner."""
import argparse
import json
from pathlib import Path


def compare(reference, candidate):
    old = reference["guest_steps_per_wall_second"]
    new = candidate["guest_steps_per_wall_second"]
    return {"from_target_steps": reference["from_target_steps"],
            "to_target_steps": reference["to_target_steps"],
            "reference_steps_per_second": old, "candidate_steps_per_second": new,
            "candidate_change_percent": (new / old - 1) * 100,
            "reference_mode": reference.get("mode_classification"),
            "candidate_mode": candidate.get("mode_classification"),
            "reference_repeated_loop_steps": reference["native_arm64_delta"].get("repeated_loop_steps"),
            "candidate_repeated_loop_steps": candidate["native_arm64_delta"].get("repeated_loop_steps")}


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("reference", type=Path)
parser.add_argument("candidate", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
reference = json.loads(args.reference.read_text())
candidate = json.loads(args.candidate.read_text())
matched_fields = ("optical_media_bytes", "optical_media_etag", "affinity_cpu", "host_available_mib",
                  "host_threshold_mib", "runner_architecture")
control_pairs = [key for key in ("protected32_control", "early_long64_control")
                 if key in reference and key in candidate]
ref_slices = {(span["from_target_steps"], span["to_target_steps"]): span
              for span in reference["early_long64_per_50m"]}
cand_slices = {(span["from_target_steps"], span["to_target_steps"]): span
               for span in candidate["early_long64_per_50m"]}
summary = {
    "reference_product_sha": reference["product_sha"],
    "candidate_product_sha": candidate["product_sha"],
    "same_runner": True,
    "matched_fields": {field: reference[field] == candidate[field] for field in matched_fields},
    "reference_valid": reference["validated_measurement"],
    "candidate_valid": candidate["validated_measurement"],
    "controls": {key: compare(reference[key], candidate[key]) for key in control_pairs},
    "early_long64_per_50m": [compare(ref_slices[key], cand_slices[key])
                             for key in sorted(ref_slices.keys() & cand_slices.keys())],
}
text = json.dumps(summary, indent=2) + "\n"
args.output.write_text(text)
print(text, end="")
raise SystemExit(0 if summary["reference_valid"] and summary["candidate_valid"]
                 and all(summary["matched_fields"].values()) else 1)
