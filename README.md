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
the `pdflatex` log for the same document, printed through the same
seventy-nine-column printer -- a `\message` keeps its distance from what is
already on the line, a `\showbox` starts one of its own, a shipped page
announces itself by its counts -- and what is missing is the file-open
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
commands are built on.

Inline formulas are typeset. `$...$` builds a math list, the atom classes
from the mathcodes decide the spacing between them, math families come from
`\textfont` and `\fam`, and the characters carry their italic corrections
and their family's ligature program. Superscripts and subscripts are set in
the script and scriptscript styles with the reference's shift arithmetic,
which is where `\scriptfont` and `\scriptscriptfont` come in.

`\halign` builds alignments: the preamble's templates, `\tabskip` glue at
every boundary, `\omit`, `\span`, `\noalign`, `\everycr`, and the `&&`
repeat, in a vertical list or as the whole of a display. That is what
`\begin{tabular}` needs and what amsmath's `align`, `gather` and `multline`
are built on.

Display math is set too: `$$...$$` breaks the paragraph so far, centres the
equation in `\displaywidth`, and chooses the short or long display skips
from how far the line above reaches. `\eqno` and `\leqno` put a number
beside it or below it.

The rest of the math builders are in: `\over` and its five relatives,
`\radical`, `\overline`, `\underline`, `\left`, `\right`, `\middle`,
`\vcenter`, `\mathchoice` and `\nonscript`. A sub-formula keeps its own
list as well as the box it was set as, so a fraction inside a fraction and a
`\mathchoice` inside either come out at the size the style they land in
asks for.

Paragraphs are broken into lines by the reference's optimal-fit method:
badness and fitness per line, demerits over the whole paragraph, and the
three passes `\pretolerance` and `\tolerance` select between. `\parshape`
and `\hangindent` shape them, and both are cleared where the reference
clears them.

The whole corpus now runs: all 217,376 lines of it, through every chapter,
on the command above.

The outermost vertical list is real: a character met there starts a
paragraph exactly as one inside a `\vbox` does, and `hstex_engine_run`
builds the list rather than handing the text back to the caller. The whole
corpus is typeset that way.

The page builder is there too. Material appended to the main vertical list
waits on a contribution list until a box, the end of a paragraph or a penalty
sets the builder going; it then moves what it can to the current page,
keeping `\pagetotal` and the rest, and sends the page off at the cheapest
break it has found. `\box255`, `\outputpenalty` and `\deadcycles` are set
the way the reference sets them, `\output` runs in a group of its own, and
`\vsplit` breaks a box the same way. LaTeX's own output routine runs on the
corpus.

Output commands that `\immediate` has not claimed become whatsits: a
`\write` keeps its text unexpanded until the page it sits on is shipped,
which is what lets LaTeX write a page number it does not yet know. The corpus
now writes an `.aux` file whose first eighteen entries are byte for byte
`pdflatex`'s.

Paragraphs are hyphenated. The pattern trie and the exception list were
already there; what runs them is a pass between the first and second attempts
at breaking, following the reference's rules about what counts as a word --
only after glue, letters of one font, `\uchyph` for a capital, ligatures read
as the letters they were made of, and nothing at all for a word an explicit
hyphen follows. `\discretionary` and `\-` are nodes the breaker treats as a
third kind of breakpoint, with `\hyphenpenalty`, `\exhyphenpenalty`,
`\brokenpenalty` and the two hyphen demerits.

Characters protrude past the margins where `\lpcode` and `\rpcode` say they
may, and the protrusion counts toward the line's width in the breaker once
`\pdfprotrudechars` has reached two -- which is what `microtype` asks for.
`\emergencystretch` buys the third pass. Formulas in a paragraph are fenced
with math nodes carrying `\mathsurround`, and a binary operator or a relation
leaves `\binoppenalty` or `\relpenalty` behind it.

Together these are enough for the whole corpus to match `pdflatex` node for
node: every page of it, dumped with `\showbox` as the output routine sees it
and shown nine levels deep, comes to 8,473,729 lines with the
cross-references resolved -- `hyperref`'s links and all -- and not one of
them differs.

Everything a line is made of is pinned the same way: protrusion kerns that
look inside boxes and are dropped again when the paragraph's last line is
measured, accent kerns rounded once rather than twice, `\pdfcolorstack` and
`\pdfdest` nodes, the `\finalhyphendemerits` charged at the end of a
paragraph, and the operator centring, limits, accents, superscript floors and
display marks a formula needs. `\tracingparagraphs` writes the passes out the
way the reference does, which is how the last few of those were found: over
the whole corpus the two engines' traces agree on 769,571 of 769,606 lines,
and the 35 that differ are feasible breaks recorded a breakpoint apart, none
of which changes a line of the output.

`\shipout` writes a page description when `\pdfoutput` is not positive, and
the whole corpus in that mode -- 2,375 pages and 22 megabytes of DVI -- is
byte for byte what `pdflatex` writes for the same source: places, fonts,
rules, leaders, set glue, the movement registers, the preamble and the
postamble, down to which of the two registers a repeated movement takes and
how far back the reference can still reach to rewrite one. A PDF is written when `\pdfoutput` is positive: the pages, their
streams of text and rules, the objects they need and the measurements of
every font, byte for byte the reference's for thirty-nine of forty randomly
nested probes -- but the fonts are not carried in the file yet and nothing is
compressed, so it is not yet the reference's file for a real document. Marks and insertions are both there, splitting and
all: an insertion that will not fit is broken where `\vsplit` would break
it, what fits goes into the box of its class, and the rest waits for the next
page with `\splittopskip` in front of it. Every file the corpus writes
beside its pages is identical to the reference's: the table of contents, the
bookmark file, and the `.aux` with all 23,372 of its `\newlabel` lines --
every page number, section number and cross-reference in a 2,375-page
document, and every `\citation` in the order the reference wrote it.

Speed is not there yet either. Loading `latex.ltx` and `amsmath` takes about
23 seconds, and the whole corpus about 74, against `pdflatex`'s 41 seconds
for the same source with a prebuilt format and an 11 MB PDF at the end. A
definition that nothing holds any more is now taken apart and its record used
again, so the corpus's 8.1 million definitions need 388,000 records rather
than 8.1 million, and the run peaks at 1.25 GB rather than 3.10 GB. Nothing
else has been tuned; correctness came first.

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
