#!/usr/bin/env bash
set -euo pipefail
product=$1
product_sha=$2
core_sha=$3
git init "$product"
git -C "$product" fetch --depth=1 "https://github.com/${GITHUB_REPOSITORY}.git" "$product_sha"
git -C "$product" checkout --detach FETCH_HEAD
test "$(git -C "$product" rev-parse HEAD)" = "$product_sha"
(cd "$product" && python3 scripts/verify_deps.py --checkout)
test "$(git -C "$product/deps/mundo-X" rev-parse HEAD)" = "$core_sha"
