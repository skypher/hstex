# Engineering decisions

## Clean-room boundary

HSTeX is implemented from public specifications and black-box observations of
reference engines. Reference-engine implementation source is never read,
translated, or adapted. TeX macro, font, metric, encoding, and map files are
input data rather than engine implementation source.

## Public corpus

The compatibility corpus is defined by `tests/corpus/documents.tsv`. Each
document is fetched from its public source and checked against its pinned
digest. The comparison runner is `tests/corpus/run-corpus.sh`; semantic
comparison requirements are defined in `docs/BENCHMARK_CONTRACT.md`.

## Performance evidence

Published measurements identify the compiler and flags, CPU affinity, worker
count, machine load, peak RSS, source manifest, and timing mode. Performance
changes require a benchmark and compatibility coverage for their semantic
boundary.

## Large glue realization

When a packed box realizes stretch or shrink glue, its running realized amount
is bounded to ±1,000,000,000sp before individual glue widths are obtained.
This is a black-box compatibility decision. With INITEX, a 16,383pt `\vbox`
containing `1fil` vertical glue and a 22pt rule emits a `down4` of
1,001,310,720sp: exactly 1,000,000,000sp of realized glue, plus the box's
ordinary offsets. A two-glue probe splits that bounded cumulative amount across
both glues, and an infinite-shrink probe reaches −1,000,000,000sp. The
canonical Trip output exercises this boundary in `tests/trip/run-trip.sh`.
