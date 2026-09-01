# The public document corpus

A collection of freely distributable TeX documents, typeset by both the
reference engine and HSTeX and compared on what the reference says about the
document.

Run it:

```sh
tests/corpus/run-corpus.sh              # report the state of the corpus
tests/corpus/run-corpus.sh --strict     # exit nonzero if anything disagrees
tests/corpus/run-corpus.sh --stress     # run adversarial/interactive inputs
```

The documents are listed in `documents.tsv` and fetched from CTAN on first
run, each pinned by SHA-256, into `build/corpus` (override with `CORPUS_WORK`).
They are test input, not engine source, so they are fetched rather than
vendored; the reference engine is run only as a black-box oracle. See
`CLEANROOM.md`.

The release corpus contains documents HSTeX currently matches and is run with
`--strict` in CI. `stress-documents.tsv` contains hostile inputs that expose
named compatibility work. It uses `build/corpus-stress`, reports every
disagreement without failing by default, and can be made a gate with
`--stress --strict` as those findings are fixed.

## The documents

| Document | Format | Size | What it exercises |
|---|---|---|---|
| `story` | plain | 1 page | Knuth's canonical sample: accents, ligatures, `\centerline`, `\vskip` |
| `gentle` | plain | 97 pages | Doob, *A Gentle Introduction to TeX*: a whole plain TeX book, with its own macros, index, footnotes and tables |
| `small2e` | LaTeX | 1 page | LaTeX's minimal sample |
| `sample2e` | LaTeX | 3 pages | LaTeX's feature sample: sectioning, cross-references, displayed math, `tabular`, `verbatim`, footnotes |
| `testmath` | LaTeX | 41 pages | The AMS `amsmath` test document: the hard cases of displayed mathematics |
| `ltx3info` | LaTeX | 7 pages | Dense macro-level typography in the LaTeX3 project history |
| `usrguide-historic` | LaTeX | 33 pages | A full historic author guide with indexes, code examples, and cross-references |
| `cfgguide` | LaTeX | 11 pages | Configuration hooks, verbatim code, and font commands |
| `cyrguide` | LaTeX | 7 pages | Cyrillic encodings and input examples |
| `modguide` | LaTeX | 7 pages | Internal command examples and modification hooks |
| `subeqn` | LaTeX | 2 pages | AMS nested equation numbering, tags, and indirect cross-references |
| `technote` | LaTeX | 4 pages | The AMS documented-source class, short verbatim, and math internals |
| `tools-overview` | LaTeX | 2 pages | `calc`, `hyperref`, PDF strings, tables, and the LaTeX tools bundle |
| `ltxcheck` | LaTeX | 0 pages | 64 scripted terminal returns drive LaTeX's interactive installation and file-lookup diagnostic |

The LaTeX Project files are distributed under the LaTeX Project Public
License, the two AMS files permit unchanged copying, `story` ships with
Knuth's TeX distribution, and `gentle` states that it may be distributed.
None is engine source, and none is read for anything but its own text.

## The adversarial stress documents

| Document | Format | What it targets | Current differential finding |
|---|---|---|---|
| `clsguide-historic` | LaTeX | A 35-page class/package writer guide and nested `tabular` | HSTeX emits four brace faults absent from the reference |
| `anc-test.ltx` | LaTeX | Fifteen pages of Ancient Greek transliteration, accents, and expected hyphen breaks | Page and box counts match; HSTeX emits 15 faults to the reference's 2 |
| `encguide` | LaTeX | Font encodings, unusual alphabets, large tables, and error recovery | HSTeX stops at a vertical-list `\cr` after 27 of the reference's 29 pages |
| `grfguide` | LaTeX | Color, graphics, file creation, EPS inclusion, and driver errors | Page and box counts match; the fault sets do not |
| `testpage` | LaTeX | Interactive `\typein`, printer geometry, and a two-sided branch | Scripted `letterpaper`/`n` answers produce one reference page and two HSTeX pages |
| `testfont` | plain | Terminal `\read`, dynamic font selection, and a large glyph exercise | The reference emits a one-page DVI; HSTeX stops at the terminal-read boundary |

The stress suite feeds both engines the same answer profile defined by the
runner. This makes interactive behavior reproducible in CI without editing
the fetched documents. Expected document disagreements are findings; fetch,
format-build, or harness failures still fail the command.

## What is compared

For each document, both engines are run over the same file and the logs are
compared on:

- the page count;
- every box that did not fit, with its kind, amount, badness and lines; and
- every fault reported.

For a plain document, the output itself is compared as well, byte for byte.
Both engines are given `\time`, `\day`, `\month` and `\year`, because a
document that prints the date would otherwise differ by the time of day and
nothing else, and the reference is asked for DVI so that there is no PDF
identifier or timestamp in the way. `story` and `gentle` both come out
identical to the reference's -- 680 and 263,424 bytes.

A LaTeX document is not compared this way: its PDF carries identifiers and
timestamps of its own.

The reference's summary statistics count its own string pool, `mem` array,
hash and font tables. Those are properties of that program, not of the
document, and reproducing them would mean copying its data structures, which
`CLEANROOM.md` forbids. They are not compared; see `docs/DECISIONS.md`,
`what-a-clean-room-engine-cannot-reproduce`.

LaTeX documents are run for a single pass on both sides, so cross-references
resolve to the same degree in each. A document whose references need a second
pass reports the same unresolved state in both logs.

## Adding a document

Append a row to `documents.tsv` for a release-gating document or to
`stress-documents.tsv` for an adversarial finding. Give its CTAN path and
SHA-256; the path's `.tex` or `.ltx` suffix is preserved. The optional sixth
column selects a scripted stdin profile for interactive tests. A document
earns its place by exercising something the corpus does not already reach, and
by being redistributable and self-contained enough to typeset with the TeX
Live packages installed by CI.
