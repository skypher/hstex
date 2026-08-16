# HSTeX

HSTeX is a clean-room TeX engine written in C17 for low-latency pdfTeX-compatible
typesetting on modern CPUs.

The first end-to-end target is the pinned `document.tex` corpus in
`tests/corpus`. Success requires semantically correct PDF and auxiliary
outputs together with at least a 5× median wall-clock speedup over pdfTeX on a
fresh TeX → BibTeX → TeX → TeX build.

## Current status

The repository contains the clean-room contract, benchmark snapshot and oracle
runner, plus the first engine substrate: regular-file loading, packed tokens,
mutable catcodes, stable control-sequence interning, a line-aware TeX mouth,
nested file/token sources, and a runtime-dispatched scalar/AVX2 lexical-boundary
scanner. The expansion core supports ordinary and delimited macros, local and
global definitions, `let`, definition prefixes, `expandafter`, and `noexpand`.
It does not yet execute the assignment and typesetting primitives required to
load LaTeX.

## Build

```sh
meson setup build
meson compile -C build
meson test -C build --no-rebuild
```

Inspect the selected scanner and probe an input file:

```sh
./build/hstex --cpu-features
./build/hstex --probe-input tests/corpus/document.tex
./build/hstex --mouth-stats-latex tests/corpus/document.tex
```

Generate a fresh pdfTeX oracle build in an ignored output directory:

```sh
tests/corpus/run_pdftex_oracle.sh
```

See `CLEANROOM.md`, `docs/ARCHITECTURE.md`, and
`docs/BENCHMARK_CONTRACT.md` before changing semantics or performance-critical
representations.
