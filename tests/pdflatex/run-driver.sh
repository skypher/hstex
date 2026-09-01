#!/bin/sh
# Exercise the installed-facing driver, including its native format cache and
# conventional output-directory/job-name behavior.

set -eu

usage() {
    printf '%s\n' 'Usage: tests/pdflatex/run-driver.sh DRIVER ENGINE'
    printf '%s\n' 'Exercise the pdflatex driver cache and output naming.'
}

case ${1:-} in
-h|--help)
    usage
    exit 0
    ;;
esac

if [ "${HSTEX_TEST_PDFLATEX_DRIVER:-0}" != 1 ]; then
    exit 77
fi
if [ "$#" -ne 2 ]; then
    usage >&2
    exit 2
fi

driver=$1
engine=$2
source=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/smoke.tex
work=$(mktemp -d "${TMPDIR:-/tmp}/hstex-pdflatex-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

run_driver() {
    report=$1
    shift
    if ! HSTEX_ENGINE=$engine HSTEX_CACHE_DIR=$work/cache \
        "$driver" "$@" >"$report" 2>&1; then
        cat "$report" >&2
        return 1
    fi
}

run_driver "$work/first.stdout" -output-directory="$work/first" \
    -jobname=first "$source"

test -s "$work/first/first.pdf"
test -s "$work/first/first.log"
test -s "$work/cache/formats"/*/pdflatex.hfmt

run_driver "$work/second.stdout" -output-directory="$work/second" \
    -jobname=second "$source"

test -s "$work/second/second.pdf"
test -s "$work/second/second.log"
if grep -q 'building native format' "$work/second.stdout"; then
    echo "driver rebuilt an unchanged native format" >&2
    exit 1
fi
