"""Archive the original terminal session and the nearest earlier observation."""
import json
from pathlib import Path
import shutil
import sys

run, evidence = map(Path, sys.argv[1:3])
role = sys.argv[3]
exit_code = int(sys.argv[4])
terminal = run / "session.json"
metadata = run / "terminal-meta.json"
snapshots = run / "snapshots.jsonl"
fault = {"probe_exit_code": exit_code, "terminal_observed": False, "cpu_fault": None}
if terminal.is_file():
    shutil.copyfile(terminal, evidence / f"{role}-session.json")
    try:
        vm = json.loads(terminal.read_text())
        fault.update(terminal_observed=True, terminal_status=vm["status"],
                     guest_steps=vm["boot_progress"]["session_steps"],
                     cpu_fault=vm.get("cpu_fault"), primary_machine=vm.get("machine"))
    except (json.JSONDecodeError, KeyError) as error:
        fault["terminal_parse_error"] = str(error)
if metadata.is_file():
    shutil.copyfile(metadata, evidence / f"{role}-terminal-meta.json")
    try:
        fault["terminal_timing"] = json.loads(metadata.read_text())
    except json.JSONDecodeError as error:
        fault["timing_parse_error"] = str(error)
if snapshots.is_file():
    last = None
    with snapshots.open("rb") as stream:
        for line in stream:
            if line.endswith(b"\n"):
                last = line
    if last:
        (evidence / f"{role}-pre-fault-session.json").write_bytes(last)
(evidence / f"{role}-fault.json").write_text(json.dumps(fault, indent=2) + "\n")
