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

The corpus runner uses reference executables only for their observable output.
Implementation source may be consulted separately under `SOURCE_POLICY.md`,
but it is not an input to the differential run.

## Correctness gates

For a plain document set to DVI, the output is compared byte for byte, with
the clock pinned so the two runs cannot differ by the time of day. Both
documents in the corpus that can be compared that way are identical to the
reference's.

Raw PDF bytes are not compared -- a PDF carries identifiers and timestamps of
its own. A candidate passes such a document only when all of the following
agree with the reference:

- the run completes, with the same faults reported in the same words;
- the page count;
- every box that did not fit, with its kind, amount, badness and lines;
- cross-reference and navigation state in generated auxiliary files;
- every page box and rotation;
- line and page breaks and normalized extracted text;
- glyph identities, fonts, transforms, and positions within 0.01 PDF points;
- named destinations, URI and page-link annotations, and bookmarks; and
- the exact 144 dpi, 8-bit-antialiased RGB rendering produced for each side by
  the same MuPDF invocation.

`tests/corpus/compare-pdf.py` implements the representation-independent PDF
checks using MuPDF and Poppler. Font subset prefixes are ignored. Auxiliary
files used as cross-pass state are compared byte for byte because both engines
receive the same job name, inputs, environment, and pass count.

Metadata timestamps, compression choices, object numbers, object-stream
grouping, xref representation, and font subset names may differ.

The reference's summary statistics count its own string pool, `mem` array,
hash and font tables. They describe its implementation rather than the
document and are not semantic gates.

## Performance gates

The primary number is the median of seven warm-filesystem-cache runs, each in
a fresh output directory. Each result records document digest, executable
checksum, compiler and flags, CPU model, CPU affinity, worker count, peak RSS,
system load, and individual run times. pdfTeX and HSTeX receive the same
source, auxiliary inputs, environment, and CPU allocation.

The threshold is at least a 5× reduction in median end-to-end wall time, and
10× thereafter. Results from persistent mode are reported separately from
ordinary process-per-pass results.
