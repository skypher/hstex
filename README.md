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
The executor supports mutable catcodes and integer/count state, character and
count definitions, nested integer and meaning conditionals, scoped restoration,
and nested file input. It currently bootstraps the installed `latex.ltx` through
its first included configuration file and date calculation, including scoped
integer arithmetic, serialization, expanded definitions, and conditionals
inside expansions. The bootstrap also has deterministic file-stream I/O, line
reads, EOF conditionals, messages, printable `string`/`meaning` expansion, and
the initial register allocators. Scaled dimensions, finite and infinite-order
glue, dimension/glue parameters, character code tables, and protected macros
are implemented with scoped restoration. Immutable token-list registers and
parameters support copying, grouping, `the`, and expansion-safe insertion;
dynamic control-sequence construction and typesetting remain under
construction.

## Build

The engine itself requires a C17 compiler, Meson, and Ninja. The engine tests
also load the standard `cmr10` and `line10` metrics through `kpsewhich`; on
Ubuntu, install `texlive-latex-base` to provide those test fonts and the lookup
tool.

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

See `CLEANROOM.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md`, and
`docs/BENCHMARK_CONTRACT.md` before changing semantics or performance-critical
representations.
