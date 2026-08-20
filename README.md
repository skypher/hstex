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
how far back the reference can still reach to rewrite one. A PDF is written when `\pdfoutput` is positive: the pages
and their streams of text, rules, colour and literals; the links,
annotations and destinations they carry, with the rectangles each covers and
the sorted tree of names the catalog points at; the tree of pages; and the
measurements of every font, named and ordered the way the reference names and
orders them; the outline the document builds with `\pdfoutline`, the action
the file opens on, and what `\pdfinfo` and `\pdfcatalog` say about it. The
whole corpus is now that file: all 2,375 pages and 49,786,244 bytes of it,
byte for byte what `pdflatex` writes for the same source with the fonts left
uncarried and nothing compressed -- every glyph placed, every correction
counted, every rule, link, destination, bookmark, font measurement and
cross-reference entry in the same order and to the same scaled point. The
fonts themselves are still not carried in the file, because subsetting a
Type 1 font is to come, and nothing is compressed. Marks and insertions are both there, splitting and
all: an insertion that will not fit is broken where `\vsplit` would break
it, what fits goes into the box of its class, and the rest waits for the next
page with `\splittopskip` in front of it. Every file the corpus writes
beside its pages is identical to the reference's: the table of contents, the
bookmark file, and the `.aux` with all 23,372 of its `\newlabel` lines --
every page number, section number and cross-reference in a 2,375-page
document, and every `\citation` in the order the reference wrote it.

The engine is now faster than the reference on the corpus, and no longer
extravagant with memory. Its final pass -- auxiliary inputs in place, format
read from a file, nothing compressed on either side -- takes 16.6 processor
seconds against `pdflatex`'s 24.8 for the same source, and peaks at 128 MB
against 47. A fresh `hstex` -> BibTeX -> `hstex` -> `hstex` build of the
whole thing takes 52.6 seconds against `pdflatex`'s 75.5. Every run above is
the least of six, taken alternately on one pinned processor.

The first round of tuning took the final pass from 72 seconds to 28: reading
the format from a file rather than executing `latex.ltx` again at every run;
asking `kpsewhich` where a file is once for each name rather than once for
each mention; sorting the 23,513 destination names once rather than
comparing every pair of them; taking a macro's arguments in runs rather than
a token at a time; and keeping the room a macro call needs rather than
taking and giving it back at every one of them.

The second round took 30 to 16.7, in order of what it was worth: building
the engine from a profile of what it does, at -O3 and linked in one piece
(19%); naming the primitive a failed scan was inside only where one fails,
rather than writing out the name of every one of the corpus's 25.6 million
(6%); keeping where the input is reading beside the stack rather than
finding it again for each of 388 million tokens, and popping a frame that
has run out where it stands (5.5%); reading a control sequence's name and an
expanded body where they stand, which between them are half of everything
the corpus expands (5% and 3.5%); starting one `kpsewhich` for the whole run
and asking it over a pipe, rather than starting one for each of 181 names at
twelve milliseconds each (5%); handing the PDF file a megabyte at a time
rather than 594,747 pieces of eighty-four bytes, and passing over a skipped
conditional where it stands (3%); holding what the input is reading in
forty-eight bytes rather than a hundred and forty-four (3%); counting what a
macro's body is made of when it is defined rather than at each of its calls,
which also showed that fifty-seven per cent of calls copy nothing at all
(1.5%); finding a font's place in the file by its number rather than by comparing
forty-six million names (0.7%); and a rewrite of the whole macro-call path
-- the arguments into one arena rather than nine vectors, the parameter text
of an ordinary macro not read at all, a brace told from a character in one
comparison, a short body copied rather than held (1.5% for all of it, which
is the interesting part; see below).

Memory went from 1.26 GB to 161 MB in the same round, and to 128 MB once a
definition stopped asking for four times the tokens it holds. The room those
bodies are kept in now comes from free lists of the engine's own -- one for
each power of two -- rather than from the allocator: a block is handed on
from the vector that read it to the macro record or input frame that keeps
it, and given back from there by length alone, which took the run from 29.5
million calls on the allocator to 8.2 million and 3.3 per cent off the time.
See docs/DECISIONS.md, where-a-body-is-kept. Nodes and the lists
that hold them were made and never unmade, and most of what that kept was
not the pages but the sub-formulas -- kept for the lifetime of the run, with
every box each had been set as, although nothing outside the formula it
belongs to can name one. What is left is now walked from the places that can
still name a node and the rest given back, between one page and the next.
Of the 128 MB, 72 is what reading the format costs before a document starts.

The milestone wants five times `pdflatex`, not one and a half, and that is
open. Asking the reference how much work there is to do settles where the
rest cannot come from: over six controlled probes the engine expands between
one and four per cent *fewer* macros than `pdflatex` does, so the two do the
same work and what separates them is the cost of each expansion rather than
the number of them. There is no redundant expansion left to find. Two thirds
of the remaining time is the expansion machinery, so five times the reference
would mean running that machinery about three and a half times faster than it
runs -- some twenty cycles for each token read, counting the macro call that
one token in twelve begins.

The macro-call path was then rewritten around what the cycle counter says it
is made of -- sixty per cent scanning the arguments, twenty copying the body,
fifteen pushing the frame -- and the whole rewrite came to one and a half per
cent. That is the useful result: the remaining factor is not hiding in the
macro call, or anywhere else in particular. It is spread across a hundred
places at one or two per cent each, and a good deal of it is not instructions
at all: `end_group` spends four hundred cycles restoring six meanings because
each is a walk into a megabyte that no cache holds. See docs/DECISIONS.md,
how-much-work-there-is-to-do.

The other place a factor could come from is more than one processor, and
what that is worth has now been measured rather than guessed. Breaking
paragraphs, shipping pages and giving back what a page leaves behind come to
an eighth of the run between them; everything else is the thread that reads
the input, so taking the back end off it wins at most 1.14 times. Running
the chapters at once, each seeded from a checkpoint and checked afterwards,
wins at most 3.9 times, because the longest single `\input` is a quarter of
the run and no `\input` boundary divides it. Both of them perfect come to
about 4.4 times, so five is close and ten is out of reach at that
granularity. A checkpoint the engine can take *inside* a file is what would
change that, and the sequential core -- seven eighths of the run, and what
every one of those processors would be running -- is what matters either
way. See docs/DECISIONS.md, what-could-leave-the-critical-path.

That checkpoint now exists. Where the driver's loop is entered from outside
rather than from inside itself and a page has just shipped, nothing is half
built in a call of its own -- which is why what a page leaves behind is
given back exactly there -- so a run is entirely in the engine record and
the files it has open, and `fork` copies both. `HSTEX_CHECKPOINT` names the
page to stop at, or `every:N` a stride. Taken up after its first page, in
the middle of its longest chapter, after its last page, and twenty-three
times over, the corpus writes the same PDF, `.aux`, `.toc` and `.out` byte
for byte, and a hundred handoffs cost about a second of the twenty.

Reading the clock at all 2,364 of those boundaries says what the boundary
being a page rather than a file is worth. Of 23.6 seconds, 22.7 lie between
the first page and the last, and dividing those into equal-cost chunks gives
10.0 times on sixteen workers, 18.1 on sixty-four, and a ceiling of 24 --
against 3.9 at file granularity. So the milestone's second threshold is
reachable, and it takes sixteen workers rather than a faster expansion
machinery. What it still takes is a guess at the state each chunk begins in
and a check that the guess held: a checkpoint says where a run may be taken
up, not what the state there will be before the run has reached it.

The division has now been done rather than only costed. `HSTEX_PARALLEL=100`
parks a chunk at every hundredth page, runs the document to the end, and
then opens a gate they are all waiting on; each takes the run up where it
was parked, runs as far as the next boundary, and stops. Nothing is joined
together afterwards, because a chunk inherits the count of bytes the run had
written when it was parked, which is exactly where the chunk before it
stops, so each opens the file for itself and seeks there. Twenty-three
chunks write the corpus's 49,786,244 bytes between them in 1.94 seconds
against 24.22 on one processor, and what they write is byte for byte the
PDF the same engine writes alone -- as are the `.aux`, the `.toc` and the
`.out`. Fifteen chunks give 10.4 times; the flattening after twenty-three
is the machine.

A chunk leaves a copy of itself parked before it does any work, so the fleet
stands rather than being spent: let go five times over, twenty-three chunks
take 2.087, 2.130, 1.945, 1.879 and 2.030 seconds against 25.39 on one
processor -- a median of **12.5 times** -- and after all five the four files
are still byte for byte right.

That is the taking up and the writing, and it is not a cold run made
faster: standing the fleet up costs a sequential run of the same document,
so what is measured is what a run costs once the fleet is there, which is
the persistent mode the contract reports on its own. The fleet now serves
the edit loop, on one switch: `HSTEX_FLEET=<dir>` makes a run with no fleet
park one as it goes, and a run that finds one get served from it -- the aux
delta is patched in automatically, chunks read the disk as it stands, edits
and all, and every released chunk leaves a successor parked so the fleet
outlives the round. A four-round editing session on the corpus: 20.6
seconds cold, then 2.6, 2.6 and 2.8 after three successive edits, every
round byte for byte -- with the state digest fully strict, no scratch-name
waiver anywhere. The waiver earlier experiments leaned on was covering for
digest defects since fixed, and a check that watches for waived names being
read before written now polices any experimental use of one. See
docs/DECISIONS.md, the-relay and the-waiver-checked-and-then-retired.

The guessing now works too. A fleet parked by one pass can serve the next:
each woken chunk reads a patch -- the labels the next pass will see
differently -- and typesets its range on that guess into fleet copies of
every file, while a verifier walks the document from the front and at each
parked page compares its state, by content, with the chunk's patched
beginning. Equal means everything from there stands and the verifier stops;
unequal means the chunk was wrong, and the truth is relayed forward by one
carrier at a time rewriting what the guesses got wrong. Under a real
70-label delta between the corpus's third and fourth passes, ten of
twenty-three chunks validate and pass 4 comes out in 13.9 seconds against
about 20 -- byte for byte, all four files. Across the pipeline's expensive
seam (citations resolving between passes 2 and 3) nothing validates and the
relay degrades to a plain sequential run, unhurt. See docs/DECISIONS.md,
the-relay, the-guess-and-what-it-is-worth.

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

A build to measure with is built from a profile of the engine's own work,
which `tools/build-pgo.sh` takes in one command. The profile comes from
building the format and from `benchmarks/training/train.tex`, never from the
milestone corpus:

```sh
tools/build-pgo.sh
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
