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
| Strict corpus | 17/17 digest-pinned documents agree with the reference gates; 338 pages and 20,741 source lines |
| Adversarial corpus | All six pinned stress cases agree across 98 pages; CI runs the suite strictly |
| Compatibility gate | Ordinary tests, the canonical two-pass Trip comparison, both strict document corpora, and the corpus through `hstex-pdflatex` |
| Measured performance | 3.26× median and 2.26× aggregate lower wall time than pdfTeX over the release corpus, faster on every document, and lower peak RSS on every document ([`docs/BENCHMARK_RESULTS.md`](docs/BENCHMARK_RESULTS.md)) |
| Performance target | At least 5× lower median end-to-end wall time under the published benchmark contract; not met |
| First release target | Linux x86-64 |

The corpus has been measured under the full benchmark contract. Over the
seventeen release documents HSTeX is faster than pdfTeX on every one, by 3.26×
at the median and 2.26× summed over the corpus, and it uses less memory on
every one; an ordinary run starts no helper process at all. That is short of
the 5× milestone, and the per-document figures, the machine, the build, and
every individual run time are recorded in
[`docs/BENCHMARK_RESULTS.md`](docs/BENCHMARK_RESULTS.md).

## Requirements

Building HSTeX requires:

- a C17 compiler;
- Meson 1.3 or newer and Ninja;
- zlib development headers;
- optional libdeflate development headers for faster PDF stream compression; and
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
It uses libdeflate when the library is available and otherwise retains the
zlib implementation. Pass `-Dlibdeflate=disabled` to exercise the fallback or
`-Dlibdeflate=enabled` to require the faster backend.

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
CI treats any semantic disagreement as a failed gate:

```sh
tests/corpus/run-corpus.sh --stress --strict ./build/hstex
```

The same corpus is also run the way an installation runs it -- through
`hstex-pdflatex`, twice per document, with the checkpoint path the driver
takes by default:

```sh
tests/corpus/run-driver-corpus.sh --strict ./build/hstex-pdflatex ./build/hstex
```

Each document is held to `tests/corpus/driver-expectations.tsv`, which pins
any document currently disagreeing and says what comes out wrong in it; every
document agrees today, so it pins none. The gate fails whichever way a
document moves, so a repair is caught as surely as a regression.

CI runs the adversarial suite with `--strict`; omitting it is useful while
adding a newly pinned finding. Corpus identity, licenses, stdin profiles, and
comparison details are documented in
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
- public file metadata primitives and TeX Live's restricted shell-command
  profile, with allowlisted programs executed without a command shell;
- TFM, in-process PFB/PFA Type 1, PK, encoding, and map-file handling;
- versioned native format caches with explicit invalidation keys; and
- scalar and runtime-dispatched AVX2 lexical scanning with identical semantic
  fallbacks.

Expansion, assignment, paragraph construction, and page building remain
ordered. Work is parallelized only after pages or resources become immutable,
and PDF object assignment remains deterministic. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the design.

## Current limits

- The supported semantic surface is the one exercised by the release tests
  and the two strict corpora. The stress manifest records adversarial coverage.
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
[`docs/BENCHMARK_CONTRACT.md`](docs/BENCHMARK_CONTRACT.md), and the latest
measurement taken under it is
[`docs/BENCHMARK_RESULTS.md`](docs/BENCHMARK_RESULTS.md).

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
- [`docs/BENCHMARK_RESULTS.md`](docs/BENCHMARK_RESULTS.md) — the latest
  measurement taken under that contract
- [`docs/RELEASING.md`](docs/RELEASING.md) — release procedure
- [`NOTICE`](NOTICE) and
  [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) — attribution and
  bundled-material records

## License

HSTeX is licensed under the Apache License 2.0. See [`LICENSE`](LICENSE).
