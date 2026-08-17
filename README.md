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
parameters support copying, grouping, `the`, and expansion-safe insertion.
Dimension units are matched as backtracking keywords and converted with the
reference engine's scaled-point arithmetic, and TeX's page state and `\end` are
in place, so the engine now runs a `\documentclass{article}` document from
`\begin{document}` to `\end{document}` against the installed `latex.ltx`:

```sh
./build/hstex --run-latex "$(kpsewhich latex.ltx)" document.tex
```

The format is built from `pdftexconfig.tex` and the given source, the way
`pdflatex.ini` does, and the engine reports pdfTeX's version, so `expl3`
selects its pdfTeX backend. The resulting message stream is a subsequence of
the `pdflatex` log for the same document; what is missing is the file-open
notation and everything downstream of the page builder.

The benchmark corpus loads its full package stack — `geometry`, `amsmath`,
`amssymb`, `mathtools`, `microtype`, `hyperref`, `xr`, `cleveref` and their
73-file dependency graph — on the same command:

```sh
./build/hstex --run-latex "$(kpsewhich latex.ltx)" tests/corpus/document.tex
```

That needed pdfTeX's regular-expression and string-escape primitives and the
font identifier reported by `\the\font`.

Typesetting has started. Characters carry the font's ligature and kerning
program, interword glue follows the space factor, and a horizontal command
met in vertical mode begins an indented paragraph the way the reference does:
the token is put back so that `\everypar` runs before the command scans its
own operands. Box bodies are executed on the live input and end when the group
they opened ends, so a box may be opened by one macro and closed by an
`\egroup` another produces — the shape LaTeX's colour, minipage and parbox
commands are built on. The corpus now runs through `\maketitle` and into the
abstract, and stops at `\halign`.

Inline formulas are typeset. `$...$` builds a math list, the atom classes
from the mathcodes decide the spacing between them, math families come from
`\textfont` and `\fam`, and the characters carry their italic corrections
and their family's ligature program. Superscripts and subscripts are set in
the script and scriptscript styles with the reference's shift arithmetic,
which is where `\scriptfont` and `\scriptscriptfont` come in.

`\halign` builds alignments: the preamble's templates, `\tabskip` glue at
every boundary, `\omit`, `\span`, `\noalign`, and the `&&` repeat. That is
what `\begin{tabular}` needs, so the corpus now runs through `\maketitle`,
the abstract and the table of contents and into its first chapter.

Display math is set too: `$$...$$` breaks the paragraph so far, centres the
equation in `\displaywidth`, and chooses the short or long display skips
from how far the line above reaches.

Paragraphs are broken into lines by the reference's optimal-fit method:
badness and fitness per line, demerits over the whole paragraph, and the
three passes `\pretolerance` and `\tolerance` select between.

The page builder, the output routine, and PDF emission remain under
construction, and there is no hyphenation yet.

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
