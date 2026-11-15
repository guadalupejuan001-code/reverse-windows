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

def checkout_locked(component, directory):
    if not (directory / ".git").exists():
        if directory.exists() and (not directory.is_dir() or any(directory.iterdir())):
            fail("Dependency path is not an empty directory or Git checkout: " + str(directory))
        subprocess.run(["git", "init", "--quiet", str(directory)], check=True)
    git = ["git", "-C", str(directory)]
    if subprocess.check_output(git + ["status", "--porcelain"], text=True).strip():
        fail("Save local dependency changes before checking out " + component["key"])
    current = subprocess.run(git + ["rev-parse", "--verify", "HEAD"],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    if current.returncode == 0 and current.stdout.strip() == component["commit"]:
        return
    url = "https://github.com/" + component["repository"] + ".git"
    subprocess.run(git + ["fetch", "--quiet", "--depth", "1", url, component["commit"]], check=True)
    subprocess.run(git + ["checkout", "--quiet", "--detach", "FETCH_HEAD"], check=True)

def run():
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--manifest-only", action="store_true")
    mode.add_argument("--checkout", action="store_true", help="Check out the exact dependency commits")
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
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", c["repository"]):
            fail("Invalid dependency repository: " + c["key"])
        checkout = pathlib.Path(c["checkout"])
        if checkout.is_absolute() or ".." in checkout.parts:
            fail("Invalid dependency path: " + c["key"])
    if args.manifest_only:
        print("REVERSE_WINDOWS_DEPENDENCIES_VERIFIED manifest-only")
        return
    for c in items:
        directory = ROOT / c["checkout"]
        sha = c["commit"]
        if args.checkout:
            checkout_locked(c, directory)
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
    print("REVERSE_WINDOWS_DEPENDENCIES_VERIFIED exact-heads")

if __name__ == "__main__":
    try:
        run()
    except (RuntimeError, ValueError, KeyError, subprocess.CalledProcessError) as e:
        print("REVERSE_WINDOWS_DEPENDENCIES_REJECTED", e, file=sys.stderr)
        sys.exit(1)
