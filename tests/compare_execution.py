"""Compare interleaved samples of the actual guest execution engines."""

from collections import defaultdict
from pathlib import Path
from statistics import median
import platform
import sys


def compare(path):
    samples = defaultdict(list)
    for line in path.read_text().splitlines():
        if line.startswith("ENGINE_SAMPLE "):
            _, version, mode, workload, *values = line.split()
            ns, steps, checksum, flags, repeated, projected = map(int, values)
            assert ns > 0
            if version == "after":
                if workload == "sum":
                    assert projected > 0, (mode, workload)
                elif platform.machine() == "aarch64":
                    assert repeated > 0, (mode, workload)
            samples[mode, workload, version].append((ns, steps, checksum, flags))

    assert len(samples) == 8
    for mode in ("protected32", "long64"):
        for workload in ("mix", "sum"):
            before, after = (samples[mode, workload, v] for v in ("before", "after"))
            assert len(before) == len(after) == 7
            assert len({s[1:] for s in before + after}) == 1, (mode, workload)
            old_ns, new_ns = (median(s[0] for s in runs) for runs in (before, after))
            _, steps, checksum, _ = before[0]
            print(f"PRIME_EXECUTION_COMPARE mode={mode} workload={workload} "
                  f"before_ms={old_ns / 1e6:.4f} after_ms={new_ns / 1e6:.4f} "
                  f"speedup={old_ns / new_ns:.2f} iterations=250000 steps={steps} "
                  f"checksum={checksum} samples=7")


if __name__ == "__main__":
    compare(Path(sys.argv[1]))
