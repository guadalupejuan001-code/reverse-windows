#!/usr/bin/env python3
"""Reject missing, moved, or silently replaced current component dependencies."""
import argparse
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXPECTED = {"mundo_x", "gpu_virtual", "qssd", "reverse_pc"}

def fail(message):
    raise RuntimeError(message)

def run():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest-only", action="store_true")
    args = parser.parse_args()
    lock = json.loads((ROOT / "config/dependencies.lock.json").read_text(encoding="utf-8"))
    if lock.get("schema") != "reverse-windows-dependency-lock-v1":
        fail("Wrong dependency lock schema")
    if lock.get("policy") != "exact-commit-only-no-silent-fallback-no-automatic-merge":
        fail("Immutable pin/fallback policy missing")
    items = lock.get("components", [])
    if {c["key"] for c in items} != EXPECTED or len(items) != len(EXPECTED):
        fail("Missing or duplicate required component")
    for c in items:
        sha = c["commit"]
        if not re.fullmatch(r"[0-9a-f]{40}", sha):
            fail("Unpinned dependency: " + c["key"])
        if not c.get("status", "").startswith("candidate-"):
            fail("Do not label unmerged candidate as stable: " + c["key"])
        checkout = pathlib.Path(c["checkout"])
        if checkout.is_absolute() or ".." in checkout.parts:
            fail("Invalid dependency path: " + c["key"])
        if args.manifest_only:
            continue
        directory = ROOT / checkout
        if not directory.is_dir():
            fail("Dependency missing: " + c["key"])
        try:
            actual = subprocess.check_output(
                ["git", "-C", str(directory), "rev-parse", "HEAD"],
                text=True, stderr=subprocess.STDOUT).strip()
        except subprocess.CalledProcessError as e:
            fail("Dependency cannot be authenticated: " + c["key"] + " " + e.output)
        if actual != sha:
            fail("Stale/substituted dependency " + c["key"] + ": " + actual + " != " + sha)
        for path in c.get("required_paths", []):
            if not (directory / path).is_file():
                fail("Missing canonical component source " + c["key"] + "/" + path)
        print("VERIFIED", c["key"], actual, c["status"], flush=True)
    print("REVERSE_WINDOWS_DEPENDENCIES_VERIFIED", "manifest-only" if args.manifest_only else "exact-heads")

if __name__ == "__main__":
    try:
        run()
    except (RuntimeError, ValueError, KeyError, json.JSONDecodeError) as e:
        print("REVERSE_WINDOWS_DEPENDENCIES_REJECTED", e, file=sys.stderr)
        sys.exit(1)
