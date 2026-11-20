"""Compare equal guest work with and without a priority-masked interrupt."""
import statistics
import subprocess
import sys


def main(before, after):
    results = {(name, pending): [] for name in ("r105", "r106") for pending in (0, 1)}
    executables = [("r105", before), ("r106", after)]
    expected = {}
    for trial in range(7):
        for name, path in executables[::1 if trial % 2 == 0 else -1]:
            lines = subprocess.check_output([path, "--measure-ap-scan"], text=True).splitlines()
            for line in lines:
                if not line.startswith("AP_SCAN_SAMPLE "):
                    continue
                print(name, line, flush=True)
                fields = dict(field.split("=") for field in line.split()[1:])
                pending = int(fields["pending"])
                guest = tuple(int(fields[key]) for key in ("steps", "bsp_steps", "checksum", "flags", "deliveries"))
                if guest != expected.setdefault(pending, guest) or guest[-1] != pending:
                    raise ValueError(f"Guest results differ for {name}, pending={pending}: {guest}")
                if (int(fields["grouped"]) > 0) != (name == "r106"):
                    raise ValueError(f"Unexpected scan path for {name}: {line}")
                results[name, pending].append(int(fields["wall_ns"]) / 1e6)
    for pending in (0, 1):
        timings = [results[name, pending] for name, _ in executables]
        if any(len(samples) != 7 for samples in timings):
            raise ValueError(f"Incomplete sample set for pending={pending}")
        old, new = map(statistics.median, timings)
        print(f"AP_SCAN_MEDIAN pending={pending} runs=7 r105_ms={old:.3f} r106_ms={new:.3f}")


if __name__ == "__main__":
    main(*sys.argv[1:])
