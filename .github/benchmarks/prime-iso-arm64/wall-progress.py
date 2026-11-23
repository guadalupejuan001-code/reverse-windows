"""Export a wall-clock checkpoint without waiting for a retired-step target."""
import json
from pathlib import Path
import shutil
import sys
import time

from summarize import load_checkpoint

runner = Path(sys.argv[1])
out = Path(sys.argv[2])
target_ms = int(sys.argv[3])
session = runner / "session-reference"
label = f"wall-{target_ms}"
source = session / f"{label}.json"
pid = (runner / "reference.pid").read_text().strip()
while not source.is_file() and not (runner / "reference.exit").is_file():
    if not Path(f"/proc/{pid}").exists():
        raise SystemExit("The comparison driver stopped before recording its exit code")
    time.sleep(5)

out.mkdir(parents=True, exist_ok=True)
report = {"target_wall_ms": target_ms, "captured": source.is_file(),
          "cpu_affinity": (runner / "probe-cpu").read_text().strip()}
if source.is_file():
    raw = json.loads(source.read_text())
    report["checkpoint"] = load_checkpoint(raw)
    for suffix in ("ppm", "txt"):
        file = session / f"{label}.{suffix}"
        if file.is_file():
            shutil.copyfile(file, out / file.name)
else:
    for name in ("terminal.ppm", "text-screen.txt"):
        file = session / name
        if file.is_file():
            shutil.copyfile(file, out / name)
(out / (label + "-progress.json")).write_text(json.dumps(report, indent=2) + "\n")
