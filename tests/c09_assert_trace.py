#!/usr/bin/env python3
"""Assert actual REVERSE Windows C09 JSONL from original x86 CPU/BIOS/hardware."""
from __future__ import annotations

import argparse
import json
import pathlib


def require(ok: bool, why: str) -> None:
    if not ok:
        raise AssertionError(why)


def load(path: pathlib.Path) -> list[dict]:
    events = [json.loads(x) for x in path.read_text(encoding="utf-8").splitlines()]
    require(len(events) >= 3, "trace has no executable evidence")
    require(events[0].get("type") == "start", "no provenance header")
    require(events[0].get("schema") == "reverse-windows-c09-trace-v1", "wrong trace schema")
    require(events[0].get("cpu") == "original-Mundo-X-PcMachine",
            "trace is not from the canonical Mundo X CPU")
    require(events[0].get("windows_boot_verified") is False,
            "C09 must not claim Windows booted")
    require(events[-1].get("type") == "finish", "no terminal summary")
    require(events[-1].get("windows_boot_verified") is False,
            "terminal claim of Windows boot is unsupported")
    require(events[-1].get("steps_observed", 0) > 0,
            "trace never observed guest CPU execution")
    return events


def iso(events: list[dict]) -> None:
    plan = next((x for x in events if x.get("type") == "el-torito-boot-plan"), None)
    require(plan is not None, "El Torito plan absent from actual ISO trace")
    require(plan["kind"] == "iso" and plan["logical_sector_bytes"] == 2048,
            "CD sector size/boot kind wrong")
    require(plan["load_segment"].endswith("2000") and
            plan["initial_rip"].endswith("0000") and plan["boot_drive"] == 0xE0,
            "wrong BIOS CD boot entry point")
    require(plan["source_offset"] == 24 * 2048 and plan["image_bytes"] == 16 * 512,
            "El Torito loader didn't materialize the real catalog entry image")
    require(any(x.get("type") == "bios-interrupt" and
                x.get("vector") == 0x13 and x.get("ah") == 0x42 and
                x.get("drive") == 0xE0 and x.get("media_reads_delta", 0) >= 1 and
                x.get("carry") is False and x.get("handled")
                for x in events),
            "real guest BIOS INT13 AH42 didn't read a subsequent CD sector")
    require(any(x.get("type") == "io-out" and x.get("port") == 0xCF8 and
                x.get("value") == 0x80001000 and x.get("handled")
                for x in events), "real guest never enumerated PCI config port CF8")
    require(any(x.get("type") == "io-in" and x.get("port") == 0xCFC and
                x.get("value") == 0xC0711B36 and x.get("handled")
                for x in events), "real guest didn't see C07 PCI IDE ID")
    require(any(x.get("type") == "io-out" and x.get("port") == 0x1F7 and
                x.get("value") == 0xEC and x.get("handled")
                for x in events), "real guest never issued ATA IDENTIFY")
    require(any(x.get("type") == "io-in" and x.get("port") == 0x1F7 and
                x.get("value") == 0x58 and x.get("handled")
                for x in events), "real guest didn't see ATA DRQ ready status")
    require(any(x.get("type") == "mode-transition" and
                x.get("from") == "real16" and x.get("to") == "protected16"
                for x in events), "real guest didn't enter protected16")
    require(any(x.get("type") == "instruction" and
                x.get("mode_before") == "protected16" and
                x.get("cs_before", "").endswith("0050") and
                x.get("rip_before", "").endswith("0854") and
                x.get("decoded") == "0f00d8" and
                x.get("outcome") in ("executed", "halted")
                for x in events),
            "0F00 LTR at exact historic synthetic CS:IP wasn't truly executed")
    last = events[-1]
    require(last.get("cpu_status") == "halted" and last.get("mode") == "protected16",
            "synthetic ISO did not complete its actual protected16 guest")
    require(last.get("bios_disk_reads", 0) >= 1 and last.get("ata_commands", 0) >= 1,
            "trace lost BIOS / ATA device observations")
    require(last.get("qssd_commits") == 0,
            "read-only synthetic boot did not require writes to QSSD")
    print("C09_SYNTHETIC_ISO_REAL_BOOT_TRACE_PASS ltr=0050:0854 "
          "bios_ah42=true pci_ide=true ata_identify=true windows_booted=false")


def fault(events: list[dict]) -> None:
    terminal = [x for x in events if x.get("type") == "terminal-fault"]
    require(terminal and terminal[-1].get("decoded") == "f090",
            "invalid LOCK NOP must expose exact decoded f090 bytes")
    finish = events[-1]
    require(finish.get("cpu_status") == "fault" and finish.get("terminal_faults") == 1,
            "real fault must stop the emulator with a terminal diagnostic")
    require(finish.get("events_dropped", 0) > 0,
            "bounded trace must drop old execution events, not fail closed")
    print("C09_TERMINAL_OPCODE_FAULT_TRACE_PASS opcode=f0 bounded=true "
          "windows_booted=false")


def quota(events: list[dict]) -> None:
    finish = events[-1]
    require(finish.get("events_dropped", 0) > 0,
            "trace quota didn't bound the event stream")
    require(any(e.get("type") == "mode-transition" for e in events),
            "critical mode transitions must survive the trace quota")
    require(finish.get("cpu_status") == "halted",
            "bounded observer changed guest execution")
    print("C09_TRACE_QUOTA_MODE_MILESTONE_PASS windows_booted=false")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("path", type=pathlib.Path)
    ap.add_argument("mode", choices=("iso", "fault", "quota"))
    args = ap.parse_args()
    events = load(args.path)
    {"iso": iso, "fault": fault, "quota": quota}[args.mode](events)


if __name__ == "__main__":
    main()
