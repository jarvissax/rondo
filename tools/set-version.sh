#!/usr/bin/env bash
# Sets a component's version everywhere it's stated. Rondo has two, released apart (since device
# 3.0.2 and server 3.1.0):
#   device  Rondo on the Y1: native/core/version.h (the firmware, the update bundle and the dashboard
#           it serves) and the update page (web-updater/package.json, package-lock.json). Tags v<version>.
#   server  the Rondo server and its updater: server/package.json, package-lock.json. Tags
#           server-v<version>.
# A component's files must agree, and a release's tag with them: .github/workflows/release.yml (v*)
# and server-release.yml (server-v*) refuse one that doesn't.
#
#   tools/set-version.sh device 3.0.2            set it
#   tools/set-version.sh server 3.1.0
#   tools/set-version.sh --check device [3.0.2]  do the component's files agree (and say this version)?
#   tools/set-version.sh --check                 both components, each on its own
#
# Two numbers in native/core/version.h are raised by hand: kFirmwareNeeded, when a Rondo release needs
# a newer firmware, and kServerVersionNeeded, the oldest server a Rondo works with, when it needs
# something only a newer server release has (released first).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NATIVE="$ROOT/native/core/version.h"

usage() { sed -n '11,14p' "$0" | sed 's/^#   //' >&2; }

# "<file> <version>" for each of a component's files
found() {
  case "$1" in
    device)
      echo "native/core/version.h $(sed -n 's/.*kRondoVersion = "\(.*\)".*/\1/p' "$NATIVE")"
      for x in package.json package-lock.json; do echo "web-updater/$x $(node -p "require('$ROOT/web-updater/$x').version")"; done
      ;;
    server)
      for x in package.json package-lock.json; do echo "server/$x $(node -p "require('$ROOT/server/$x').version")"; done
      ;;
    *)
      echo "Unknown component '$1': device or server" >&2
      return 2
      ;;
  esac
}

check() {
  local lines first name v
  lines="$(found "$1")"
  echo "$1:"
  sed 's/^/  /' <<< "$lines"
  first="$(head -1 <<< "$lines" | cut -d' ' -f2)"
  while read -r name v; do
    [[ "$v" == "$first" ]] || { echo "$1's versions disagree" >&2; return 1; }
    [[ -z "${2:-}" || "$v" == "$2" ]] || { echo "$1 is $v, not $2" >&2; return 1; }
  done <<< "$lines"
}

if [[ "${1:-}" == "--check" ]]; then
  shift
  if [[ $# -eq 0 ]]; then
    check device && check server
  else
    check "$1" "${2:-}"
  fi
  exit
fi

component="${1:-}" want="${2:-}"
if [[ "$component" != device && "$component" != server ]] || ! [[ "$want" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  usage
  exit 1
fi

# a package's package.json and package-lock.json
setPackage() {
  node - "$ROOT/$1" "$want" <<'JS'
const fs = require("fs");
const [dir, version] = process.argv.slice(2);
for (const name of ["package.json", "package-lock.json"]) {
  const file = `${dir}/${name}`;
  const json = JSON.parse(fs.readFileSync(file, "utf8"));
  json.version = version;
  if (json.packages?.[""]) json.packages[""].version = version;
  fs.writeFileSync(file, JSON.stringify(json, null, 2) + "\n");
}
JS
}

case "$component" in
  device)
    sed -i.bak -e "s/kRondoVersion = \".*\"/kRondoVersion = \"$want\"/" "$NATIVE"
    rm -f "$NATIVE.bak"
    setPackage web-updater
    ;;
  server)
    setPackage server
    ;;
esac
echo "Rondo $component $want"
"$0" --check "$component" "$want"
