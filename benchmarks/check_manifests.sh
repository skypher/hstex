#!/usr/bin/env bash
# Verify the vendored benchmark input is byte-exact.
set -euo pipefail

benchmark_dir=$(cd "$(dirname "$0")" && pwd)
cd "$benchmark_dir"

sha256sum --check vendor-manifest.sha256
