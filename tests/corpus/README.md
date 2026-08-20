# The public document corpus

A collection of freely distributable TeX documents, typeset by both the
reference engine and HSTeX and compared on what the reference says about the
document.

Run it:

```sh
tests/corpus/run-corpus.sh              # report the state of the corpus
tests/corpus/run-corpus.sh --strict     # exit nonzero if anything disagrees
```

The documents are listed in `documents.tsv` and fetched from CTAN on first
run, each pinned by SHA-256, into `build/corpus` (override with `CORPUS_WORK`).
They are test input, not engine source, so they are fetched rather than
vendored; the reference engine is run only as a black-box oracle. See
`CLEANROOM.md`.

## The documents

| Document | Format | Size | What it exercises |
|---|---|---|---|
| `story` | plain | 1 page | Knuth's canonical sample: accents, ligatures, `\centerline`, `\vskip` |
| `gentle` | plain | 97 pages | Doob, *A Gentle Introduction to TeX*: a whole plain TeX book, with its own macros, index, footnotes and tables |
| `small2e` | LaTeX | 1 page | LaTeX's minimal sample |
| `sample2e` | LaTeX | 3 pages | LaTeX's feature sample: sectioning, cross-references, displayed math, `tabular`, `verbatim`, footnotes |
| `testmath` | LaTeX | 41 pages | The AMS `amsmath` test document: the hard cases of displayed mathematics |

All five permit redistribution: `story` ships with Knuth's TeX distribution,
`gentle` states that you should feel free to distribute it, and `small2e`,
`sample2e` and `testmath` are under the LaTeX Project Public License. None of
them is engine source, and none is read for anything but its own text.

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

Append a row to `documents.tsv` with its CTAN path and SHA-256. A document
earns its place by exercising something the corpus does not already reach, and
by being redistributable and self-contained enough to typeset with a stock TeX
Live installation.
