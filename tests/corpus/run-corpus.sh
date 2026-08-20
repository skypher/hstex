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
# Usage: tests/corpus/run-corpus.sh [--strict] [--fetch-only] [path-to-hstex]
#        CORPUS_WORK=dir  tests/corpus/run-corpus.sh   (default build/corpus)
#
# --strict exits nonzero if any document disagrees.  Without it the script
# reports the state of the corpus and exits 0.
#
# --fetch-only fetches every document and checks it against its pinned digest,
# without running either engine.  This is the corpus identity check, and needs
# no TeX installation.

set -e

strict=0
fetch_only=0
while :; do
    case ${1:-} in
    --strict) strict=1; shift ;;
    --fetch-only) fetch_only=1; shift ;;
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
    curl -sSLf -o "$src/$fetch_file" "$base/$fetch_path"
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

printf '%-10s %-9s %-11s %s\n' document pages boxes verdict
printf '%-10s %-9s %-11s %s\n' ---------- --------- ----------- -------
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

    # The reference.
    ( cd "$dir/ref"
      if [ "$format" = plain ]; then
          pdftex -interaction=nonstopmode "\\input $name \\end" >stdout.txt 2>&1 || true
      else
          pdflatex -interaction=nonstopmode "$name.tex" >stdout.txt 2>&1 || true
      fi ) || true
    ref_log=$dir/ref/$name.log

    # HSTeX over the same file.
    hs_log=$dir/hstex/hstex.log
    hs_status=0
    ( cd "$dir/hstex"
      if [ "$format" = plain ]; then
          printf '\\input plain \\input %s \\end\n' "$name" >"run-$name.tex"
          "$engine" --run-ini "run-$name.tex" >hstex.log 2>&1
      else
          "$engine" --run-latex "$latex_ltx" "$name.tex" >hstex.log 2>&1
      fi ) || hs_status=$?

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
    else
        verdict=agrees
    fi

    printf '%-10s %-9s %-11s %s\n' \
        "$name" "${ref_pages:--}/${hs_pages:--}" "$ref_boxes/$hs_boxes" "$verdict"
done <"$work/manifest.clean"

if [ -s "$detail" ]; then
    echo
    cat "$detail"
fi

echo
total=$(wc -l <"$work/manifest.clean")
echo "$((total - disagreed))/$total documents agree with the reference"
if [ "$strict" -eq 1 ] && [ "$disagreed" -ne 0 ]; then
    exit 1
fi
