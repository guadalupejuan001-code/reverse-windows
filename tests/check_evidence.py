#!/usr/bin/env python3
"""Only accept machine-generated guest execution, not a UI or static boot screenshot."""
import json
import pathlib
import sys

def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_evidence.py PATH")
    evidence = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
    if evidence["schema"] != "reverse-windows-boot-evidence-v1":
        raise RuntimeError("Wrong evidence schema")
    if evidence["guest_kind"] != "synthetic-x64":
        raise RuntimeError("Not the expected real x86/x64 guest demo")
    if evidence["source"] != "original-Mundo-X-PcMachine":
        raise RuntimeError("Source not canonical")
    if evidence["windows10_boot_verified"] is not False:
        raise RuntimeError("Do not infer Windows boot from synthetic guest")
    machine = evidence["machine"]
    if machine["engine"] != "cosmic-pc-own-x86":
        raise RuntimeError("Using an unrelated or mocked CPU")
    if machine["cpu_mode"] != "long64" or machine["status"] != "halted":
        raise RuntimeError("CPU did not enter x64 long mode and execute HLT")
    if not (0 < machine["instructions"] <= 200000):
        raise RuntimeError("No real guest CPU steps")
    if machine["boot_kind"] != "demo-x64":
        raise RuntimeError("Wrong guest boot kind")
    if machine.get("windows_ready") is not False:
        raise RuntimeError("Windows readiness must not be fabricated")
    if machine["ram_bytes"] != 2048 * 1024 * 1024:
        raise RuntimeError("Logical guest RAM inconsistent")
    if not isinstance(evidence["vga_text"], str):
        raise RuntimeError("No real guest VGA sample")
    print("REVERSE_WINDOWS_REAL_GUEST_X64_PASS", "instructions=" + str(machine["instructions"]), "CPU=" + machine["cpu_mode"])

if __name__ == "__main__":
    main()
