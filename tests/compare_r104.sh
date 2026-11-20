#!/usr/bin/env bash
set -euo pipefail
baseline=$1
old_engine=$2
output=$3
mkdir -p "$output"
for version in r103 r104; do
  if [[ "$version" == r104 ]]; then
    bash scripts/build_current_native.sh \
      reverse_windows_boot_segment_bench "$output/segment-$version" \
      reverse_windows_prime_vbe_guest_test "$output/vbe-$version" \
      reverse_windows_smp_guest_test "$output/bsp-$version"
    continue
  fi
  src=src
  engine=deps/mundo-X
  extra=()
  if [[ "$version" == r103 ]]; then
    src="$baseline/src"
    engine="$old_engine"
    extra=(-DPRIME_R103_BASELINE)
  fi
  flags=(-std=c++17 -O2 -DNDEBUG -frounding-math -pthread -I"$src"
    -I"$engine/pc-android/native" -I"$engine/native/include"
    -Ideps/Virtual-qSSD-/include -Ideps/GPU-VIRTUAL/include '-DREVERSE_MUNDO_X_SOURCE_SHA="benchmark"')
  c++ "${flags[@]}" tests/boot_segment_bench.cpp -lz -o "$output/segment-$version"
  c++ "${flags[@]}" tests/prime_vbe_guest_test.cpp -lz -o "$output/vbe-$version"
  c++ "${flags[@]}" "${extra[@]}" tests/smp_guest_test.cpp -lz -o "$output/bsp-$version"
done
for trial in {1..7}; do
  versions=(r103 r104)
  if (( trial%2 == 0 )); then versions=(r104 r103); fi
  for v in "${versions[@]}"; do
    "$output/segment-$v" | sed "s/^/$v /"
    "$output/vbe-$v" --measure-boot-surface | sed "s/^/$v /"
    if (( trial == 1 )); then "$output/bsp-$v" --measure-idle-bsp | sed "s/^/$v /"; fi
  done
done | tee "$output/samples.log"
python3 - "$output/samples.log" <<'PY'
import platform, statistics, sys
from collections import defaultdict
times, checks = defaultdict(list), defaultdict(set)
for line in open(sys.argv[1], encoding="utf-8"):
    version, probe, *fields = line.split()
    v = {k: int(n) for k, n in (field.split("=") for field in fields)}
    if probe == "PRIME_IDLE_BSP_PASS":
        if v["retired"] != (16384 if version == "r103" else 32768) or v["wake_irq"] != 1:
            raise ValueError(line)
    elif probe == "PRIME_BOOT_GS_PASS":
        if platform.machine()=="aarch64" and version=="r104" and v["native"] < v["steps"]*0.99:
            raise ValueError(line)
        checks[probe].add((v["steps"],v["checksum"]))
        times["GS",version].append(v["elapsed_ns"])
    else:
        checks[probe,v["width"]].add((v["frames"],v["checksum"]))
        for mode in ("full_ns","delta_ns"):
            times[f'VBE_{v["width"]}_{mode}',version].append(v[mode])
if len(times)!=10 or len(checks)!=3 or any(len(group)!=1 for group in checks.values()):
    raise ValueError("workloads differ")
for (name,version), values in sorted(times.items()):
    if len(values)!=7: raise ValueError("missing trial")
    print(f"{name} {version} median_ns={statistics.median(values)}")
PY
