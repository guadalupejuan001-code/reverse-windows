#!/usr/bin/env bash
set -euo pipefail
role=$1
case $role in reference|candidate) ;; *) exit 2 ;; esac
finish() {
  local code=$?
  printf '%s\n' "$code" > "$RUNNER_TEMP/$role.exit.tmp"
  mv "$RUNNER_TEMP/$role.exit.tmp" "$RUNNER_TEMP/$role.exit"
}
trap finish EXIT
bench="$GITHUB_WORKSPACE/.github/benchmarks/prime-iso-arm64"
evidence="$RUNNER_TEMP/evidence"
IFS=$'\t' read -r selected product_sha core_sha < <(awk -F '\t' -v role="$role" '$1==role {print}' "$RUNNER_TEMP/targets.tsv")
test "$selected" = "$role"
export PROBE_CPU
PROBE_CPU=$(cat "$RUNNER_TEMP/probe-cpu")
iso="$RUNNER_TEMP/windows-eval-$role.sparse"
output="$RUNNER_TEMP/session-$role"
truncate -s 5550497792 "$iso"
chmod 400 "$iso"
probe=("$RUNNER_TEMP/build-$role/session_probe" "$iso" "$output" "$STOP_TARGET" "$PROBE_TIMEOUT_SECONDS"
       "$AVAILABLE_MIB" "$THRESHOLD_MIB")
if [[ -n "${EXCEPTION_REPLAY_POST_NMI_STEPS:-}" ]]; then
  probe+=("$EXCEPTION_REPLAY_POST_NMI_STEPS")
fi
optical_env=(EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
             LD_PRELOAD="$RUNNER_TEMP/verified_iso.so" R110_REMOTE_ISO_STUB="$iso")
if timeout "$((PROBE_TIMEOUT_SECONDS + 120))s" taskset -c "$PROBE_CPU" env "${optical_env[@]}" "${probe[@]}" \
  2> "$evidence/$role.stderr" | tee "$evidence/$role.stdout"; then
  probe_exit=0
else
  probe_exit=${PIPESTATUS[0]}
  if ((probe_exit == 0)); then probe_exit=1; fi
fi
python3 "$bench/archive-terminal-fault.py" "$output" "$evidence" "$role" "$probe_exit"
for artifact in "$output"/screen-*.ppm "$output/pre-fault-latest.ppm" "$output/pre-fault-frame.json" \
                "$output/terminal.ppm" "$output/text-screen.txt" "$output/first-nmi.ppm" \
                "$output/first-nmi-session.json" "$output"/exceptions-*.json; do
  if [[ -f "$artifact" ]]; then
    cp "$artifact" "$evidence/$role-${artifact##*/}"
  fi
done
if [[ -n "${EXCEPTION_REPLAY_POST_NMI_STEPS:-}" && -f "$output/terminal-meta.json" ]]; then
  STOP_TARGET=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["retired_stop_target"])' \
    "$output/terminal-meta.json")
fi
PRODUCT_SHA="$product_sha" MUNDO_X_SHA="$core_sha" python3 "$bench/summarize.py" \
  --run "$output" --transport-log "$evidence/$role.stderr" --output "$evidence/$role.json" --exit-code "$probe_exit"
