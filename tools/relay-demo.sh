#!/bin/bash
# Reproduce the relay: a pass produced from the previous pass's parked fleet.
#
#   tools/relay-demo.sh <format.hfmt> <document.tex> <workdir>
#
# The work directory must hold the document's auxiliary inputs as the pass
# BEFORE the parking pass would leave them (aux/toc/out and any .bbl), plus
# a copy of the aux the parking pass will write, named aux.next -- which a
# deterministic engine makes available by running the pass once beforehand.
# The demo then runs the parking pass, wakes its fleet against the patch,
# runs the verifier, and reports whether the verifier's outputs are byte for
# byte a plain run's.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
format="$1"; document="$2"; work="$3"
job="$(basename "${document%.tex}")"
out="$work/build/document-output"
relay="$work/relay"
mkdir -p "$out" "$relay"
rm -f "$relay"/* "$out"/*-fleet*

python3 "$root/tools/gen-aux-patch.py" "$out/$job.aux" "$work/aux.next" \
    > "$work/patch.tex"

echo "parking pass (fleet + patch at its end)"
( cd "$work" && HSTEX_PARALLEL=100 HSTEX_PARALLEL_ROUNDS=1 \
    HSTEX_PATCH="$work/patch.tex" HSTEX_SPECULATE="$relay" \
    HSTEX_DIGEST_SOFT="$root/tools/soft-names.txt" \
    "$root/build/hstex" --format "$format" "$document" >/dev/null 2>parking.log )

echo "verifier"
( cd "$work" && HSTEX_VERIFY="$relay" \
    HSTEX_DIGEST_SOFT="$root/tools/soft-names.txt" \
    /usr/bin/time -f "verifier wall=%e" "$root/build/hstex" \
    --format "$format" "$document" >/dev/null 2>verifier.log )
tail -1 "$work/verifier.log"
echo "valid: $(ls "$relay" | grep -c '^valid')  stale: $(ls "$relay" | grep -c '^stale')"
