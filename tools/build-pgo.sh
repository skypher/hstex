#!/bin/bash
# Build the engine the way a published timing is measured: optimized, linked
# in one piece, and told beforehand which way its branches go.
#
#   tools/build-pgo.sh [build directory]
#
# The profile is taken from work that is not the milestone corpus -- building
# the format from the installed `latex.ltx`, and three passes over
# `benchmarks/training/train.tex` -- so that no measurement on the corpus is
# taken with a build that was told the corpus's own answers.
set -euo pipefail

usage() {
  printf '%s\n' 'Usage: tools/build-pgo.sh [BUILD-DIRECTORY]'
  printf '%s\n' 'Build an optimized HSTeX binary using the training corpus for PGO.'
}

case ${1:-} in
  -h|--help)
    usage
    exit 0
    ;;
  -*)
    printf 'unknown option: %s\n' "$1" >&2
    usage >&2
    exit 2
    ;;
esac
if [ "$#" -gt 1 ]; then
  usage >&2
  exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build-pgo}"
training="$root/benchmarks/training/train.tex"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

flags='-O3 -fno-plt -fno-semantic-interposition'
configure=(--buildtype=release -Db_lto=true -Db_pie=false -Db_ndebug=true
           "-Dc_args=$flags" -Dc_link_args=-fno-plt)

if [ -f "$build/build.ninja" ]; then
  meson configure "$build" "${configure[@]}" -Db_pgo=generate >/dev/null
else
  meson setup "$build" "${configure[@]}" -Db_pgo=generate >/dev/null
fi
find "$build" -name '*.gcda' -delete
meson compile -C "$build" hstex

echo "training: the format"
( cd "$work" && "$build/hstex" --make-format "$(kpsewhich latex.ltx)" \
    train.hfmt >/dev/null )
echo "training: a document"
mkdir -p "$work/build/document-output"
cp "$training" "$work/train.tex"
for pass in 1 2 3; do
  ( cd "$work" && TEXINPUTS="$root/benchmarks/texmf:" \
      "$build/hstex" --format train.hfmt train.tex >/dev/null )
done

meson configure "$build" -Db_pgo=use >/dev/null
meson compile -C "$build"
echo "built $build/hstex"
