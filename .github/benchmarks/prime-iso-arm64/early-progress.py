"""Export checkpoints from the already-running candidate, then let it continue."""
import argparse
import json
from pathlib import Path
import time
from summarize import compare, load_checkpoint, long64_interval


def checkpoints(path):
    if not path.exists():
        return {}
    with path.open() as stream:
        records = [load_checkpoint(json.loads(line)) for line in stream if line.endswith("\n")]
    return {record["target_steps"]: record for record in records}


cli = argparse.ArgumentParser(description=__doc__)
cli.add_argument("runner", type=Path)
cli.add_argument("output", type=Path)
args = cli.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
evidence = args.runner / "evidence"
reference = json.loads((evidence / "reference.json").read_text())
pin = json.loads((evidence / "targets.json").read_text())["candidate"]
milestones = args.runner / "session-candidate/milestones.jsonl"
exit_status = args.runner / "candidate.exit"
deadline = time.monotonic() + 900
while True:
    points = checkpoints(milestones)
    if 1_000_000_000 in points or exit_status.exists() or time.monotonic() >= deadline:
        break
    time.sleep(5)

controls = {}
for name, start, stop, comparator in (
    ("protected32_control", 400_000_000, 700_000_000, compare),
    ("early_long64_control", 750_000_000, 1_000_000_000, long64_interval),
):
    if name not in reference or start not in points or stop not in points:
        continue
    if any(not points[n]["native_arm64_enabled"] or points[n]["mundo_x_source_sha"] != pin["mundo_x_sha"]
           for n in (start, stop)):
        continue
    original, current = reference[name], comparator(points[start], points[stop])
    comparable = (reference["validated_measurement"] and all(original[key] == current[key] for key in (
        "mode_changes_in_interval", "restarts_in_interval", "from_cpu_mode", "to_cpu_mode", "bios_read_delta")))
    percent = (current["guest_steps_per_wall_second"] / original["guest_steps_per_wall_second"] - 1) * 100
    controls[name] = {"reference": original, "candidate": current,
                      "relative_change_percent": percent if comparable else None}
report = {
    "provisional_until_full_probe_and_transport_finish": True,
    "reference_product_sha": reference["product_sha"],
    "candidate_product_sha": pin["product_sha"],
    "reference_transport": reference["transport"],
    "candidate_latest_steps": max((point["actual_steps"] for point in points.values()), default=None),
    "candidate_finished": exit_status.exists(),
    "same_runner_cpu": reference["affinity_cpu"],
    "controls": controls,
}
(args.output / "early-comparison.json").write_text(json.dumps(report, indent=2) + "\n")
for name, pair in controls.items():
    print(f"{name} reference {pair['reference']['guest_steps_per_wall_second']:,.0f} "
          f"candidate {pair['candidate']['guest_steps_per_wall_second']:,.0f} steps/s, provisional")
