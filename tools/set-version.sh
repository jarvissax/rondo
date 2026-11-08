#!/usr/bin/env bash
# Sets Rondo's version everywhere it's stated: the app (versionName, and versionCode as
# major × 10000 + minor × 100 + patch), the server and the update page (their package.json and
# package-lock.json), and native/core/version.h. The whole project shares one version, and a release is tagged v<version>;
# .github/workflows/release.yml refuses to build one whose files disagree with its tag.
#
#   tools/set-version.sh 1.0.2            set it
#   tools/set-version.sh --check [1.0.2]  are they all the same (and this version)?
#
# The firmware number (firmwareLevel in app/app/build.gradle.kts) is separate: raise it by hand
# when a release changes the firmware itself.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GRADLE="$ROOT/app/app/build.gradle.kts"
NATIVE="$ROOT/native/core/version.h"  # Rondo 2.0's own (About, the user agent)
PACKAGES=(server web-updater)

check=0
[[ "${1:-}" == "--check" ]] && { check=1; shift; }
want="${1:-}"

if [[ $check == 1 ]]; then
  found=("app $(sed -n 's/.*versionName = "\(.*\)".*/\1/p' "$GRADLE")")
  found+=("native $(sed -n 's/.*kRondoVersion = "\(.*\)".*/\1/p' "$NATIVE")")
  for p in "${PACKAGES[@]}"; do
    found+=("$p $(node -p "require('$ROOT/$p/package.json').version")")
    found+=("$p-lock $(node -p "require('$ROOT/$p/package-lock.json').version")")
  done
  printf '%s\n' "${found[@]}"
  first="${found[0]#* }"
  for f in "${found[@]}"; do
    v="${f#* }"
    [[ "$v" == "$first" ]] || { echo "Versions disagree" >&2; exit 1; }
    [[ -z "$want" || "$v" == "$want" ]] || { echo "Not $want" >&2; exit 1; }
  done
  exit 0
fi

[[ "$want" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || { echo "Usage: tools/set-version.sh <major.minor.patch>" >&2; exit 1; }
major="${BASH_REMATCH[1]}" minor="${BASH_REMATCH[2]}" patch="${BASH_REMATCH[3]}"
(( minor < 100 && patch < 100 )) || { echo "Minor and patch must stay under 100 (versionCode)" >&2; exit 1; }
code=$(( major * 10000 + minor * 100 + patch ))

sed -i.bak -e "s/versionName = \".*\"/versionName = \"$want\"/" -e "s/versionCode = [0-9]*/versionCode = $code/" "$GRADLE"
rm -f "$GRADLE.bak"
sed -i.bak -e "s/kRondoVersion = \".*\"/kRondoVersion = \"$want\"/" "$NATIVE"
rm -f "$NATIVE.bak"
for p in "${PACKAGES[@]}"; do
  node - "$ROOT/$p" "$want" <<'EOF'
const fs = require("fs");
const [dir, version] = process.argv.slice(2);
for (const name of ["package.json", "package-lock.json"]) {
  const file = `${dir}/${name}`;
  const json = JSON.parse(fs.readFileSync(file, "utf8"));
  json.version = version;
  if (json.packages?.[""]) json.packages[""].version = version;
  fs.writeFileSync(file, JSON.stringify(json, null, 2) + "\n");
}
EOF
done
echo "Rondo $want (versionCode $code)"
"$0" --check "$want"
