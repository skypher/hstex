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

# -halt-on-error stops at the first error, writes nothing, and says so. The
# flag was accepted and discarded once, so a document with three undefined
# control sequences came out as a finished PDF and a successful exit: what is
# checked here is the reference's own answer to the same document -- one
# fault reported, no PDF, and a status that says the run failed.
cat >"$work/broken.tex" <<'BROKEN'
\documentclass{article}
\begin{document}
Before.
\undefinedcommandone
\undefinedcommandtwo
\newpage
After.
\end{document}
BROKEN

if HSTEX_ENGINE=$engine HSTEX_CACHE_DIR=$work/cache \
    "$driver" -halt-on-error -output-directory="$work/halt" \
    -jobname=halt "$work/broken.tex" >"$work/halt.stdout" 2>&1; then
    echo "-halt-on-error reported success on a document with errors" >&2
    exit 1
fi
if [ -e "$work/halt/halt.pdf" ]; then
    echo "-halt-on-error left an output file behind" >&2
    exit 1
fi
if ! grep -q '^!  ==> Fatal error occurred, no output PDF file produced!' \
        "$work/halt/halt.log"; then
    echo "-halt-on-error did not say what the run came to" >&2
    cat "$work/halt/halt.log" >&2
    exit 1
fi
# The second undefined control sequence is past the stop and must not be
# reported: a run that halted read no further.
if grep -q 'undefinedcommandtwo' "$work/halt/halt.log"; then
    echo "-halt-on-error carried on past the first error" >&2
    exit 1
fi

# Every option the driver accepts must do something. -interaction picks a
# mode the engine has, -file-line-error changes how an error opens, and a
# mode the engine does not have is refused rather than quietly replaced.
run_driver "$work/nonstop.stdout" -interaction=nonstopmode \
    -output-directory="$work/nonstop" -jobname=nonstop "$source"
test -s "$work/nonstop/nonstop.pdf"

if HSTEX_ENGINE=$engine HSTEX_CACHE_DIR=$work/cache \
    "$driver" -interaction=nosuchmode -output-directory="$work/bogus" \
    -jobname=bogus "$source" >"$work/bogus.stdout" 2>&1; then
    echo "an unknown interaction mode was accepted" >&2
    exit 1
fi
grep -q 'unknown interaction mode' "$work/bogus.stdout" || {
    echo "an unknown interaction mode was refused without saying why" >&2
    cat "$work/bogus.stdout" >&2
    exit 1
}

# -file-line-error opens an error with the file and line it was met in, the
# way the reference does; without it the same error opens with "! ".
if HSTEX_ENGINE=$engine HSTEX_CACHE_DIR=$work/cache \
    "$driver" -file-line-error -interaction=nonstopmode \
    -output-directory="$work/fileline" -jobname=fileline \
    "$work/broken.tex" >"$work/fileline.stdout" 2>&1; then :; fi
# The document is given by absolute path here, and the reference names a file
# as it opened it, so that is what must appear -- not a "./" the run never saw.
if ! grep -q "^$work/broken\.tex:4: Undefined control sequence\." \
        "$work/fileline/fileline.log"; then
    echo "-file-line-error did not name the file and line" >&2
    grep -n 'Undefined control sequence' "$work/fileline/fileline.log" >&2
    exit 1
fi
if grep -q '^! Undefined control sequence' "$work/fileline/fileline.log"; then
    echo "-file-line-error left an error opening with \"! \"" >&2
    exit 1
fi
