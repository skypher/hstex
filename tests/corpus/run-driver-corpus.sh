#!/bin/sh
# The corpus, run the way an installation runs it.
#
# tests/corpus/run-corpus.sh drives the engine directly, one pass, through
# `hstex --format'. That is not the path an installed HSTeX takes: the
# supported command is `hstex-pdflatex', which builds a format cache and then
# compiles through the checkpoint path unless HSTEX_NO_PARALLEL says
# otherwise. Nothing gated that path, so a document could come out wrong
# through the command a reader actually types while every corpus run stayed
# green.
#
# This runs each document the way that command does -- twice, so that the
# cross-references and the checkpoint cache are both warm, which is the state
# the second and every later run of an edit loop is in -- and compares what
# comes out against the same reference given the same two passes.
#
# Usage: tests/corpus/run-driver-corpus.sh [--strict] [DRIVER] [ENGINE]
#
# What each document is expected to do is pinned in driver-expectations.tsv.
# A document that stops matching its pin fails under --strict, whichever
# direction it moved: a new disagreement is a regression, and a disagreement
# that has gone is a fix whose pin needs updating.
set -e

usage() {
    cat <<'EOF'
Usage: tests/corpus/run-driver-corpus.sh [OPTIONS] [DRIVER] [ENGINE]

Run the public corpus through hstex-pdflatex, twice per document, and compare
what it produces with the reference given the same two passes.

Options:
  --strict      exit nonzero when a document stops matching its pinned result
  -h, --help    show this help and exit

DRIVER defaults to ./build/hstex-pdflatex and ENGINE to ./build/hstex.
CORPUS_WORK overrides the work directory (default build/corpus-driver).
EOF
}

strict=0
while :; do
    case ${1:-} in
    --strict) strict=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*) printf 'unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    *) break ;;
    esac
done

driver=${1:-./build/hstex-pdflatex}
engine=${2:-./build/hstex}
case $driver in /*) ;; *) driver=$(pwd)/$driver ;; esac
case $engine in /*) ;; *) engine=$(pwd)/$engine ;; esac

here=$(cd "$(dirname "$0")" && pwd)
manifest=$here/documents.tsv
expectations=$here/driver-expectations.tsv
work=${CORPUS_WORK:-build/corpus-driver}
mkdir -p "$work"
work=$(cd "$work" && pwd)

for tool in python3 mutool pdfinfo pdftotext pdflatex; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'required tool is missing: %s\n' "$tool" >&2
        exit 1
    fi
done

# The documents themselves come from the runner that owns them, so that this
# is never a second opinion about what the corpus is.
CORPUS_WORK=$work "$here/run-corpus.sh" --fetch-only >/dev/null

# A document that prints the date must be given one, or two runs differ by
# the time of day and nothing else.
SOURCE_DATE_EPOCH=1767261600
FORCE_SOURCE_DATE=1
HSTEX_ENGINE=$engine
export SOURCE_DATE_EPOCH FORCE_SOURCE_DATE HSTEX_ENGINE

cache=$work/format-cache
mkdir -p "$cache"

expected_of() {
    awk -F'\t' -v want="$1" '$1 == want { print $2; found = 1 }
                             END { if (!found) print "agrees" }' "$expectations"
}

printf '%-20s %-9s %-9s %s\n' document expected result verdict
printf '%-20s %-9s %-9s %s\n' -------------------- --------- --------- -------
unexpected=0
while IFS='	' read -r name format path want note input_profile; do
    [ -n "$name" ] || continue
    # The driver is the LaTeX command; a plain document is not its work, and
    # a document that reads the terminal is not what an edit loop repeats.
    [ "$format" = latex ] || continue
    [ -z "$input_profile" ] || continue

    suffix=${path##*.}
    input_file=$name.$suffix
    dir=$work/$name
    rm -rf "$dir"
    mkdir -p "$dir/ref" "$dir/hstex"
    cp "$work/src/$input_file" "$dir/ref/"
    cp "$work/src/$input_file" "$dir/hstex/"

    ( cd "$dir/ref"
      pdflatex -interaction=nonstopmode "$input_file" >/dev/null 2>&1 || :
      pdflatex -interaction=nonstopmode "$input_file" >/dev/null 2>&1 || : )
    ( cd "$dir/hstex"
      "$driver" --format-cache="$cache" "$input_file" >pass1.log 2>&1 || :
      "$driver" --format-cache="$cache" "$input_file" >pass2.log 2>&1 || : )

    ref_pdf=$dir/ref/$name.pdf
    hs_pdf=$dir/hstex/$name.pdf
    [ -f "$hs_pdf" ] || hs_pdf=$dir/hstex/build/document-output/$name.pdf

    if [ ! -f "$ref_pdf" ] || [ ! -f "$hs_pdf" ]; then
        result=missing
    elif python3 -u "$here/compare-pdf.py" "$ref_pdf" "$hs_pdf" \
            >"$dir/compare.txt" 2>&1; then
        result=agrees
    else
        result=differs
    fi

    expected=$(expected_of "$name")
    if [ "$result" = "$expected" ]; then
        verdict=as-pinned
    else
        verdict="CHANGED (pinned $expected)"
        unexpected=$((unexpected + 1))
    fi
    printf '%-20s %-9s %-9s %s\n' "$name" "$expected" "$result" "$verdict"
done <"$work/manifest.clean"

echo
if [ "$unexpected" -eq 0 ]; then
    echo "every document matches its pinned result"
else
    printf '%s document(s) no longer match driver-expectations.tsv\n' \
        "$unexpected"
fi
if [ "$strict" -eq 1 ] && [ "$unexpected" -ne 0 ]; then
    exit 1
fi
