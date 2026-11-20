"""Compare signed-off guest work using both versions on the same host."""
import statistics
import subprocess
import sys
from collections import defaultdict

programs = dict(zip(("r107", "r108"), sys.argv[1:]))
expected_cases = {"independent", "readers", "counter"}
timings = defaultdict(list)
work = {}
for trial in range(7):
    for version in (("r107", "r108") if trial % 2 == 0 else ("r108", "r107")):
        output = subprocess.check_output([programs[version], "--measure-ram-views"], text=True)
        seen = set()
        for line in output.splitlines():
            marker, case, *fields = line.split()
            if marker != "RAM_VIEW_SAMPLE" or case not in expected_cases or case in seen:
                raise ValueError(line)
            seen.add(case)
            sample = dict(item.split("=") for item in fields)
            retired = int(sample["steps"])
            if work.setdefault(case, retired) != retired or sample["irq"] != "1" or sample["eoi"] != "1":
                raise ValueError(f"Guest work differs: {line}")
            print(version, line, flush=True)
            timings[version, case].append(int(sample["ns"]))
        if seen != expected_cases:
            raise ValueError(f"Missing guest results: {expected_cases - seen}")
for case in sorted(expected_cases):
    before, after = (statistics.median(timings[version, case]) / 1e6 for version in programs)
    print(f"RAM_VIEW_MEDIAN {case} steps={work[case]} r107_ms={before:.6f} r108_ms={after:.6f} runs=7")
