"""Compare equal guest work with and without a priority-masked interrupt."""
import argparse
import math
import statistics
import subprocess


def compare(before, after, labels, grants, trials):
    budgets = (8192, 0x7fffffff) if grants else (8192,)
    cases = {(budget, pending) for budget in budgets for pending in (0, 1)}
    results = {(label, key): [] for label in labels for key in cases}
    executables = list(zip(labels, (before, after)))
    expected = {}
    for trial in range(trials):
        order = executables if trial % 2 == 0 else reversed(executables)
        for name, path in order:
            mode = "--measure-ap-scan-grants" if grants else "--measure-ap-scan"
            lines = subprocess.check_output([path, mode], text=True).splitlines()
            seen = set()
            for line in lines:
                if not line.startswith("AP_SCAN_SAMPLE "):
                    continue
                fields = dict(field.split("=") for field in line.split()[1:])
                pending = int(fields["pending"])
                grant = int(fields.get("grant", 8192))
                key = grant, pending
                if key not in cases or key in seen:
                    raise ValueError(f"Unexpected sample for {name}: {line}")
                seen.add(key)
                guest = tuple(int(fields[k]) for k in ("steps", "bsp_steps", "checksum", "flags", "deliveries"))
                if guest != expected.setdefault(pending, guest) or guest[-1] != pending:
                    raise ValueError(f"Guest state differs across versions or grants: {name}, {line}")
                if (int(fields["grouped"]) > 0) != (name != "r105"):
                    raise ValueError(f"Unexpected scan path for {name}: {line}")
                if grants and (int(fields["cooperative_steps"]) > 0) != (name == labels[1]):
                    raise ValueError(f"Unexpected cooperative path for {name}: {line}")
                ns = int(fields["wall_ns"])
                if ns <= 0:
                    raise ValueError(f"Invalid elapsed time for {name}: {line}")
                results[name, key].append(ns)
                print(name, line, flush=True)
            if seen != cases:
                raise ValueError(f"Incomplete sample set for {name}: {seen}")
    for grant, pending in sorted(cases):
        for name in labels:
            samples = sorted(results[name, (grant, pending)])
            p95 = samples[math.ceil(0.95 * len(samples)) - 1]
            print(f"AP_SCAN_LATENCY version={name} grant={grant} pending={pending} runs={trials} "
                  f"median_ms={statistics.median(samples)/1e6:.6f} p95_ms={p95/1e6:.6f} "
                  f"max_ms={samples[-1]/1e6:.6f} under_100us={sum(n < 100000 for n in samples)}")
        if not grants:
            old, new = [statistics.median(results[name, (grant, pending)]) / 1e6 for name in labels]
            print(f"AP_SCAN_MEDIAN pending={pending} runs={trials} {labels[0]}_ms={old:.3f} {labels[1]}_ms={new:.3f}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--labels", nargs=2, default=("r105", "r106"))
    parser.add_argument("--grants", action="store_true")
    args = parser.parse_args()
    compare(args.before, args.after, args.labels, args.grants, 21 if args.grants else 7)
