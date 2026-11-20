#!/usr/bin/env bash
set -euo pipefail
base=$1
output=$2
mkdir -p "$output"
flags=(-std=c++17 -O2 -DNDEBUG -frounding-math -pthread
  -Ideps/mundo-X/pc-android/native -Ideps/mundo-X/native/include
  -Ideps/Virtual-qSSD-/include -Ideps/GPU-VIRTUAL/include)
c++ "${flags[@]}" -I"$base/src" -DPRIME_R102_BASELINE tests/smp_guest_test.cpp -lz -o "$output/idle-r102"
c++ "${flags[@]}" -I"$base/src" -DPRIME_R102_BASELINE tests/vram_partial_bench.cpp -lz -o "$output/vram-r102"
c++ "${flags[@]}" -Isrc tests/vram_partial_bench.cpp -lz -o "$output/vram-r103"
# The preceding IRQ regression compiled this very revision with the same flags.
test -x "$output/smp-after"
for trial in 1 2 3 4 5 6 7; do
  order=(r102 r103)
  if ((trial % 2 == 0)); then order=(r103 r102); fi
  for version in "${order[@]}"; do
    smp="$output/idle-r102"
    if [[ "$version" == r103 ]]; then smp="$output/smp-after"; fi
    "$smp" --measure-idle-aps | sed "s/^/$version /"
    "$output/vram-$version" | sed "s/^/$version /"
  done
done | tee "$output/r103-samples.log"
python3 - "$output/r103-samples.log" <<'PY'
import statistics
import sys
from collections import defaultdict
samples = defaultdict(list)
checksums = set()
for line in open(sys.argv[1], encoding="utf-8"):
    version, probe, *pairs = line.split()
    value = dict(pair.split("=") for pair in pairs)
    if probe == "PRIME_PARTIAL_VRAM_PASS":
        checksums.add(value["checksum"])
        samples[version, "partial_vram_ns"].append(int(value["elapsed_ns"]))
    elif probe == "PRIME_BSP_IRQ_AP_REP_PASS":
        expected = "0" if version == "r102" else "15"
        if value["batched"] != expected or value["delivered"] != "1":
            raise SystemExit(f"unexpected cross-vCPU interrupt result {line}")
    else:
        expected = (2048, 16) if version == "r102" else (32768, 1)
        if (int(value["retired"]), int(value["slices"])) != expected:
            raise SystemExit(f"unexpected idle-AP scheduling {line}")
        samples[version, "bsp_equal_work_ns_threaded_" + value["threaded"]].append(int(value["equal_work_ns"]))
if len(samples) != 6 or len(checksums) != 1 or any(len(v) != 7 for v in samples.values()):
    raise SystemExit("incomplete or mismatched comparison")
for metric in sorted({key[1] for key in samples}):
    before, after = (statistics.median(samples[v, metric]) for v in ("r102", "r103"))
    print(f"{metric} r102={before} r103={after} trials=7 ratio={before / after:.3f}")
PY
