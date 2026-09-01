#!/bin/sh
# The public document corpus, run as a behavioral comparison.
#
# Each document is fetched from CTAN, pinned by digest, and typeset twice:
# once by the reference engine and once by HSTeX.  Nothing of the reference
# but its observable output is used by this runner. See SOURCE_POLICY.md.
#
# The comparison includes logs, auxiliary state, page geometry, line and page
# breaks, glyph placement, extracted text, links, destinations, bookmarks, and
# deterministic rendered pages. The reference's own storage statistics
# describe its implementation and are not semantic comparison targets.
#
# Usage: tests/corpus/run-corpus.sh [--stress] [--strict] [--fetch-only]
#                                   [--time] [path-to-hstex]
#        CORPUS_WORK=dir tests/corpus/run-corpus.sh
#
# --stress selects deliberately adversarial documents, including interactive
# inputs and documents that expose known incompatibilities.  It uses a
# separate work directory and is non-strict unless --strict is also given.
#
# --strict exits nonzero if any document disagrees.  Without it the script
# reports the state of the selected corpus and exits 0.
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

usage() {
    cat <<'EOF'
Usage: tests/corpus/run-corpus.sh [OPTIONS] [path-to-hstex]

Run pinned public TeX documents through a reference engine and HSTeX.

Options:
  --stress      select the adversarial stress-document manifest
  --strict      exit nonzero when a document disagrees
  --fetch-only  fetch documents and verify their SHA-256 digests only
  --time        report median warm document-pass timings
  -h, --help    show this help and exit

CORPUS_WORK overrides the selected suite's work directory. The defaults are
build/corpus for the release corpus and build/corpus-stress for --stress.
EOF
}

strict=0
fetch_only=0
time_runs=0
suite=release
while :; do
    case ${1:-} in
    --stress) suite=stress; shift ;;
    --strict) strict=1; shift ;;
    --fetch-only) fetch_only=1; shift ;;
    --time) time_runs=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*) printf 'unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    *) break ;;
    esac
done

engine=${1:-./build/hstex}
case $engine in
/*) ;;
*) engine=$(pwd)/$engine ;;
esac

here=$(cd "$(dirname "$0")" && pwd)
if [ "$suite" = stress ]; then
    manifest=$here/stress-documents.tsv
    default_work=build/corpus-stress
else
    manifest=$here/documents.tsv
    default_work=build/corpus
fi
work=${CORPUS_WORK:-$default_work}
mkdir -p "$work"
work=$(cd "$work" && pwd)
src=$work/src
mkdir -p "$src"

# Availability decides only where bytes are fetched. Every candidate is an
# official CTAN endpoint, and only a file matching the pinned digest is moved
# into the corpus cache.
corpus_bases='https://mirrors.mit.edu/CTAN
https://ctan.math.washington.edu/tex-archive
https://mirrors.ctan.org
https://ctan.math.illinois.edu
https://mirrors.ibiblio.org/CTAN'
# A document that prints the date must be given one, or the two runs differ by
# the time of day and nothing else.
clock='\time=600 \day=1 \month=1 \year=2026'
SOURCE_DATE_EPOCH=1767261600
FORCE_SOURCE_DATE=1
export SOURCE_DATE_EPOCH FORCE_SOURCE_DATE
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
    fetch_temporary=$src/$fetch_file.fetch.$$
    for fetch_base in $corpus_bases; do
        if curl -sSLf --retry 3 --retry-delay 2 --retry-connrefused \
            --connect-timeout 20 -o "$fetch_temporary" \
            "$fetch_base/$fetch_path" &&
           printf '%s  %s\n' "$fetch_want" "$fetch_temporary" |
               sha256sum -c - >/dev/null; then
            mv -f "$fetch_temporary" "$src/$fetch_file"
            return 0
        fi
    done
    rm -f "$fetch_temporary"
    return 1
}

# Reproducible answers for public tests that use terminal \read or \typein.
# An empty profile also gives noninteractive documents a closed stdin.
make_stdin() {
    stdin_profile=$1
    stdin_file=$2
    case ${stdin_profile:-none} in
    none) : >"$stdin_file" ;;
    returns)
        awk 'BEGIN { for (i = 0; i < 64; ++i) print "" }' >"$stdin_file"
        ;;
    testpage) printf 'letterpaper\nn\n' >"$stdin_file" ;;
    testfont) printf 'cmr10\n\\bigtest\n\\bye\n' >"$stdin_file" ;;
    *)
        printf 'unknown stdin profile %s in %s\n' \
            "$stdin_profile" "$manifest" >&2
        exit 2
        ;;
    esac
}

# What the log says about the document.
pages_of() {
    sed -n \
        -e 's/.*Output written on [^(]*(\([0-9][0-9]*\) page.*/\1/p' \
        -e 's/.*No pages of output.*/0/p' "$1" | tail -1
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
    while IFS='	' read -r name format path want note input_profile; do
        [ -n "$name" ] || continue
        suffix=${path##*.}
        fetch "$path" "$name.$suffix" "$want"
        printf '%s  %s\n' "$name" "ok"
    done <"$work/manifest.clean"
    printf '%s documents match their pinned digests\n' \
        "$(wc -l <"$work/manifest.clean")"
    exit 0
fi

if grep -q "	latex	" "$work/manifest.clean"; then
    for comparison_tool in python3 mutool pdfinfo pdftotext; do
        if ! command -v "$comparison_tool" >/dev/null 2>&1; then
            printf 'required corpus comparison tool is missing: %s\n' \
                "$comparison_tool" >&2
            exit 1
        fi
    done
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
    "$engine" --make-ini-format "$work/mkplain.tex" "$plainfmt" \
        >"$work/plain-format.log" 2>&1 ||
        { echo "could not build the plain format; see $work/plain-format.log" >&2
          exit 1; }
    plain_format_seconds=$(($(date +%s) - plain_start))
fi

printf '%-20s %-9s %-11s %-9s %-8s %s\n' \
    document pages boxes faults output verdict
printf '%-20s %-9s %-11s %-9s %-8s %s\n' \
    -------------------- --------- ----------- --------- -------- -------
disagreed=0
detail=$work/detail.txt
: >"$detail"

while IFS='	' read -r name format path want note input_profile; do
    [ -n "$name" ] || continue
    suffix=${path##*.}
    input_file=$name.$suffix
    fetch "$path" "$input_file" "$want"

    dir=$work/$name
    rm -rf "$dir"
    mkdir -p "$dir/ref" "$dir/hstex/build"
    cp "$src/$input_file" "$dir/ref/"
    cp "$src/$input_file" "$dir/hstex/"
    make_stdin "$input_profile" "$dir/stdin.txt"

    # The reference. A plain document is set to DVI with the clock pinned, so
    # that what comes out can be compared byte for byte with HSTeX's.
    ( cd "$dir/ref"
      if [ "$format" = plain ]; then
          if [ "${input_profile:-none}" = none ]; then
              pdftex -output-format=dvi -interaction=nonstopmode \
                  "$clock \\input $input_file \\end" \
                  <../stdin.txt >stdout.txt 2>&1 || true
          else
              pdftex -output-format=dvi \
                  "$clock \\input $input_file \\end" \
                  <../stdin.txt >stdout.txt 2>&1 || true
          fi
      else
          if [ "${input_profile:-none}" = none ]; then
              pdflatex -interaction=nonstopmode "$input_file" \
                  <../stdin.txt >stdout.txt 2>&1 || true
          else
              pdflatex "$input_file" <../stdin.txt >stdout.txt 2>&1 || true
          fi
      fi ) || true
    ref_log=$dir/ref/$name.log
    if [ ! -f "$ref_log" ]; then
        printf 'reference produced no log for %s; see %s\n' \
            "$input_file" "$dir/ref/stdout.txt" >&2
        exit 1
    fi

    # HSTeX over the same file.
    hs_log=$dir/hstex/hstex.log
    hs_status=0
    ( cd "$dir/hstex"
      if [ "$format" = plain ]; then
          printf '\\pdfoutput=0 %s \\input %s \\end\n' \
              "$clock" "$input_file" \
              >"run-$name.tex"
          "$engine" --format "$plainfmt" "run-$name.tex" \
              <../stdin.txt >hstex.log 2>&1
      else
          "$engine" --format "$hfmt" "$input_file" \
              <../stdin.txt >hstex.log 2>&1
      fi ) || hs_status=$?

    # What each engine actually produced. DVI is byte-stable with the pinned
    # clock. PDF is compared semantically because identifiers, timestamps,
    # compression, and object numbering may differ.
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
    else
        ref_pdf=$dir/ref/$name.pdf
        hs_output_dir=$dir/hstex/build/document-output
        hs_pdf=$hs_output_dir/$name.pdf
        pdf_output=none
        if [ "$hs_status" -ne 0 ]; then
            if [ -f "$ref_pdf" ] || [ -f "$hs_pdf" ]; then
                pdf_output=differs
            fi
        elif [ -f "$ref_pdf" ] && [ -f "$hs_pdf" ]; then
            mkdir -p "$dir/pdf-semantics"
            if python3 -u "$here/compare-pdf.py" \
                "$ref_pdf" "$hs_pdf" --artifacts "$dir/pdf-semantics" \
                >"$dir/pdf.compare" 2>&1; then
                pdf_output=same
            else
                pdf_compare_status=$?
                if [ "$pdf_compare_status" -eq 1 ]; then
                    pdf_output=differs
                    { printf '=== %s: PDF semantics ===\n' "$name"
                      cat "$dir/pdf.compare"; } >>"$detail"
                else
                    printf 'PDF comparison failed for %s; see %s\n' \
                        "$name" "$dir/pdf.compare" >&2
                    cat "$dir/pdf.compare" >&2
                    exit 1
                fi
            fi
        elif [ -f "$ref_pdf" ] || [ -f "$hs_pdf" ]; then
            pdf_output=missing
        fi

        # Cross-reference and navigation files are semantic state between
        # passes. Exact bytes are expected because both runs use the same job
        # name, input, environment, and pass count.
        auxiliary_output=none
        : >"$dir/auxiliary.compare"
        for auxiliary_suffix in \
            aux bbl bcf brf idx ind lof lol lot nav out run.xml snm toc vrb; do
            ref_auxiliary=$dir/ref/$name.$auxiliary_suffix
            hs_auxiliary=$hs_output_dir/$name.$auxiliary_suffix
            if [ -f "$ref_auxiliary" ] || [ -f "$hs_auxiliary" ]; then
                [ "$auxiliary_output" = none ] && auxiliary_output=same
                if [ ! -f "$ref_auxiliary" ] || [ ! -f "$hs_auxiliary" ]; then
                    auxiliary_output=missing
                    printf '%s.%s exists on only one side\n' \
                        "$name" "$auxiliary_suffix" >>"$dir/auxiliary.compare"
                elif ! cmp -s "$ref_auxiliary" "$hs_auxiliary"; then
                    [ "$auxiliary_output" = missing ] || auxiliary_output=differs
                    { printf '%s.%s differs\n' "$name" "$auxiliary_suffix"
                      diff -u "$ref_auxiliary" "$hs_auxiliary" || true; } \
                        >>"$dir/auxiliary.compare"
                fi
            fi
        done
        if [ "$auxiliary_output" = differs ] || \
           [ "$auxiliary_output" = missing ]; then
            { printf '=== %s: auxiliary state ===\n' "$name"
              cat "$dir/auxiliary.compare"; } >>"$detail"
        fi

        if [ "$pdf_output" = missing ] || [ "$auxiliary_output" = missing ]; then
            output=missing
        elif [ "$pdf_output" = differs ] || [ "$auxiliary_output" = differs ]; then
            output=differs
        elif [ "$pdf_output" = same ] || [ "$auxiliary_output" = same ]; then
            output=same
        else
            output=none
        fi
    fi

    ref_pages=$(pages_of "$ref_log" 2>/dev/null || true)
    hs_pages=$(pages_of "$hs_log" 2>/dev/null || true)
    boxes_of "$ref_log" >"$dir/ref.boxes" 2>/dev/null || : >"$dir/ref.boxes"
    boxes_of "$hs_log" >"$dir/hstex.boxes" 2>/dev/null || : >"$dir/hstex.boxes"
    ref_boxes=$(wc -l <"$dir/ref.boxes")
    hs_boxes=$(wc -l <"$dir/hstex.boxes")
    faults_of "$ref_log" >"$dir/ref.faults" 2>/dev/null || : >"$dir/ref.faults"
    faults_of "$hs_log" >"$dir/hstex.faults" 2>/dev/null || : >"$dir/hstex.faults"
    ref_faults=$(wc -l <"$dir/ref.faults")
    hs_faults=$(wc -l <"$dir/hstex.faults")

    if [ "$hs_status" -ne 0 ]; then
        stop=$(grep -oE '[^ ]*\.(tex|ltx):[0-9]+:[0-9]+: .*' \
            "$hs_log" | tail -1)
        verdict="stopped: ${stop:-exit $hs_status}"
        disagreed=$((disagreed + 1))
        hs_pages=${hs_pages:--}
        hs_boxes=-
        hs_faults=-
    elif [ "${ref_pages:-x}" != "${hs_pages:-y}" ]; then
        verdict="page count differs"
        disagreed=$((disagreed + 1))
    elif ! diff -q "$dir/ref.boxes" "$dir/hstex.boxes" >/dev/null; then
        verdict="box reports differ"
        disagreed=$((disagreed + 1))
        { printf '=== %s: box reports ===\n' "$name"
          diff "$dir/ref.boxes" "$dir/hstex.boxes" || true; } >>"$detail"
    elif ! diff -q "$dir/ref.faults" "$dir/hstex.faults" >/dev/null; then
        verdict="faults differ"
        disagreed=$((disagreed + 1))
        { printf '=== %s: faults ===\n' "$name"
          diff "$dir/ref.faults" "$dir/hstex.faults" || true; } >>"$detail"
    elif [ "$output" = differs ] || [ "$output" = missing ]; then
        verdict="the output itself differs"
        disagreed=$((disagreed + 1))
    else
        verdict=agrees
    fi

    printf '%-20s %-9s %-11s %-9s %-8s %s\n' \
        "$name" "${ref_pages:--}/${hs_pages:--}" "$ref_boxes/$hs_boxes" \
        "$ref_faults/$hs_faults" "$output" "$verdict"
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
# docs/BENCHMARK_CONTRACT.md asks for.
#
# The clock is read either side of each run rather than the run being handed
# to `time', because `time' resolves ten milliseconds and the shortest
# document here takes fifty: a tick would be a fifth of the answer, and a
# five per cent change -- which is about what a worthwhile one is -- would
# not show at all. Asked for in nanoseconds and reported in milliseconds.
# This shell has no local variables, so these names are deliberately distinct
# from every other loop's.
time_median() {
    tm_side=$1
    tm_name=$2
    tm_format=$3
    tm_input_profile=$4
    tm_path=$5
    : >"$work/times"
    tm_i=0
    while [ "$tm_i" -lt 7 ]; do
        tm_dir=$work/$tm_name/t-$tm_side
        rm -rf "$tm_dir"
        mkdir -p "$tm_dir/build"
        tm_suffix=${tm_path##*.}
        tm_input_file=$tm_name.$tm_suffix
        cp "$src/$tm_input_file" "$tm_dir/"
        make_stdin "$tm_input_profile" "$tm_dir/stdin.txt"
        if [ "$tm_format" = plain ]; then
            printf '\\pdfoutput=0 %s \\input %s \\end\n' \
                "$clock" "$tm_input_file" \
                >"$tm_dir/run-$tm_name.tex"
        fi
        tm_started=$(date +%s%N)
        ( cd "$tm_dir"
          if [ "$tm_side" = ref ] && [ "$tm_format" = plain ]; then
              if [ "${tm_input_profile:-none}" = none ]; then
                  pdftex -output-format=dvi -interaction=nonstopmode \
                      "$clock \\input $tm_input_file \\end" \
                      <stdin.txt >/dev/null 2>&1 || :
              else
                  pdftex -output-format=dvi \
                      "$clock \\input $tm_input_file \\end" \
                      <stdin.txt >/dev/null 2>&1 || :
              fi
          elif [ "$tm_side" = ref ]; then
              if [ "${tm_input_profile:-none}" = none ]; then
                  pdflatex -interaction=nonstopmode "$tm_input_file" \
                      <stdin.txt >/dev/null 2>&1 || :
              else
                  pdflatex "$tm_input_file" \
                      <stdin.txt >/dev/null 2>&1 || :
              fi
          elif [ "$tm_format" = plain ]; then
              "$engine" --format "$plainfmt" "run-$tm_name.tex" \
                  <stdin.txt >/dev/null 2>&1 || :
          else
              "$engine" --format "$hfmt" "$tm_input_file" \
                  <stdin.txt >/dev/null 2>&1 || :
          fi ) || :
        tm_ended=$(date +%s%N)
        echo "$tm_started $tm_ended" |
            awk '{ printf "%.1f\n", ($2 - $1) / 1000000 }' >>"$work/times"
        tm_i=$((tm_i + 1))
    done
    sort -n "$work/times" | awk 'NR==4 {print $1}'
}

if [ "$time_runs" -eq 1 ]; then
    if [ "$(date +%N)" = "N" ]; then
        echo
        echo "--time wants a date(1) that reads nanoseconds; none reported" >&2
    else
        echo
        printf '%-10s %-11s %-11s %s\n' document reference hstex speedup
        printf '%-10s %-11s %-11s %s\n' ---------- ----------- ----------- -------
        while IFS='	' read -r name format path want note input_profile; do
            [ -n "$name" ] || continue
            ref_seconds=$(time_median \
                ref "$name" "$format" "$input_profile" "$path")
            hs_seconds=$(time_median \
                hstex "$name" "$format" "$input_profile" "$path")
            printf '%-10s %-11s %-11s %s\n' "$name" "${ref_seconds}ms" \
                "${hs_seconds}ms" \
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
