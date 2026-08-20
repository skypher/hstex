# Benchmark contract

## Corpus identity

The corpus is the set of documents listed in `tests/corpus/documents.tsv`.
Each is a freely distributable public TeX document, fetched from CTAN and
pinned by SHA-256. Documents are test input, not engine source, so they are
fetched rather than vendored; `tests/corpus/README.md` records what each one
exercises and under what licence it is redistributable.

`tests/corpus/run-corpus.sh --fetch-only` checks the corpus against its pinned
digests and is the identity check CI runs.

Some corpus documents load packages from a stock TeX Live installation. Where
the reference build selects an input that is not part of that installation,
that exact input is vendored and checksummed under `benchmarks/texmf`, and
verified by `benchmarks/check_manifests.sh`.

## Reference pipeline

The reference engine is pdfTeX: `pdftex` in plain format for plain documents,
`pdflatex` for LaTeX ones. Both engines get one pass over the same file, so
cross-references resolve to the same degree on each side.

The reference is run only as a black-box behavioural oracle. Nothing of its
implementation is read; see `CLEANROOM.md`.

## Correctness gates

Raw PDF bytes are not compared. A candidate passes a document only when all of
the following agree with the reference:

- the run completes, with the same faults reported in the same words;
- the page count;
- every box that did not fit, with its kind, amount, badness and lines;
- line and page breaks;
- glyph identities and positions within a fixed PDF-coordinate tolerance; and
- normalized extracted text.

Metadata timestamps, compression choices, object numbers, object-stream
grouping, xref representation, and font subset names may differ.

The reference's summary statistics count its own string pool, `mem` array,
hash and font tables. They are properties of that program rather than of the
document and are not gated; see `docs/DECISIONS.md`,
`what-a-clean-room-engine-cannot-reproduce`.

## Performance gates

The primary number is the median of seven warm-filesystem-cache runs, each in
a fresh output directory. Each result records document digest, executable
checksum, compiler and flags, CPU model, CPU affinity, worker count, peak RSS,
system load, and individual run times. pdfTeX and HSTeX receive the same
source, auxiliary inputs, environment, and CPU allocation.

The threshold is at least a 5× reduction in median end-to-end wall time, and
10× thereafter. Results from persistent mode are reported separately from
ordinary process-per-pass results.

### A note on scale

This contract previously measured a single legacy benchmark document, which
has been removed from the repository. The largest public subjects now available
are `gentle` at 97 pages and the synthetic `benchmarks/training/train.tex` at
73 pages. That is a real reduction in scale: effects that only appear in a long
document -- deep auxiliary files, large label and destination tables, memory
growth over thousands of pages -- are no longer covered by a standing subject.
Restoring that coverage needs either a large redistributable document or a
generated one of comparable size, and until it exists the performance figures
should be read as applying to documents of the size actually measured.
