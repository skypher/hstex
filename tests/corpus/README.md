# The public document corpus

A collection of freely distributable TeX documents, typeset by both the
reference engine and HSTeX and compared on what the reference says about the
document.

Run it:

```sh
tests/corpus/run-corpus.sh              # report the state of the corpus
tests/corpus/run-corpus.sh --strict     # exit nonzero if anything disagrees
tests/corpus/run-corpus.sh --stress --strict  # gate adversarial/interactive inputs
```

The documents are listed in `documents.tsv` and fetched from CTAN on first
run, each pinned by SHA-256, into `build/corpus` (override with `CORPUS_WORK`).
They are test input, not engine source, so they are fetched rather than
vendored; the comparison run uses only the reference engine's observable
outputs. See `SOURCE_POLICY.md`.

The release corpus contains ordinary documents HSTeX currently matches.
`stress-documents.tsv` contains hostile inputs that have exposed named
compatibility work. It uses `build/corpus-stress` and reports every
disagreement without failing by default; CI runs both manifests with
`--strict`.

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

| Document | Format | What it targets | Current result |
|---|---|---|---|
| `clsguide-historic` | LaTeX | A 35-page class/package writer guide and nested `tabular` | All comparison gates agree across 35 pages |
| `anc-test.ltx` | LaTeX | Fifteen pages of Ancient Greek transliteration, accents, and expected hyphen breaks | All comparison gates agree across 15 pages, including the two expected Babel faults |
| `encguide` | LaTeX | Font encodings, unusual alphabets, large tables, and error recovery | All comparison gates agree across 29 pages |
| `grfguide` | LaTeX | Color, graphics, file creation, EPS inclusion, and driver errors | All comparison gates agree across 17 pages, including EPS conversion fallback and expected driver errors |
| `testpage` | LaTeX | Interactive `\typein`, printer geometry, and a two-sided branch | All comparison gates agree on the scripted one-page branch |
| `testfont` | plain | Terminal `\read`, dynamic font selection, and a large glyph exercise | All comparison gates agree on the one-page DVI |

The stress suite feeds both engines the same answer profile defined by the
runner. This makes interactive behavior reproducible in CI without editing
the fetched documents. A new disagreement can be inspected without
`--strict`; the CI invocation treats any disagreement, fetch failure,
format-build failure, or harness failure as a failed gate.

## What is compared

For each document, both engines are run over the same file. The gate compares:

- the page count;
- every box that did not fit, with its kind, amount, badness and lines; and
- every fault reported;
- generated cross-reference and navigation state;
- page boxes and rotation;
- line and page breaks, glyph identities and positions, and normalized text;
- destinations, links, and bookmarks; and
- rendered pages at fixed settings.

For a plain document, the output itself is compared as well, byte for byte.
Both engines are given `\time`, `\day`, `\month` and `\year`, because a
document that prints the date would otherwise differ by the time of day and
nothing else, and the reference is asked for DVI so that there is no PDF
identifier or timestamp in the way. `story` and `gentle` both come out
identical to the reference's -- 680 and 263,424 bytes.

A LaTeX PDF is compared by `compare-pdf.py`, which uses `mutool`, `pdfinfo`,
and `pdftotext` to compare document semantics without comparing identifiers,
timestamps, compression, object numbers, xref layout, or font subset prefixes.
MuPDF renders both sides at 144 dpi with identical RGB and antialiasing
settings. The fixed glyph-coordinate tolerance is 0.01 PDF points. Generated
`.aux`, `.toc`, `.out`, and other cross-pass state files are compared exactly.

The reference's summary statistics count its own string pool, `mem` array,
hash and font tables. Those are properties of that implementation, not of the
document, so they are not semantic comparison targets.

LaTeX documents are run for a single pass on both sides, so cross-references
resolve to the same degree in each. A document whose references need a second
pass reports the same unresolved state in both logs.

The LaTeX comparison requires MuPDF's command-line tools and Poppler's
`pdfinfo` and `pdftotext`. On Debian and Ubuntu these are provided by
`mupdf-tools` and `poppler-utils`.

## Adding a document

Append a row to `documents.tsv` for a release-gating document or to
`stress-documents.tsv` for an adversarial finding. Give its CTAN path and
SHA-256; the path's `.tex` or `.ltx` suffix is preserved. The optional sixth
column selects a scripted stdin profile for interactive tests. A document
earns its place by exercising something the corpus does not already reach, and
by being redistributable and self-contained enough to typeset with the TeX
Live packages installed by CI.
