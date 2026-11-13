#!/usr/bin/env python3
"""Offline diagnosis of actual C09 guest JSONL; never infers Windows boot."""
from __future__ import annotations

import argparse
import collections
import json
import pathlib
import sys


def diagnose(events: list[dict]) -> dict:
    if not events or events[0].get("schema") != "reverse-windows-c09-trace-v1":
        raise ValueError("Not a REVERSE Windows C09 trace")
    if events[-1].get("type") != "finish":
        raise ValueError("Trace lacks a final CPU state; do not infer a successful boot")
    start, final = events[0], events[-1]
    if start.get("cpu") != "original-Mundo-X-PcMachine":
        raise ValueError("Trace does not come from the canonical CPU")
    if start.get("windows_boot_verified") is not False or \
       final.get("windows_boot_verified") is not False:
        raise ValueError("Unverified Windows boot claim in source trace")

    steps = [event for event in events if event.get("type") == "instruction"]
    bios = [event for event in events if event.get("type") == "bios-interrupt"]
    irq = [event for event in events if event.get("type") == "hardware-irq"]
    transitions = [event for event in events if event.get("type") == "mode-transition"]
    faults = [event for event in events if event.get("type") == "terminal-fault"]
    boot = next((event for event in events if event.get("type") ==
                 "el-torito-boot-plan"), None)
    modes = dict(collections.Counter(event["mode_before"] for event in steps))
    io_out = collections.Counter(event["port"] for event in events
                                 if event.get("type") == "io-out")
    io_in = collections.Counter(event["port"] for event in events
                                if event.get("type") == "io-in")

    # This checkpoint is meaningful only as the state of a specific guest,
    # not proof that the ISO was Microsoft Windows.
    checkpoint = [
        event for event in steps
        if event.get("cs_before", "").endswith("0050") and
           event.get("rip_before", "").endswith("0854")
    ]
    fault = faults[0] if faults else None
    if fault:
        blocker = {
            "type": "terminal_instruction_fault",
            "first_observed": fault,
            "requires_guest_or_cpu_fix": True,
        }
    elif final.get("events_dropped", 0):
        blocker = {
            "type": "no_terminal_fault_observed_with_truncated_trace",
            "requires_more_trace_or_guest_evidence": True,
        }
    elif final.get("cpu_status") == "halted":
        blocker = {
            "type": "guest_halt",
            "requires_authentic_os_boot_evidence": True,
        }
    elif final.get("cpu_status") == "ready":
        blocker = {
            "type": "instruction_budget_or_waiting_for_guest_input",
            "requires_more_guest_steps_or_hardware": True,
        }
    else:
        blocker = {
            "type": "guest_state_unverified",
            "cpu_status": final.get("cpu_status"),
            "requires_more_evidence": True,
        }

    return {
        "schema": "reverse-windows-c09-diagnostic-v1",
        "source": start["cpu"],
        "boot_kind": start.get("boot_kind"),
        "el_torito_plan": boot,
        "steps_observed": final.get("steps_observed", 0),
        "events_dropped": final.get("events_dropped", 0),
        "truncated": bool(final.get("events_dropped", 0)),
        "mode_counts_in_captured_window": modes,
        "mode_transitions": transitions,
        "reached_cs_0050_ip_0854": bool(checkpoint),
        "checkpoint_opcode_bytes": checkpoint[0].get("decoded") if checkpoint else None,
        "last_captured_step": steps[-1] if steps else None,
        "first_terminal_blocker": blocker,
        "bios_int13_ah42_reads": sum(
            e.get("media_reads_delta", 0) for e in bios
            if e.get("vector") == 0x13 and e.get("ah") == 0x42
        ),
        "pci_cfg_out_cf8": io_out[0xCF8],
        "pci_cfg_in_cfc": io_in[0xCFC],
        "ata_identify_commands": sum(
            e.get("value") == 0xEC for e in events
            if e.get("type") == "io-out" and e.get("port") == 0x1F7
        ),
        "hardware_irq_vectors": [e["vector"] for e in irq],
        "mmio_reads": final.get("mmio_reads", 0),
        "mmio_writes": final.get("mmio_writes", 0),
        "mmu_page_walks_total": final.get("page_walks_total", 0),
        "ata_sectors_written": final.get("ata_sectors_written", 0),
        "qssd_commits": final.get("qssd_commits", 0),
        "cpu_final_status": final.get("cpu_status"),
        "cpu_final_mode": final.get("mode"),
        "windows_10_boot_verified": False,
        "winpe_boot_verified": False,
        "guest_image_identity_verified": False,
        "limitation": (
            "This reports the observed guest instruction and device path only. "
            "It cannot identify a Microsoft Windows build or prove Windows PE boot "
            "without an authentic local ISO and guest-visible OS evidence."
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace_jsonl", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path,
                        help="Create a new machine-readable diagnostic JSON")
    opts = parser.parse_args()
    events = [json.loads(line) for line in
              opts.trace_jsonl.read_text(encoding="utf-8").splitlines()]
    report = diagnose(events)
    value = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if opts.output:
        with opts.output.open("x", encoding="utf-8") as dst:
            dst.write(value)
    print(value, end="")


if __name__ == "__main__":
    try:
        main()
    except (OSError, KeyError, ValueError, AssertionError,
            json.JSONDecodeError) as error:
        print("C09_TRACE_DIAGNOSIS_REJECTED:", error, file=sys.stderr)
        raise SystemExit(1)
