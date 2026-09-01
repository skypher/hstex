#!/bin/bash
# The edit loop in two commands.
#
#   HSTEX_FLEET=<dir> hstex --format <format> <document>
#
# A run finding no fleet in <dir> runs whole and leaves one parked; a run
# finding one is served from it -- it verifies the parked chunks against its
# own state at every parked page, lets the valid ones stand, rewrites the
# rest, and leaves the fleet parked for the next run. Chunks read the disk
# as it stands when THEY are released, so edits between runs are seen; the
# aux delta between runs is patched into the chunks automatically from the
# snapshot the parking run took. The default state digest is strict.
#
# This script demonstrates the loop: it compiles DOCUMENT twice with an
# edit-hook of your choosing in between, and reports both times.
#
#   tools/relay-demo.sh <format.hfmt> <document.tex> <workdir> [edit-command]
set -euo pipefail

usage() {
  printf '%s\n' \
    'Usage: tools/relay-demo.sh FORMAT.hfmt DOCUMENT.tex WORKDIR [EDIT-COMMAND]' \
    'Compile twice through the persistent fleet, applying EDIT-COMMAND between runs.'
}

case "${1:-}" in
  -h|--help)
    usage
    exit 0
    ;;
esac

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
  usage >&2
  exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
format="$1"; document="$2"; work="$3"; edit="${4:-true}"
mkdir -p "$work/build/document-output"

compile() {
  ( cd "$work" && HSTEX_FLEET="$work/fleet" \
      /usr/bin/time -f "wall=%e" "$root/build/hstex" \
      --format "$format" "$document" >/dev/null 2>"$work/last.log" )
  grep -o "wall=.*" "$work/last.log" | tail -1
}

echo "first compile (parks the fleet):"
compile
echo "applying the edit: $edit"
eval "$edit"
echo "second compile (served from the fleet):"
compile
echo "fleet still parked: $(pgrep -f "$root/build/hstex" | wc -l) processes"
