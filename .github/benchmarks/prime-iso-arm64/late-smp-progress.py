"""Publish a provisional observation from a running physical-role process."""
import json
import os
from pathlib import Path
import sys
import time

from summarize import load_checkpoint

runner = Path(sys.argv[1])
output = Path(sys.argv[2])
target = int(sys.argv[3])
role = sys.argv[4] if len(sys.argv) > 4 else "reference"
if role not in ("reference", "candidate"):
    raise SystemExit("Unknown benchmark role")
milestones = runner / f"session-{role}" / "milestones.jsonl"
exit_code = runner / f"{role}.exit"
deadline = time.monotonic() + int(os.environ["PROBE_TIMEOUT_SECONDS"]) + 150
pid = (runner / f"{role}.pid").read_text().strip()
while True:
    with milestones.open() if milestones.is_file() else open("/dev/null") as stream:
        checkpoints = [load_checkpoint(json.loads(line)) for line in stream if line.endswith("\n")]
    latest = checkpoints[-1] if checkpoints else None
    if (latest and latest["target_steps"] >= target) or exit_code.is_file():
        break
    if not Path(f"/proc/{pid}").exists() or time.monotonic() >= deadline:
        raise SystemExit(f"{role} driver disappeared or exceeded the observer deadline")
    time.sleep(10)

output.mkdir(parents=True, exist_ok=True)
summary = {
    "provisional": True,
    "role": role,
    "target_steps": target,
    "observed": latest is not None and latest["target_steps"] >= target,
    "cpu_affinity": (runner / "probe-cpu").read_text().strip(),
    "checkpoints": [point for point in checkpoints if point["target_steps"] >= 9_500_000_000],
}
(output / f"{role}-progress.json").write_text(json.dumps(summary, indent=2) + "\n")
if latest:
    print(f"{role} reached {latest['actual_steps']:,} guest steps "
          f"with {latest['started_vcpus']} vCPUs started")
