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
# A passing second pass also has the same complete DVI payload. The DVI
# preamble's producer timestamp is deliberately excluded: it records the
# instant each engine was run, not the document semantics.
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

usage() {
    printf '%s\n' 'Usage: tests/trip/run-trip.sh [path-to-hstex]'
    printf '%s\n' 'Fetch the digest-pinned Trip inputs and compare HSTeX with pdfTeX.'
}

case ${1:-} in
-h|--help)
    usage
    exit 0
    ;;
esac
if [ "$#" -gt 1 ]; then
    usage >&2
    exit 2
fi

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
# Availability decides only where the bytes are fetched: every candidate is
# a CTAN endpoint, and a transfer is admitted only by the pinned digest.
trip_bases='https://mirrors.mit.edu/CTAN/systems/knuth/dist/tex
https://ctan.math.washington.edu/tex-archive/systems/knuth/dist/tex
https://mirrors.ctan.org/systems/knuth/dist/tex
https://ctan.math.illinois.edu/systems/knuth/dist/tex
https://mirrors.ibiblio.org/CTAN/systems/knuth/dist/tex'

fetch() {
    name=$1
    want=$2
    if [ -f "$name" ] && printf '%s  %s\n' "$want" "$name" | sha256sum -c - >/dev/null 2>&1; then
        return 0
    fi
    temporary=$name.fetch.$$
    for base in $trip_bases; do
        if curl -sSLf --retry 3 --retry-delay 2 --retry-connrefused \
            --connect-timeout 20 -o "$temporary" "$base/$name" &&
            printf '%s  %s\n' "$want" "$temporary" |
                sha256sum -c - >/dev/null; then
            mv -f "$temporary" "$name"
            return 0
        fi
    done
    rm -f "$temporary"
    return 1
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
# Built by a TeX82 engine, as the reference builds it: trip line 29 says
# `\toksdef\tokens=256', which the reference has not got and an
# eTeX-enabled engine has. Building the format the other way made the
# second pass disagree with the first over what \tokens means.
"$engine" --make-ini-format trip.tex trip.hfmt >format.log 2>format.err
timeout 300 "$engine" --format trip.hfmt trip.tex >pass2.log 2>pass2.err
status2=$?
set -e

cd "$work"
failed=0
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
    if ! diff -u "oracle/$pass.faults" "hstex/$pass.faults"; then
        failed=1
    fi
}

compare_dvi() {
    oracle_dvi=oracle/trip.dvi
    hstex_dvi=hstex/build/document-output/trip.dvi
    if [ ! -f "$oracle_dvi" ] || [ ! -f "$hstex_dvi" ]; then
        echo "missing final-pass DVI output"
        failed=1
        return
    fi
    oracle_bytes=$(wc -c <"$oracle_dvi")
    hstex_bytes=$(wc -c <"$hstex_dvi")
    if [ "$oracle_bytes" -ne "$hstex_bytes" ]; then
        echo "DVI length differs: oracle $oracle_bytes, hstex $hstex_bytes"
        failed=1
        return
    fi
    differences=$(cmp -l "$oracle_dvi" "$hstex_dvi" || true)
    if [ -z "$differences" ]; then
        echo "final-pass DVI: exact match"
        return
    fi
    # DVI bytes are one-indexed here. Bytes 28..42 are the 15 digits and
    # separators in the conventional ` TeX output YYYY.MM.DD:HHMM` comment.
    if printf '%s\n' "$differences" | awk '$1 < 28 || $1 > 42 { exit 1 }'; then
        echo "final-pass DVI: payload match (preamble timestamp differs)"
        return
    fi
    echo "final-pass DVI differs outside the preamble timestamp:"
    printf '%s\n' "$differences"
    failed=1
}

echo "trip.tex: $(wc -l <trip.tex) lines"
if [ "$status1" -ne 0 ]; then
    echo "hstex pass one exited $status1"
    failed=1
fi
if [ "$status2" -ne 0 ]; then
    echo "hstex pass two exited $status2"
    failed=1
fi
report pass1
report pass2
compare_dvi
if [ "$failed" -ne 0 ]; then
    exit 1
fi
echo "TRIP PASS: both fault transcripts and the final-pass DVI payload match."
