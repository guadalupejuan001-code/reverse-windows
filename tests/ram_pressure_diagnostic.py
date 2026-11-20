#!/usr/bin/env python3
"""Validate exported RAM costs from the actual synthetic boot and restart."""
import json
from pathlib import Path
import sys


def verify(snapshot):
    ram = snapshot["ram_profile"]
    costs = ram["access_pressure"]
    assert costs["schema_version"] == 1
    assert type(costs["backing_timing_enabled"]) is bool
    for name, value in costs.items():
        if name == "backing_timing_enabled":
            continue
        assert type(value) is int and value >= 0, (name, value)
    assert costs["forced_failures"] <= costs["forced_reclaims"]
    assert costs["requested_pages"] >= costs["forced_reclaims"]
    assert costs["forced_reclaim_max_ns"] <= costs["forced_reclaim_ns"]
    assert costs["compact_ns"] <= costs["selection_ns"] <= costs["forced_reclaim_ns"]
    assert costs["spill_ns"] <= costs["forced_reclaim_ns"]
    assert costs["compacted_pages"] <= costs["compact_attempts"]
    assert costs["write_promotions"] <= costs["write_promotion_attempts"]
    assert costs["backing_read_failures"] <= costs["backing_read_attempts"]
    assert costs["backing_read_attempts"] - costs["backing_read_failures"] == ram["spill_reads"]
    assert costs["backing_timed_attempts"] <= costs["backing_read_attempts"]
    assert costs["backing_read_max_ns"] <= costs["backing_read_ns"]
    if costs["backing_timed_attempts"] == 0:
        assert costs["backing_read_ns"] == costs["backing_read_max_ns"] == costs["backing_lock_wait_ns"] == 0
    # Disabled timing can retain measurements from an earlier enabled interval.


if __name__ == "__main__":
    evidence = json.loads(Path(sys.argv[1]).read_text())
    for phase in ("before_restart", "after_restart"):
        verify(evidence[phase])
    print("RAM_PRESSURE_DIAGNOSTIC_PASS snapshots=2")
