#!/usr/bin/env bash
set -euo pipefail

# Build current-revision comparison executables through the production component
# graph. Historic comparison slots keep their own direct compiler commands and
# pinned sources. Keep the existing benchmark optimization/FP policy unchanged.
if (( $# == 0 || $# % 2 != 0 )); then
  echo "usage: $0 TARGET OUTPUT [TARGET OUTPUT ...]" >&2
  exit 2
fi
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=${REVERSE_CURRENT_NATIVE_BUILD:-"$root/build/current-native"}
cmake_bin=${CMAKE_COMMAND:-cmake}
targets=()
outputs=()
while (( $# )); do
  if [[ ! $1 =~ ^reverse_windows_[a-z0-9_]+$ ]]; then
    echo "invalid current native target: $1" >&2
    exit 2
  fi
  targets+=("$1")
  outputs+=("$2")
  shift 2
done

"$cmake_bin" -S "$root" -B "$build" \
  -DREVERSE_MUNDO_X_ROOT="$root/deps/mundo-X" \
  -DREVERSE_QSSD_ROOT="$root/deps/Virtual-qSSD-" \
  -DREVERSE_GPU_VIRTUAL_ROOT="$root/deps/GPU-VIRTUAL" \
  -DREVERSE_REGRAPHICS_ROOT="$root/deps/REVERSE-PC" \
  -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG' \
  -DREVERSE_WINDOWS_GLES_GUEST=OFF -DBUILD_TESTING=ON
"$cmake_bin" --build "$build" --parallel "${REVERSE_NATIVE_BUILD_JOBS:-2}" --target "${targets[@]}"
for ((i = 0; i < ${#targets[@]}; ++i)); do
  mkdir -p -- "$(dirname -- "${outputs[i]}")"
  "$cmake_bin" -E copy_if_different "$build/${targets[i]}" "${outputs[i]}"
done
