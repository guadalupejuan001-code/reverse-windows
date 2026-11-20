#!/usr/bin/env bash
set -euo pipefail

# Run from the project root with verified components in deps/.
baseline=$1
output=$2
mkdir -p "$output"
flags=(-std=c++17 -O2 -DNDEBUG -frounding-math -pthread
  -Ideps/mundo-X/pc-android/native -Ideps/mundo-X/native/include
  -Ideps/Virtual-qSSD-/include -Ideps/GPU-VIRTUAL/include)
c++ "${flags[@]}" -I"$baseline/src" -DPRIME_R101_BASELINE=1 tests/smp_guest_test.cpp -lz -o "$output/smp-before"
bash scripts/build_current_native.sh reverse_windows_smp_guest_test "$output/smp-after"

for trial in 1 2 3 4 5 6 7; do
  order=(before after)
  if ((trial % 2 == 0)); then order=(after before); fi
  for version in "${order[@]}"; do
    "$output/smp-$version" --measure-irq-work | sed "s/^/$version /"
  done
done | tee "$output/smp-irq-samples.log"
python3 tests/compare_execution.py --irq-work "$output/smp-irq-samples.log" | tee "$output/smp-irq-summary.log"
