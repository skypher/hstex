# Milestone-one benchmark contract

## Corpus identity

The root document is `tests/corpus/document.tex`. Its recursive
project-owned input closure is recorded in
`tests/corpus-manifest.sha256`. The snapshot comes from
`removed-location` commit `06c7ec6ee6e543c283753f7cc7ac187f81baeef9`.

The corpus loads a locally selected `cleveref.sty`; that exact input is vendored
and checksummed under `benchmarks/texmf`. Other TeX Live dependencies are
captured by the oracle run's recorder file and tool-version report.

## Reference pipeline

The clean reference pipeline is:

1. pdfTeX in pdflatex format;
2. BibTeX when the first-pass auxiliary file contains citations;
3. a second pdfTeX pass; and
4. a final pdfTeX pass.

The runner uses a fresh output directory, records every TeX input with
`-recorder`, retains stage stdout and resource timing, and rejects final logs
with unresolved references, unresolved citations, duplicate destinations, or
multiply defined labels.

## Correctness gates

Raw PDF bytes are not compared. A candidate output passes only when all of the
following normalized observations agree with the pinned reference:

- successful pass sequence and exit status;
- citation, label, table-of-contents, and bookmark auxiliary semantics;
- page count, media boxes, and page ordering;
- line and page breaks;
- glyph identities and positions within a fixed PDF-coordinate tolerance;
- link annotations, named destinations, and document outlines;
- normalized extracted text; and
- rendered-page comparisons using the same rasterizer and a documented
  anti-aliasing tolerance.

Metadata timestamps, compression choices, object numbers, object-stream
grouping, xref representation, and font subset names may differ.

## Performance gates

Two latency measurements are mandatory:

1. a final pass with completed auxiliary inputs; and
2. a fresh TeX → BibTeX → TeX → TeX build.

The primary number is the median of seven warm-filesystem-cache runs, each in a
fresh output directory. Each result records source checksum, executable
checksum, compiler and flags, CPU model, CPU affinity, worker count, peak RSS,
system load, and individual run times. pdfTeX and HSTeX receive the same source,
auxiliary inputs, environment, and CPU allocation.

Milestone one requires at least a 5× reduction in median end-to-end wall time.
The subsequent performance threshold is 10×. Results from persistent mode are
reported separately from ordinary process-per-pass results.
