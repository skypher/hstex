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
