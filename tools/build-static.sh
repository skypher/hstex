#!/bin/bash
# Build the engine as one file that needs nothing else: statically linked,
# with no interpreter to load and no shared library to find at run time.
#
#   tools/build-static.sh [--musl] [build directory]
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
# The link is optimized whole, the way a published build is. What gcc
# says about the hyphenation loops when it does that is answered in
# meson.build, not here.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

libc=glibc
if [ "${1:-}" = "--musl" ]; then
  libc=musl
  shift
fi
build="${1:-$root/build-static}"

configure=(--buildtype=release -Db_lto=true -Db_ndebug=true -Db_pie=false
           -Dc_link_args=-static)

if [ "$libc" = musl ]; then
  if ! command -v musl-gcc >/dev/null; then
    echo "tools/build-static.sh: musl-gcc not found." >&2
    echo "  Debian and Ubuntu: sudo apt-get install musl-tools" >&2
    echo "  Fedora: sudo dnf install musl-gcc" >&2
    exit 1
  fi
  export CC=musl-gcc
fi

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

echo "built $binary ($libc, $(du -h "$binary" | cut -f1), $("$binary" --version))"
