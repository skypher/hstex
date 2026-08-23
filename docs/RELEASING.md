# Release procedure

## Preconditions

1. The source is Apache-2.0. Release assets carry `LICENSE`, `NOTICE`, and
   `THIRD_PARTY_NOTICES.md`; the packager refuses to run if any is absent.
2. Update the Meson project version and create an annotated matching tag,
   `vVERSION`.
3. Run the full engine suite, the canonical TRIP gate, and the public corpus.
4. Record the release benchmark according to `docs/BENCHMARK_CONTRACT.md`.

## Automated release

Pushing a matching `vVERSION` tag starts `.github/workflows/release.yml`. It
builds statically linked Linux x86-64 `hstex` and `hstex-pdflatex` binaries,
runs the test suite and TRIP, creates a tarball and Debian package, emits
checksums and an SPDX SBOM, publishes the GitHub Release, and attaches GitHub
build/SBOM provenance to the primary tarball.

Consumers can verify the primary release asset with:

```sh
gh attestation verify hstex-VERSION-linux-x86_64.tar.gz -R skypher/hstex
```

## Local package rehearsal

```sh
tools/build-static.sh --musl build-release
tools/package-release.sh build-release dist
```

`tools/package-release.sh` never chooses a version or license. It obtains the
version from the built binary, requires a committed `LICENSE`, and fails when
the destination already contains a release asset of that version.
