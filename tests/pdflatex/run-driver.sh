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

# Two caches beside the format cache. The installation record keeps what
# kpsewhich answered about where latex.ltx and pdftexconfig.tex are and which
# trees keep an ls-R, under a stamp over those lists; it exists after a first
# run and holds four lines. The preamble cache keeps a document's state from
# just before its .aux is first read: a first run cannot take it (there is no
# .aux to read yet), a second writes it, a third takes it up and must produce
# the same document as a run that read the class afresh.
test -s "$work/cache/installation"
if [ "$(wc -l <"$work/cache/installation")" -ne 4 ]; then
    echo "the installation record does not hold four lines" >&2
    cat "$work/cache/installation" >&2
    exit 1
fi
# The documents are compared byte for byte below, so the clock is pinned:
# a PDF carries the time it was made.
SOURCE_DATE_EPOCH=1767261600
FORCE_SOURCE_DATE=1
export SOURCE_DATE_EPOCH FORCE_SOURCE_DATE
# The sequential path is the one that reads the preamble from the cache on
# every run; the default path's warm runs resume chunk checkpoints instead
# and only its cold passes take the preamble up.
# Opted in: the preamble cache is off by default while it is measured
# unsound on documents with a table of contents; the plumbing is held here on
# a document without one.
for pass in 1 2 3; do
    HSTEX_NO_PARALLEL=1 HSTEX_PREAMBLE_CACHE=1 run_driver \
        "$work/pre$pass.stdout" -output-directory="$work/pre" -jobname=pre \
        "$source"
done
if ! grep -q 'preamble taken up' "$work/pre/pre.log"; then
    echo "the third run did not take up the preamble put by" >&2
    grep -n 'preamble' "$work/pre/pre.log" >&2
    exit 1
fi
if ! ls "$work/cache/preambles"/*/preamble.ckpt >/dev/null 2>&1; then
    echo "no preamble checkpoint was put by" >&2
    exit 1
fi
# The control is a fourth run of the same job, into the same directory,
# reading the same settled .aux, told to read the class afresh. The trailer
# ID is seeded from the output's name, so the directory has to be the same
# one; what is left to differ is where the preamble came from, and the
# documents must be the same bytes.
cp "$work/pre/pre.pdf" "$work/pre-cached.pdf"
HSTEX_ENGINE=$engine HSTEX_CACHE_DIR=$work/cache \
    HSTEX_NO_PARALLEL=1 \
    "$driver" -output-directory="$work/pre" -jobname=pre "$source" \
    >"$work/fresh.stdout" 2>&1
if ! cmp -s "$work/pre-cached.pdf" "$work/pre/pre.pdf"; then
    echo "a run from the preamble cache differs from a fresh one" >&2
    exit 1
fi
