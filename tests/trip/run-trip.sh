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
# Both passes are run: the first reads trip.tex from the top and ends at the
# \dump on line 92, the second loads what that dumped and reads trip.tex
# again -- which skips the first ninety lines and runs the other three
# hundred and fifty. EACH ENGINE RUNS IN A DIRECTORY OF ITS OWN, because
# trip writes files it later reads back (8terminal.tex on line 94, tripos.tex
# on line 94 and read on 211 and 424) and one engine must never read the
# other's.
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
work=$(pwd)

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

# A clean room per engine, holding only what trip is given to start with.
for room in oracle hstex; do
    rm -rf "$work/$room"
    mkdir -p "$work/$room/build"
    cp trip.tex trip.tfm "$work/$room/"
done

# The oracle: pdfTeX's own two passes. The \dump makes a format, which is
# not what is being compared -- the log is.
cd "$work/oracle"
pdftex -ini </dev/null trip >/dev/null 2>&1 || true
mv -f trip.log pass1.log
TEXMFOUTPUT=. timeout 300 pdftex </dev/null '&trip' trip >/dev/null 2>&1 || true
mv -f trip.log pass2.log

# HSTeX over the same file, from its own copy. The first pass is run twice:
# once for the log, and once to write the format the second pass loads --
# making a format is not reading a file, and its log says other things.
cd "$work/hstex"
set +e
"$engine" --run-ini trip.tex >pass1.log 2>pass1.err
status1=$?
"$engine" --make-format trip.tex trip.hfmt >format.log 2>format.err
timeout 300 "$engine" --format trip.hfmt trip.tex >pass2.log 2>pass2.err
status2=$?
set -e

cd "$work"
report() {
    pass=$1
    grep -E '^(! |> )' "oracle/$pass.log" >"oracle/$pass.faults" || true
    grep -E '^(! |> )' "hstex/$pass.log" >"hstex/$pass.faults" || true
    echo "--- $pass"
    echo "oracle $(wc -l <"oracle/$pass.log") lines, $(wc -l <"oracle/$pass.faults") diagnostics"
    echo "hstex  $(wc -l <"hstex/$pass.log") lines, $(wc -l <"hstex/$pass.faults") diagnostics"
    if [ -s "hstex/$pass.err" ]; then
        echo "hstex stopped: $(cat "hstex/$pass.err")"
    fi
    diff "oracle/$pass.faults" "hstex/$pass.faults" || true
}

echo "trip.tex: $(wc -l <trip.tex) lines"
[ "$status1" -eq 0 ] || echo "hstex pass one exited $status1"
[ "$status2" -eq 0 ] || echo "hstex pass two exited $status2"
report pass1
report pass2
