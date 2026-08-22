#!/bin/bash
# Build the engine as one file that needs nothing else: statically linked,
# with no interpreter to load and no shared library to find at run time.
#
#   tools/build-static.sh [--musl] [--mimalloc] [build directory]
#
# The default links against the build machine's glibc. Nothing the engine
# asks of a libc is among the parts glibc keeps dynamic -- it looks up no
# user, resolves no host, opens no plugin -- so the binary runs where no
# glibc is installed at all. It is still glibc's code inside, statically.
#
# --musl links against musl instead, which is what independence of this
# machine's libc means in practice: a binary of half the size carrying no
# GNU runtime at all. It needs musl-gcc, from the musl-tools package on
# Debian and Ubuntu, or from musl's own source configured with a --prefix,
# which wants no root and takes a minute. Two things to know before
# publishing a number taken from one: the heap tuning in src/main.c is
# glibc's own and quietly does nothing under musl, and musl's allocator is
# the slower of the two here -- the engine test runs about five per cent
# longer against it, 3.29s where glibc's static build takes 3.12s, the two
# built alike in every other way. A musl build is the portable one, not
# the fast one.
#
# musl-gcc is the toolchain to use, not a clang-and-lld one: the runtime
# dispatch in src/scan.c asks __builtin_cpu_supports, whose
# __cpu_indicator_init lives in libgcc, and a musl target built on zig's
# compiler-rt has no such symbol to link against.
#
# --mimalloc gives the musl build a different allocator rather than a
# tuned one, musl having no tuning to offer: mallocng reads no
# environment and answers no mallopt, and the choice musl's own configure
# does allow -- --with-malloc=oldmalloc -- measured slower here than the
# default. mimalloc, linked in front of musl's, is worth about a quarter of
# the engine test, and most of what musl costs: at the median of fifteen
# interleaved rounds musl takes 0.64s where glibc takes 0.47s, and mimalloc
# brings musl to 0.49s. The machine was shared while that was taken -- load
# average 72 across 64 cores -- so take a real figure on a quiet one before
# publishing it. What is left over glibc is the heap tuning src/main.c does
# there and musl has no answer to.
#
# That the difference shows at all is recent. While the engine started a
# child process for every file it looked for, the allocator was a few per
# cent of a run four seconds long; src/filedb.c took the children away, and
# what is left is short enough for the allocator to be a quarter of it.
#
# It is offered for the musl build alone. Overriding malloc in a static
# glibc link pulls glibc's own malloc.o in beside it and the two collide,
# and glibc gains nothing measurable from the swap in any case.
#
# The link is optimized whole, the way a published build is. What gcc
# says about the hyphenation loops when it does that is answered in
# meson.build, not here.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The allocator is pinned to a commit, not to a tag that can be moved.
mimalloc_tag=v3.5.0
mimalloc_commit=18b08671c9302247bfb682286e6bf3cc1773f801

libc=glibc
mimalloc=0
while :; do
  case ${1:-} in
  --musl) libc=musl; shift ;;
  --mimalloc) mimalloc=1; shift ;;
  *) break ;;
  esac
done
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
meson compile -C "$build" hstex

binary="$build/hstex"

# What was asked for is the absence of a dependency, so it is the binary
# that has to say so, not the flags that went in. A static link has no
# interpreter to name and no library to need.
if readelf -lW "$binary" | grep -q INTERP; then
  echo "tools/build-static.sh: $binary still asks for an interpreter" >&2
  exit 1
fi
if readelf -dW "$binary" 2>/dev/null | grep -q NEEDED; then
  echo "tools/build-static.sh: $binary still needs a shared library" >&2
  exit 1
fi

# The same again for the allocator: whether it was linked in is a
# question for the binary, which says so when asked to be verbose.
if [ "$mimalloc" -eq 1 ]; then
  if ! MIMALLOC_VERBOSE=1 "$binary" --version 2>&1 >/dev/null |
      grep -q '^mimalloc:'; then
    echo "tools/build-static.sh: $binary is not serving malloc from mimalloc" >&2
    exit 1
  fi
  libc="$libc+mimalloc"
fi

echo "built $binary ($libc, $(du -h "$binary" | cut -f1), $("$binary" --version))"
