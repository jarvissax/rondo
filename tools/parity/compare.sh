#!/usr/bin/env bash
# Compares native screenshots with their 1.0.x references: private/parity/2.0/<name>.png against
# private/parity/1.0/<name>.png, writing private/parity/diff/<name>.png.
#
#   tools/parity/compare.sh [name...]      default: every native screenshot that has a reference
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=$ROOT/tools/parity/.compare
if [[ ! -x $BIN || $ROOT/tools/parity/compare.cpp -nt $BIN ]]; then
  clang++ -std=c++17 -O2 -I/opt/homebrew/include -L/opt/homebrew/lib -lpng -o "$BIN" "$ROOT/tools/parity/compare.cpp"
fi
P=$ROOT/private/parity
mkdir -p "$P/diff"
names=("$@")
if [[ ${#names[@]} -eq 0 ]]; then
  for f in "$P"/2.0/*.png; do [[ -f $f ]] && names+=("$(basename "$f" .png)"); done
fi
for n in "${names[@]}"; do
  [[ -f $P/1.0/$n.png ]] || { echo "$n: no reference"; continue; }
  "$BIN" "$P/1.0/$n.png" "$P/2.0/$n.png" "$P/diff/$n.png" | sed "s|$P/2.0/||" || echo "$n: could not be read (shoot it again)"
done
