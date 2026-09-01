# HSTeX

HSTeX is an independent TeX engine written in C17 for low-latency,
pdfTeX-compatible typesetting on modern CPUs.

The first end-to-end milestone is semantic agreement with pdfTeX on the
digest-pinned public document corpus, together with at least a 5× reduction in
median end-to-end wall time. HSTeX is under active development; compatibility
is defined by its tested surface, not by a claim that every TeX document is
already supported.

## Status

| Area | Current state |
| --- | --- |
| Engine | Independent C17 implementation; no pdfTeX fallback |
| User command | `hstex-pdflatex`, using an installed TeX Live or MacTeX tree |
| Strict corpus | 14/14 digest-pinned documents agree with the reference gates; 216 pages and 13,275 source lines |
| Adversarial corpus | Six pinned stress cases exercise hostile inputs; 4/6 currently agree and the suite runs non-strictly in CI |
| Compatibility gate | Ordinary tests, the canonical two-pass Trip comparison, and the strict public corpus |
| Performance target | At least 5× lower median end-to-end wall time under the published benchmark contract |
| First release target | Linux x86-64 |

The project will publish a headline speedup only after measuring it under the
full benchmark contract.

## Requirements

Building HSTeX requires:

- a C17 compiler;
- Meson 1.3 or newer and Ninja;
- zlib development headers; and
- TeX Live or MacTeX for LaTeX inputs, fonts, metrics, encodings, maps,
  `kpsewhich`, and reference comparisons.

The CI LaTeX environment installs `texlive-latex-base`,
`texlive-latex-recommended`, and `texlive-fonts-recommended`. Some tests also
require `curl`, `pdftex`, `pltotf`, and `tftopl`.

## Build

```sh
meson setup build
meson compile -C build
meson test -C build --no-rebuild --print-errorlogs
```

Meson enables C17, warnings as errors, and a debug-optimized build by default.

## Use

`hstex-pdflatex` is the supported LaTeX entry point:

```sh
./build/hstex-pdflatex report.tex
./build/hstex-pdflatex \
  -output-directory=build/output \
  -jobname=report \
  report.tex
```

The driver builds and reuses HSTeX's native `pdflatex.hfmt` cache. It reads
the installed TeX tree through public file-lookup data and `kpsewhich`; it
never reads a pdfTeX format dump. Use `--format-cache=DIR` to select a cache
root or `--rebuild-format` after a local format-input change.

Run the driver with `--help` for the accepted pdfLaTeX options. Unsupported
options are rejected instead of silently ignored, and HSTeX never invokes
pdfTeX as a fallback. The complete command-line contract is in
[`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md).

## Correctness tests

The normal Meson suite exercises the tokenizer, input stack, catcodes,
symbol table, mouth, file database, engine, and driver.

The canonical Trip comparison fetches digest-pinned inputs and compares HSTeX
with pdfTeX behavior:

```sh
tests/trip/run-trip.sh ./build/hstex
```

The strict release corpus fetches each public document from CTAN, verifies its
SHA-256 digest, runs the same input through the reference and HSTeX, and checks
the semantic gates:

```sh
tests/corpus/run-corpus.sh --strict ./build/hstex
```

The adversarial suite deliberately includes hostile and interactive documents.
It records remaining findings without failing CI:

```sh
tests/corpus/run-corpus.sh --stress ./build/hstex
```

Use `--stress --strict` when checking whether every tracked incompatibility
has been closed. Corpus identity, licenses, stdin profiles, and comparison
details are documented in
[`tests/corpus/README.md`](tests/corpus/README.md).

## Implemented surface

The engine currently includes:

- catcode-aware tokenization, macro expansion, grouping, assignments,
  registers, conditionals, file streams, and recoverable diagnostics;
- scaled dimensions, finite and infinite-order glue, boxes, alignments,
  paragraph breaking, hyphenation, ligatures, kerning, and page building;
- inline and display math, scripts, fractions, radicals, delimiters,
  equation numbers, and the math layouts exercised by the corpus;
- direct DVI and PDF emission, including links, destinations, annotations,
  outlines, color stacks, origin-relative transformations, literals,
  compression, and Type 1 subsetting;
- TFM, Type 1, PK, encoding, and map-file handling;
- versioned native format caches with explicit invalidation keys; and
- scalar and runtime-dispatched AVX2 lexical scanning with identical semantic
  fallbacks.

Expansion, assignment, paragraph construction, and page building remain
ordered. Work is parallelized only after pages or resources become immutable,
and PDF object assignment remains deterministic. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the design.

## Current limits

- The supported semantic surface is the one exercised by the release tests
  and strict corpus. The stress manifest records adversarial coverage and
  remaining gaps.
- The first release target is Linux x86-64. CI also compiles selected Linux
  Arm, macOS, FreeBSD, and NetBSD configurations.
- Image inclusion is outside the current strict corpus surface.
- The native-format smoke gate uses Ubuntu 24.04's TeX Live 2023. Ubuntu
  22.04's TeX Live 2021 format is rejected because its `latex.ltx` does not
  complete the expected format-building transition.
- Only the options listed in `hstex-pdflatex --help` are accepted.

## Benchmarking

Run the corpus timing mode with:

```sh
tests/corpus/run-corpus.sh --time ./build/hstex
```

Release numbers use the median of seven warm-filesystem-cache runs in fresh
output directories. They record document and executable identities, compiler
and flags, CPU model and affinity, worker count, machine load, peak RSS, and
every individual wall time. Final-pass and fresh three-pass-plus-BibTeX
results are separate, as are ordinary and persistent-process results.

The authoritative procedure is
[`docs/BENCHMARK_CONTRACT.md`](docs/BENCHMARK_CONTRACT.md).

## Source use and provenance

Public TeX-engine implementations may be consulted for behavior, algorithms,
and edge cases. Incompatibly licensed code must not be pasted or mechanically
translated into HSTeX, and production HSTeX never invokes pdfTeX as a fallback.
TeX Live macro, font, metric, encoding, and map files remain external input
data.

Read [`SOURCE_POLICY.md`](SOURCE_POLICY.md) before contributing. Non-obvious
compatibility choices must identify the specification, controlled experiment,
or exact source version and location in
[`docs/DECISIONS.md`](docs/DECISIONS.md).

## Documentation

- [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md) — supported driver and
  compatibility contract
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — execution and data design
- [`docs/BENCHMARK_CONTRACT.md`](docs/BENCHMARK_CONTRACT.md) — correctness and
  timing rules
- [`docs/RELEASING.md`](docs/RELEASING.md) — release procedure
- [`NOTICE`](NOTICE) and
  [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) — attribution and
  bundled-material records

## License

HSTeX is licensed under the Apache License 2.0. See [`LICENSE`](LICENSE).
