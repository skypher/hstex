#!/bin/bash
# Build a statically linked, whole-program-optimized engine.
#
#   tools/build-static.sh [--musl] [--mimalloc] [build directory]
#
# The default uses glibc. --musl selects musl-gcc for a libc-independent
# binary, and --mimalloc links the pinned mimalloc source into that musl build.
# Runtime CPU dispatch continues to use libgcc through musl-gcc.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  printf '%s\n' \
    'Usage: tools/build-static.sh [--musl] [--mimalloc] [BUILD-DIRECTORY]' \
    'Build a statically linked HSTeX binary.' \
    '  --musl      build with musl-gcc instead of glibc' \
    '  --mimalloc  link the pinned mimalloc source into a musl build' \
    '  -h, --help  show this help and exit'
}

# The allocator is pinned to a commit, not to a tag that can be moved.
mimalloc_tag=v3.5.0
mimalloc_commit=18b08671c9302247bfb682286e6bf3cc1773f801

libc=glibc
mimalloc=0
while :; do
  case ${1:-} in
  --musl) libc=musl; shift ;;
  --mimalloc) mimalloc=1; shift ;;
  -h|--help) usage; exit 0 ;;
  --) shift; break ;;
  -*) printf 'unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  *) break ;;
  esac
done
if [ "$#" -gt 1 ]; then
  usage >&2
  exit 2
fi
build="${1:-$root/build-static}"

link=(-static)

if [ "$libc" = musl ]; then
  if ! command -v musl-gcc >/dev/null; then
    echo "tools/build-static.sh: musl-gcc not found." >&2
    echo "  Debian and Ubuntu: sudo apt-get install musl-tools" >&2
    echo "  Fedora: sudo dnf install musl-gcc" >&2
    echo "  Anywhere, without root: configure musl's own source --prefix=DIR" >&2
    exit 1
  fi
  export CC=musl-gcc
elif [ "$mimalloc" -eq 1 ]; then
  echo "tools/build-static.sh: --mimalloc is for --musl builds." >&2
  echo "  A static glibc link pulls in glibc's malloc beside it and the" >&2
  echo "  two collide; glibc's own allocator measured no slower here." >&2
  exit 1
fi

mkdir -p "$build"

if [ "$mimalloc" -eq 1 ]; then
  source_dir="$build/mimalloc"
  if [ ! -d "$source_dir" ]; then
    git clone --quiet --depth 1 --branch "$mimalloc_tag" \
      https://github.com/microsoft/mimalloc.git "$source_dir"
  fi
  have=$(git -C "$source_dir" rev-parse HEAD)
  if [ "$have" != "$mimalloc_commit" ]; then
    echo "tools/build-static.sh: $source_dir is $have," >&2
    echo "  not the pinned $mimalloc_commit." >&2
    exit 1
  fi
  # One translation unit is all mimalloc asks for, and its own warnings
  # are not this project's to answer.
  "${CC:-cc}" -O2 -w -DNDEBUG -DMI_MALLOC_OVERRIDE=1 \
    -I "$source_dir/include" -c "$source_dir/src/static.c" \
    -o "$build/mimalloc.o"
  ar rcs "$build/libmimalloc.a" "$build/mimalloc.o"
  link+=(-L"$build" -lmimalloc)
fi

configure=(--buildtype=release -Db_lto=true -Db_ndebug=true -Db_pie=false
           "-Dc_link_args=${link[*]}")

if [ -f "$build/build.ninja" ]; then
  meson configure "$build" "${configure[@]}" >/dev/null
else
  meson setup "$build" "${configure[@]}" >/dev/null
fi
meson compile -C "$build" hstex hstex-pdflatex

binaries=("$build/hstex" "$build/hstex-pdflatex")

# What was asked for is the absence of a dependency, so it is the binary
# that has to say so, not the flags that went in. A static link has no
# interpreter to name and no library to need.
for binary in "${binaries[@]}"; do
  if readelf -lW "$binary" | grep -q INTERP; then
    echo "tools/build-static.sh: $binary still asks for an interpreter" >&2
    exit 1
  fi
  if readelf -dW "$binary" 2>/dev/null | grep -q NEEDED; then
    echo "tools/build-static.sh: $binary still needs a shared library" >&2
    exit 1
  fi
done

# The same again for the allocator: whether it was linked in is a
# question for the binary, which says so when asked to be verbose.
if [ "$mimalloc" -eq 1 ]; then
  if ! MIMALLOC_VERBOSE=1 "$build/hstex" --version 2>&1 >/dev/null |
      grep -q '^mimalloc:'; then
    echo "tools/build-static.sh: $build/hstex is not serving malloc from mimalloc" >&2
    exit 1
  fi
  libc="$libc+mimalloc"
fi

for binary in "${binaries[@]}"; do
  echo "built $binary ($libc, $(du -h "$binary" | cut -f1), $("$binary" --version))"
done
