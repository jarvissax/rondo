#!/usr/bin/env bash
# Compiles native/ files for the Y1 without building anything (-fsyntax-only), with the flags the build
# uses (native/build/y1/compile_commands.json, written by tools/native/build.sh): a quick check that
# several people (or sessions) can run at once, since nothing is written to the build folder.
#   tools/native/check-file.sh native/shell/settings_screens.cpp [more.cpp …]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DB=$ROOT/native/build/y1/compile_commands.json
[[ -f $DB ]] || { echo "No $DB: run tools/native/build.sh once" >&2; exit 1; }
status=0
for f in "$@"; do
  abs=$(cd "$(dirname "$f")" && pwd)/$(basename "$f")
  cmd=$(node -e '
    const [db, file] = process.argv.slice(1);
    const e = require(db).find((x) => x.file === file);
    if (!e) process.exit(2);
    // the same compiler and flags, checking only: no object file, no dependency file
    let c = e.command.replace(/ -o \S+/, " -o /dev/null").replace(/ -MD -MT \S+ -MF \S+/, "").replace(/ -c /, " -fsyntax-only -c ");
    process.stdout.write("cd " + JSON.stringify(e.directory) + " && " + c);
  ' "$DB" "$abs") || { echo "$f: not in the build (a header? check a .cpp that includes it)" >&2; status=1; continue; }
  bash -c "$cmd" || status=1
done
exit $status
