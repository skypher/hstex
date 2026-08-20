#!/bin/sh
# Knuth's trip test, run as a black-box comparison.
#
# trip.tex is a test input, not engine source: it is fetched rather than
# vendored, pinned by digest, and the reference engine is run only to
# produce the log this one is compared against. See CLEANROOM.md.
#
# The trip test is above all an error-recovery test -- most of what it does
# is wrong on purpose, and the reference reports each fault and carries on.
# What this script reports is how far HSTeX gets and whether the faults it
# reports are the reference's, in the reference's words.
#
# Usage: tests/trip/run-trip.sh [path-to-hstex]

set -e

engine=${1:-./build/hstex}
case $engine in
/*) ;;
*) engine=$(pwd)/$engine ;;
esac

work=${TRIP_WORK:-build/trip}
mkdir -p "$work"
cd "$work"

TRIP_TEX_SHA=15f15c2ca1470085299056ec89dea5f51e9fe9303ef25581b2f2eaf7809ae97b
TRIP_PL_SHA=93b38cc794f0c4a462667e25ef34a83552cbcdd62a42b10f739a431166525a79
base=https://mirrors.ctan.org/systems/knuth/dist/tex

fetch() {
    name=$1
    want=$2
    if [ -f "$name" ] && printf '%s  %s\n' "$want" "$name" | sha256sum -c - >/dev/null 2>&1; then
        return 0
    fi
    curl -sSLf -o "$name" "$base/$name"
    printf '%s  %s\n' "$want" "$name" | sha256sum -c - >/dev/null
}

fetch trip.tex "$TRIP_TEX_SHA"
fetch trip.pl "$TRIP_PL_SHA"

# The font the test defines for itself.
pltotf trip.pl trip.tfm

# The oracle: pdfTeX's own first pass over the same file. Its \dump makes a
# format, which is not what is being compared -- the log is.
rm -f trip.fmt oracle.log
pdftex -ini </dev/null trip >/dev/null 2>&1 || true
mv trip.log oracle.log

# HSTeX over the same file.
mkdir -p build
rm -f hstex.log hstex.err
set +e
"$engine" --run-ini trip.tex >hstex.log 2>hstex.err
status=$?
set -e

# The faults each engine reported, in order.
grep -E '^(! |> )' oracle.log >oracle.faults || true
grep -E '^(! |> )' hstex.log >hstex.faults || true

oracle_total=$(wc -l <oracle.faults)
hstex_total=$(wc -l <hstex.faults)

# How far HSTeX got: where it stopped, or the last line it named.
stop_line=$(sed -n 's/.*trip\.tex:\([0-9]*\):.*/\1/p' hstex.err | tail -1)
if [ -z "$stop_line" ]; then
    stop_line=$(grep -oE '^l\.[0-9]+' hstex.log | tail -1 | cut -c3-)
fi

echo "trip.tex: $(wc -l <trip.tex) lines"
echo "hstex reached line ${stop_line:-0}, reported $hstex_total diagnostics"
echo "oracle reported $oracle_total diagnostics"
if [ "$status" -ne 0 ]; then
    echo "hstex stopped: $(cat hstex.err)"
else
    echo "hstex ran to the end of the file"
fi
echo
echo "--- diagnostics HSTeX reports, against the oracle's ---"
diff oracle.faults hstex.faults || true
