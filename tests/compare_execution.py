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


def compare_windows(path):
    measurements = defaultdict(list)
    for line in path.read_text().splitlines():
        version, marker, *pairs = line.split()
        assert marker == "PRIME_ENGINE_WINDOW_SAMPLE"
        fields = dict(pair.split("=", 1) for pair in pairs)
        measurements[version, fields["kind"], fields["paced"]].append(fields)
    for kind in ("mix", "sum"):
        before, after = (measurements[v, kind, "0"] for v in ("before", "after"))
        assert len(before) == len(after) == 5
        assert len({(r["steps"], r["checksum"]) for r in before + after}) == 1
        old_ns, new_ns = (median(int(r["wall_ns"]) for r in runs) for runs in (before, after))
        print(f"ENGINE_WINDOW_COMPARE kind={kind} before_ms={old_ns/1e6:.4f} "
              f"after_ms={new_ns/1e6:.4f} speedup={old_ns/new_ns:.2f} steps={after[0]['steps']}")
    runs = measurements["after", "sum", "1"]
    assert len(runs) == 5 and all(int(r["steps"]) == 4_500_000_000_005 for r in runs)
    rate = median(int(r["steps"])*1e9/int(r["wall_ns"]) for r in runs)
    print(f"ENGINE_WINDOW_SUSTAIN target=4500000000000 measured_steps_per_second={rate:.0f} "
          f"median_full_windows={median(int(r['full']) for r in runs)} measured_runs=5")


if __name__ == "__main__":
    if sys.argv[1] == "--windows":
        compare_windows(Path(sys.argv[2]))
    else:
        compare(Path(sys.argv[1]))
