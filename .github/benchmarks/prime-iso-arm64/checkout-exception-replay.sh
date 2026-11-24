#!/usr/bin/env bash
set -euo pipefail
product=$1
product_sha=$2
core_sha=$3
diagnostic_sha=$4
evidence=$5
[[ "$diagnostic_sha" =~ ^[0-9a-f]{40}$ ]] && test "$diagnostic_sha" != "$core_sha"

bash "$(dirname "$0")/checkout-product.sh" "$product" "$product_sha" "$core_sha"
lock="$product/config/dependencies.lock.json"
core="$product/deps/mundo-X"
cp "$lock" "$evidence/reference-original-dependencies.lock.json"
git -C "$core" fetch --depth=2 https://github.com/guadalupejuan001-code/mundo-X.git "$diagnostic_sha"
test "$(git -C "$core" rev-parse FETCH_HEAD)" = "$diagnostic_sha"
test "$(git -C "$core" show -s --format=%P "$diagnostic_sha")" = "$core_sha"
git -C "$core" diff --no-ext-diff "$core_sha" "$diagnostic_sha" > "$evidence/reference-core-diagnostics.patch"
git -C "$core" checkout --detach "$diagnostic_sha"

# The exact product source is retained. This ephemeral dependency pin identifies
# the single diagnostic core used to build the replay and ordinary test gates.
python3 - "$lock" "$core_sha" "$diagnostic_sha" <<'PY'
import json
from pathlib import Path
import sys

lock = Path(sys.argv[1])
base, diagnostic = sys.argv[2:]
record = json.loads(lock.read_text())
component, = [component for component in record['components'] if component['key'] == 'mundo_x']
if component['commit'] != base:
    raise SystemExit('Unexpected base core pin in the original product checkout')
component['commit'] = diagnostic
lock.write_text(json.dumps(record, indent=2) + '\n')
PY
test "$(git -C "$product" diff --name-only HEAD)" = 'config/dependencies.lock.json'
cp "$lock" "$evidence/reference-replay-dependencies.lock.json"
git -C "$product" diff --no-ext-diff HEAD -- config/dependencies.lock.json > "$evidence/reference-replay-lock.patch"
python3 "$product/scripts/verify_deps.py"
