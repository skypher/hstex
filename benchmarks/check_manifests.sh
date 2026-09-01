#!/usr/bin/env bash
# Verify the vendored benchmark input is byte-exact.
set -euo pipefail

usage() {
    printf '%s\n' 'Usage: benchmarks/check_manifests.sh'
    printf '%s\n' 'Verify the SHA-256 manifest for vendored benchmark inputs.'
}

case ${1:-} in
-h|--help)
    usage
    exit 0
    ;;
esac
if [ "$#" -ne 0 ]; then
    usage >&2
    exit 2
fi

benchmark_dir=$(cd "$(dirname "$0")" && pwd)
cd "$benchmark_dir"

sha256sum --check vendor-manifest.sha256
