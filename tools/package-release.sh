#!/bin/sh
# Package an already-tested static Linux build. Publishing is intentionally a
# separate GitHub Actions step so local rehearsals never alter a release.

set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: tools/package-release.sh BUILD_DIR OUTPUT_DIR" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$1
output=$2

case $build in
/*) ;;
*) build=$(CDPATH= cd -- "$build" && pwd) ;;
esac
case $output in
/*) ;;
*) output=$root/$output ;;
esac

for file in "$build/hstex" "$build/hstex-pdflatex" "$root/LICENSE" \
    "$root/NOTICE" "$root/THIRD_PARTY_NOTICES.md" \
    "$root/packaging/debian/copyright"; do
    if [ ! -f "$file" ]; then
        echo "tools/package-release.sh: required file is missing: $file" >&2
        exit 1
    fi
done
if [ ! -x "$build/hstex" ] || [ ! -x "$build/hstex-pdflatex" ]; then
    echo "tools/package-release.sh: release binaries are not executable" >&2
    exit 1
fi

version=$($build/hstex --version | awk 'NR == 1 { print $2 }')
if [ -z "$version" ]; then
    echo "tools/package-release.sh: cannot determine HSTeX version" >&2
    exit 1
fi
case $version in
*[!0-9A-Za-z._-]*|'')
    echo "tools/package-release.sh: unsafe version: $version" >&2
    exit 1
    ;;
esac

name=hstex-$version-linux-x86_64
archive=$output/$name.tar.gz
debian=$output/hstex_${version}_amd64.deb
checksums=$output/SHA256SUMS
if [ -e "$archive" ] || [ -e "$debian" ] || [ -e "$checksums" ]; then
    echo "tools/package-release.sh: release assets already exist in $output" >&2
    exit 1
fi

mkdir -p "$output"
stage=$(mktemp -d "${TMPDIR:-/tmp}/hstex-release.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM

prefix=$stage/$name
mkdir -p "$prefix/bin" "$prefix/share/doc/hstex" "$prefix/share/man/man1"
install -m 0755 "$build/hstex" "$prefix/bin/hstex"
install -m 0755 "$build/hstex-pdflatex" "$prefix/bin/hstex-pdflatex"
install -m 0644 "$root/LICENSE" "$prefix/share/doc/hstex/LICENSE"
install -m 0644 "$root/NOTICE" "$root/THIRD_PARTY_NOTICES.md" \
    "$prefix/share/doc/hstex/"
install -m 0644 "$root/README.md" "$root/CLEANROOM.md" \
    "$root/docs/COMPATIBILITY.md" "$root/docs/RELEASING.md" \
    "$prefix/share/doc/hstex/"
install -m 0644 "$root/docs/hstex-pdflatex.1" "$prefix/share/man/man1/"

epoch=$(git -C "$root" show -s --format=%ct HEAD)
tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
    -C "$stage" -czf "$archive" "$name"

debian_root=$stage/debian
mkdir -p "$debian_root/DEBIAN" "$debian_root/usr"
cp -a "$prefix/bin" "$prefix/share" "$debian_root/usr/"
install -D -m 0644 "$root/packaging/debian/copyright" \
    "$debian_root/usr/share/doc/hstex/copyright"
sed "s/@VERSION@/$version/g" "$root/packaging/debian/control.in" \
    >"$debian_root/DEBIAN/control"
dpkg-deb --build --root-owner-group "$debian_root" "$debian" >/dev/null

(
    cd "$output"
    sha256sum "$(basename "$archive")" "$(basename "$debian")" >SHA256SUMS
)

echo "wrote $archive"
echo "wrote $debian"
echo "wrote $checksums"
