#!/bin/sh
# The public document corpus, run as a black-box comparison.
#
# Each document is fetched from CTAN, pinned by digest, and typeset twice:
# once by the reference engine and once by HSTeX.  Nothing of the reference
# but its behaviour is used -- it is run to produce the log this engine's is
# compared against.  See CLEANROOM.md.
#
# What is compared is what the reference says about the document: how many
# pages it made, which boxes did not fit, and which faults it reported.  The
# reference's own storage statistics are not comparable and are not compared;
# see docs/DECISIONS.md, what-a-clean-room-engine-cannot-reproduce.
#
# Usage: tests/corpus/run-corpus.sh [--strict] [--fetch-only] [--time]
#                                   [path-to-hstex]
#        CORPUS_WORK=dir  tests/corpus/run-corpus.sh   (default build/corpus)
#
# --strict exits nonzero if any document disagrees.  Without it the script
# reports the state of the corpus and exits 0.
#
# --fetch-only fetches every document and checks it against its pinned digest,
# without running either engine.  This is the corpus identity check, and needs
# no TeX installation.
#
# --time additionally reports what each engine takes over each document: the
# median of seven warm runs, each in an output directory of its own, as
# docs/BENCHMARK_CONTRACT.md asks.  What it reports is THE DOCUMENT PASS, with
# both engines starting from a format that is already built; the cost of
# building HSTeX's is reported on its own line, because the reference's was
# built once by its distribution and is not part of typesetting a document.

set -e

strict=0
fetch_only=0
time_runs=0
while :; do
    case ${1:-} in
    --strict) strict=1; shift ;;
    --fetch-only) fetch_only=1; shift ;;
    --time) time_runs=1; shift ;;
    *) break ;;
    esac
done

engine=${1:-./build/hstex}
case $engine in
/*) ;;
*) engine=$(pwd)/$engine ;;
esac

here=$(cd "$(dirname "$0")" && pwd)
manifest=$here/documents.tsv
root=$(pwd)

work=${CORPUS_WORK:-build/corpus}
mkdir -p "$work"
work=$(cd "$work" && pwd)
src=$work/src
mkdir -p "$src"

base=https://mirrors.ctan.org
# A document that prints the date must be given one, or the two runs differ by
# the time of day and nothing else.
clock='\time=600 \day=1 \month=1 \year=2026'
latex_ltx=
[ "$fetch_only" -eq 1 ] || latex_ltx=$(kpsewhich latex.ltx)

# Note: this shell has no local variables, so these names are deliberately
# distinct from the ones the document loop uses.
fetch() {
    fetch_path=$1
    fetch_file=$2
    fetch_want=$3
    if [ -f "$src/$fetch_file" ] &&
       printf '%s  %s\n' "$fetch_want" "$src/$fetch_file" |
           sha256sum -c - >/dev/null 2>&1; then
        return 0
    fi
    # Fetched from a public host, which is sometimes briefly unreachable:
    # a single refusal or a hung connect is not a corpus that has changed.
    # What the file is remains settled by the digest checked below.
    curl -sSLf --retry 3 --retry-delay 2 --retry-connrefused \
        --connect-timeout 20 -o "$src/$fetch_file" "$base/$fetch_path"
    printf '%s  %s\n' "$fetch_want" "$src/$fetch_file" | sha256sum -c - >/dev/null
}

# What the log says about the document.
pages_of() {
    sed -n 's/.*Output written on [^(]*(\([0-9][0-9]*\) page.*/\1/p' "$1" | tail -1
}
boxes_of() {
    # Box reports wrap across lines in both engines; join the report to its
    # location before comparing.
    tr '\n' ' ' <"$1" |
        grep -oE '(Overfull|Underfull) \\[hv]box \([^)]*\)[^\\]*(at lines [0-9]+--[0-9]+|has occurred while \\output is active|in (paragraph|alignment) at lines [0-9]+--[0-9]+)' |
        sed 's/  */ /g' | sort
}
faults_of() {
    grep -E '^! ' "$1" | sed 's/  */ /g' | sort || true
}

# Strip comments and blank lines from the manifest.
sed 's/#.*//' "$manifest" | grep -v '^[[:space:]]*$' > "$work/manifest.clean"

if [ "$fetch_only" -eq 1 ]; then
    while IFS='	' read -r name format path want note; do
        [ -n "$name" ] || continue
        fetch "$path" "$name.tex" "$want"
        printf '%s  %s\n' "$name" "ok"
    done <"$work/manifest.clean"
    printf '%s documents match their pinned digests\n' \
        "$(wc -l <"$work/manifest.clean")"
    exit 0
fi

# HSTEX'S LATEX FORMAT, BUILT ONCE. The reference starts each document from
# a format its distribution built once, so an HSTeX that read latex.ltx afresh
# for every document would not be doing the reference's work: reading it is
# about thirty times what setting a document costs, and it swamps everything
# else. A profile taken through a run that rebuilt it puts the macro
# expander at 25% of the run and its substitution step at 5.6%; over the
# document pass those are 13% and 3.3%, so the two disagree about what the
# engine spends its time on. Build it once here, and report what it cost on
# a line of its own.
hfmt=$work/latex.hfmt
plainfmt=$work/plain.hfmt
format_seconds=-
plain_format_seconds=-
if grep -q "	latex	" "$work/manifest.clean"; then
    format_start=$(date +%s)
    "$engine" --make-format "$latex_ltx" "$hfmt" >"$work/format.log" 2>&1 ||
        { echo "could not build the LaTeX format; see $work/format.log" >&2
          exit 1; }
    format_seconds=$(($(date +%s) - format_start))
fi
if grep -q "	plain	" "$work/manifest.clean"; then
    # plain.tex does not dump itself -- iniTeX is told to, and so is this.
    printf '\\input plain \\dump\n' >"$work/mkplain.tex"
    plain_start=$(date +%s)
    "$engine" --make-format "$work/mkplain.tex" "$plainfmt" \
        >"$work/plain-format.log" 2>&1 ||
        { echo "could not build the plain format; see $work/plain-format.log" >&2
          exit 1; }
    plain_format_seconds=$(($(date +%s) - plain_start))
fi

printf '%-10s %-9s %-11s %-8s %s\n' document pages boxes output verdict
printf '%-10s %-9s %-11s %-8s %s\n' ---------- --------- ----------- -------- -------
disagreed=0
detail=$work/detail.txt
: >"$detail"

while IFS='	' read -r name format path want note; do
    [ -n "$name" ] || continue
    fetch "$path" "$name.tex" "$want"

    dir=$work/$name
    rm -rf "$dir"
    mkdir -p "$dir/ref" "$dir/hstex/build"
    cp "$src/$name.tex" "$dir/ref/"
    cp "$src/$name.tex" "$dir/hstex/"

    # The reference. A plain document is set to DVI with the clock pinned, so
    # that what comes out can be compared byte for byte with HSTeX's.
    ( cd "$dir/ref"
      if [ "$format" = plain ]; then
          pdftex -output-format=dvi -interaction=nonstopmode \
              "$clock \\input $name \\end" >stdout.txt 2>&1 || true
      else
          pdflatex -interaction=nonstopmode "$name.tex" >stdout.txt 2>&1 || true
      fi ) || true
    ref_log=$dir/ref/$name.log

    # HSTeX over the same file.
    hs_log=$dir/hstex/hstex.log
    hs_status=0
    ( cd "$dir/hstex"
      if [ "$format" = plain ]; then
          printf '\\pdfoutput=0 %s \\input %s \\end\n' "$clock" "$name" \
              >"run-$name.tex"
          "$engine" --format "$plainfmt" "run-$name.tex" >hstex.log 2>&1
      else
          "$engine" --format "$hfmt" "$name.tex" >hstex.log 2>&1
      fi ) || hs_status=$?

    # What each engine actually produced. Only a plain document can be
    # compared this way: a PDF carries its own identifiers and timestamps.
    output=-
    if [ "$format" = plain ]; then
        ref_dvi=$dir/ref/$name.dvi
        hs_dvi=$dir/hstex/build/document-output/run-$name.dvi
        if [ ! -f "$ref_dvi" ] || [ ! -f "$hs_dvi" ]; then
            output=missing
        elif cmp -s "$ref_dvi" "$hs_dvi"; then
            output=same
        else
            output=differs
        fi
    fi

    ref_pages=$(pages_of "$ref_log" 2>/dev/null || true)
    hs_pages=$(pages_of "$hs_log" 2>/dev/null || true)
    boxes_of "$ref_log" >"$dir/ref.boxes" 2>/dev/null || : >"$dir/ref.boxes"
    boxes_of "$hs_log" >"$dir/hstex.boxes" 2>/dev/null || : >"$dir/hstex.boxes"
    ref_boxes=$(wc -l <"$dir/ref.boxes")
    hs_boxes=$(wc -l <"$dir/hstex.boxes")

    if [ "$hs_status" -ne 0 ]; then
        stop=$(grep -oE '[^ ]*\.tex:[0-9]+:[0-9]+: .*' "$hs_log" | tail -1)
        verdict="stopped: ${stop:-exit $hs_status}"
        disagreed=$((disagreed + 1))
        hs_pages=${hs_pages:--}
        hs_boxes=-
    elif [ "${ref_pages:-x}" != "${hs_pages:-y}" ]; then
        verdict="page count differs"
        disagreed=$((disagreed + 1))
    elif ! diff -q "$dir/ref.boxes" "$dir/hstex.boxes" >/dev/null; then
        verdict="box reports differ"
        disagreed=$((disagreed + 1))
        { printf '=== %s: box reports ===\n' "$name"
          diff "$dir/ref.boxes" "$dir/hstex.boxes" || true; } >>"$detail"
    elif [ "$output" = differs ] || [ "$output" = missing ]; then
        verdict="the output itself differs"
        disagreed=$((disagreed + 1))
    else
        verdict=agrees
    fi

    printf '%-10s %-9s %-11s %-8s %s\n' \
        "$name" "${ref_pages:--}/${hs_pages:--}" "$ref_boxes/$hs_boxes" \
        "$output" "$verdict"
done <"$work/manifest.clean"

if [ -s "$detail" ]; then
    echo
    cat "$detail"
fi

echo
total=$(wc -l <"$work/manifest.clean")
echo "$((total - disagreed))/$total documents agree with the reference"

# Seven warm runs of one engine over one document, each in an output directory
# of its own, and the median of what they took. A single run on a machine with
# other tenants varies by about a fifth, so the median of seven is what
# docs/BENCHMARK_CONTRACT.md asks for. This shell has no local variables, so
# these names are deliberately distinct from every other loop's.
time_median() {
    tm_side=$1
    tm_name=$2
    tm_format=$3
    : >"$work/times"
    tm_i=0
    while [ "$tm_i" -lt 7 ]; do
        tm_dir=$work/$tm_name/t-$tm_side
        rm -rf "$tm_dir"
        mkdir -p "$tm_dir/build"
        cp "$src/$tm_name.tex" "$tm_dir/"
        if [ "$tm_format" = plain ]; then
            printf '\\pdfoutput=0 %s \\input %s \\end\n' "$clock" "$tm_name" \
                >"$tm_dir/run-$tm_name.tex"
        fi
        ( cd "$tm_dir"
          if [ "$tm_side" = ref ] && [ "$tm_format" = plain ]; then
              /usr/bin/time -f %e -o t.txt pdftex -output-format=dvi \
                  -interaction=nonstopmode "$clock \\input $tm_name \\end" \
                  >/dev/null 2>&1 || :
          elif [ "$tm_side" = ref ]; then
              /usr/bin/time -f %e -o t.txt pdflatex -interaction=nonstopmode \
                  "$tm_name.tex" >/dev/null 2>&1 || :
          elif [ "$tm_format" = plain ]; then
              /usr/bin/time -f %e -o t.txt "$engine" --format "$plainfmt" \
                  "run-$tm_name.tex" >/dev/null 2>&1 || :
          else
              /usr/bin/time -f %e -o t.txt "$engine" --format "$hfmt" \
                  "$tm_name.tex" >/dev/null 2>&1 || :
          fi ) || :
        cat "$tm_dir/t.txt" >>"$work/times" 2>/dev/null || echo 0 >>"$work/times"
        tm_i=$((tm_i + 1))
    done
    sort -n "$work/times" | awk 'NR==4 {print $1}'
}

if [ "$time_runs" -eq 1 ]; then
    if ! /usr/bin/time -f %e -o /dev/null true >/dev/null 2>&1; then
        echo
        echo "--time wants GNU time at /usr/bin/time; no timings reported" >&2
    else
        echo
        printf '%-10s %-11s %-11s %s\n' document reference hstex speedup
        printf '%-10s %-11s %-11s %s\n' ---------- ----------- ----------- -------
        while IFS='	' read -r name format path want note; do
            [ -n "$name" ] || continue
            ref_seconds=$(time_median ref "$name" "$format")
            hs_seconds=$(time_median hstex "$name" "$format")
            printf '%-10s %-11s %-11s %s\n' "$name" "${ref_seconds}s" \
                "${hs_seconds}s" \
                "$(awk -v r="$ref_seconds" -v h="$hs_seconds" \
                     'BEGIN { if (h > 0) printf "%.2fx", r / h; else print "-" }')"
        done <"$work/manifest.clean"
        echo
        echo "The document pass alone, each engine starting from a format it"
        echo "already had. HSTeX's LaTeX format took ${format_seconds}s to build,"
        echo "once, and is not counted above; the reference's was built once by"
        echo "its distribution and is not counted either. HSTeX's plain format"
        echo "took ${plain_format_seconds}s and is not counted above; the"
        echo "reference starts a plain document from its own preloaded format."
    fi
fi
if [ "$strict" -eq 1 ] && [ "$disagreed" -ne 0 ]; then
    exit 1
fi
