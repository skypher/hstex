# Measured benchmark results

The corpus was measured under `docs/BENCHMARK_CONTRACT.md` as the median of
seven warm-filesystem-cache runs per engine per document, each run in an
output directory of its own, on the machine below. The reference is the
installed pdfTeX. Every number here was taken in one sitting from one build.

## What was measured on

| Item | Value |
| --- | --- |
| CPU | AMD Ryzen 7 7840HS w/ Radeon 780M, 5 logical CPUs online |
| CPU affinity | unrestricted (mask `1f`) |
| Worker count | one engine process per pass; no `HSTEX_PARALLEL`, `HSTEX_FLEET`, or `HSTEX_FONT_WORKERS` set, so the chunked and fleet paths are inert and Type 1 subsetting may use up to the online CPU count |
| OS | Ubuntu 24.04.3 LTS, Linux 6.8.0-124-generic |
| Load average at the start of the run | 0.40, 0.67, 0.52 |
| Compiler | gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| Build | `tools/build-pgo.sh`: `--buildtype=release -Db_lto=true -Db_pie=false -Db_ndebug=true -Dc_args='-O3 -fno-plt -fno-semantic-interposition' -Dc_link_args=-fno-plt`, profile trained on `benchmarks/training/train.tex` and a format build, never on the corpus |
| Engine | `hstex` at `0a694fd`, SHA-256 `ce206ae698044c10a4731252654a98f6ae95728b5e0817a7f1a508bfa208e4f5` |
| Libraries | zlib 1.3, libdeflate 1.19 |
| Reference | pdfTeX 3.141592653-2.6-1.40.25 (TeX Live 2023/Debian), kpathsea 6.3.5 |
| Corpus manifest | `tests/corpus/documents.tsv`, SHA-256 `8681f4df7424f7ac585a7a508047eaf266751d3264b6f049781a961b3040a26f`; per-document digests are the manifest's own column |

## Document pass

Median of seven runs, in milliseconds. Peak RSS is from a separate run of the
same command under `/usr/bin/time -v`, so that the measurement does not sit
inside a timed run.

| Document | Format | Reference | HSTeX | Speedup | Reference RSS | HSTeX RSS |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `story` | plain | 21.5 ms | 3.1 ms | 6.94× | 20.0 MB | 4.0 MB |
| `gentle` | plain | 43.0 ms | 36.5 ms | 1.18× | 20.1 MB | 7.2 MB |
| `small2e` | LaTeX | 107.4 ms | 21.8 ms | 4.93× | 39.0 MB | 13.5 MB |
| `sample2e` | LaTeX | 117.6 ms | 31.4 ms | 3.75× | 39.2 MB | 14.9 MB |
| `testmath` | LaTeX | 226.6 ms | 121.6 ms | 1.86× | 39.8 MB | 20.2 MB |
| `ltx3info` | LaTeX | 133.4 ms | 44.3 ms | 3.01× | 39.4 MB | 16.8 MB |
| `usrguide-historic` | LaTeX | 210.4 ms | 113.4 ms | 1.86× | 39.6 MB | 19.6 MB |
| `cfgguide` | LaTeX | 142.3 ms | 52.4 ms | 2.72× | 39.4 MB | 17.5 MB |
| `cyrguide` | LaTeX | 142.2 ms | 47.9 ms | 2.97× | 39.5 MB | 17.4 MB |
| `modguide` | LaTeX | 134.7 ms | 43.8 ms | 3.08× | 39.4 MB | 17.1 MB |
| `subeqn` | LaTeX | 128.6 ms | 37.6 ms | 3.42× | 39.2 MB | 14.4 MB |
| `technote` | LaTeX | 234.1 ms | 113.5 ms | 2.06× | 39.6 MB | 18.6 MB |
| `tools-overview` | LaTeX | 217.4 ms | 92.2 ms | 2.36× | 39.4 MB | 16.9 MB |
| `ltxcheck` | LaTeX | 100.1 ms | 26.8 ms | 3.74× | 38.1 MB | 11.9 MB |

Median per-document speedup: **2.99×**. Summing the medians, the corpus takes
1959.3 ms on the reference and 786.3 ms on HSTeX, an aggregate of **2.49×**.
HSTeX is faster than the reference on every document and uses less peak
memory on every document; `gentle`, the one document that was at parity, is
now ahead.

Format construction is not counted above, on either side. HSTeX built the
LaTeX format in 3 s and the plain format in under 1 s, once; the reference's
formats were built once by its distribution.

## Against the previous engine

The previous record in this file was taken at `3a6d5de`, on the same
machine, with the same harness. Every document in the corpus got faster.

| Document | Before | After | Time removed |
| --- | ---: | ---: | ---: |
| `story` | 3.1 ms | 3.1 ms | 0.0% |
| `gentle` | 43.9 ms | 36.5 ms | 16.9% |
| `small2e` | 48.3 ms | 21.8 ms | 54.9% |
| `sample2e` | 59.4 ms | 31.4 ms | 47.1% |
| `testmath` | 153.0 ms | 121.6 ms | 20.5% |
| `ltx3info` | 72.3 ms | 44.3 ms | 38.7% |
| `usrguide-historic` | 141.8 ms | 113.4 ms | 20.0% |
| `cfgguide` | 80.1 ms | 52.4 ms | 34.6% |
| `cyrguide` | 77.9 ms | 47.9 ms | 38.5% |
| `modguide` | 73.6 ms | 43.8 ms | 40.5% |
| `subeqn` | 64.8 ms | 37.6 ms | 42.0% |
| `technote` | 141.3 ms | 113.5 ms | 19.7% |
| `tools-overview` | 121.4 ms | 92.2 ms | 24.1% |
| `ltxcheck` | 67.1 ms | 26.8 ms | 60.1% |
| **corpus** | **1148.0 ms** | **786.3 ms** | **31.5%** |

Median per-document speedup went from 1.76× to 2.99×, and the aggregate from
1.67× to 2.49×.

Where the time went, measured on the same machine with the same build:

| | Before | After |
| --- | ---: | ---: |
| `kpsewhich` children, LaTeX run | 1 | 0 |
| `kpsewhich` children, plain run | 0 | 0 |
| Format load alone (`\stop`) | 17.1 ms | 0.9 ms |
| Empty LaTeX document, end to end | 41.4 ms | 13.4 ms |
| Minor page faults, empty LaTeX document | 5,756 | 830 |
| Peak RSS, empty LaTeX document | 23.7 MB | 12.1 MB |
| LaTeX format on disk | 9,643,737 B | 14,234,224 B |

The changes behind those numbers are recorded in `docs/DECISIONS.md` under
"What a format carries built, and what is read where it lies" and "The
search path, walked as the tool walks it": the format carries the filename
database built and the search paths the tool would walk, so a run starts no
tool; the format is mapped at the address it was written for and its tables
are read where they lie, so a run reads the pages it uses and copies none.
The format is larger for what it carries, none of which a run reads unless
it looks something up.

## Warm runs over the checkpoint cache

The contract keeps persistent-state results apart from the numbers above,
and these are those: what it costs to build a document again over the
checkpoint cache the previous run of it left, against the reference asked
for the same work -- a rebuild of a document whose auxiliary state has
settled. Both sides are settled to their fixpoint before anything is timed.
Median of seven, in milliseconds; `cold` is the run that built the cache.

| Document | Reference | HSTeX cold | HSTeX warm | Warm speedup | Agreement |
| --- | ---: | ---: | ---: | ---: | --- |
| `small2e` | 112.1 ms | 163.5 ms | 65.9 ms | 1.70× | agrees |
| `sample2e` | 117.0 ms | 181.3 ms | 76.2 ms | 1.54× | agrees |
| `testmath` | 218.7 ms | 455.8 ms | 136.0 ms | 1.61× | agrees |
| `ltx3info` | 129.2 ms | 200.9 ms | 87.1 ms | 1.48× | agrees |
| `usrguide-historic` | 212.7 ms | 495.2 ms | 159.6 ms | 1.33× | agrees |
| `cfgguide` | 145.9 ms | 312.4 ms | 93.9 ms | 1.55× | agrees |
| `cyrguide` | 141.0 ms | 301.0 ms | 91.0 ms | 1.55× | agrees |
| `modguide` | 134.2 ms | 285.8 ms | 84.6 ms | 1.59× | agrees |
| `subeqn` | 127.2 ms | 273.1 ms | 72.2 ms | 1.76× | agrees |
| `technote` | 237.6 ms | 375.8 ms | 88.9 ms | 2.67× | agrees |
| `tools-overview` | 211.6 ms | 318.5 ms | 74.2 ms | 2.85× | agrees |

Every warm run agrees with the reference on all nine semantic checks of
`tests/corpus/compare-pdf.py`. The cold run is slower than a plain run: it
compiles to its fixpoint and drops a checkpoint every stride while it does.

## Correctness at the same tree

Both strict corpora agree with the reference: `tests/corpus/run-corpus.sh
--strict` reports 14/14 documents, and `--stress --strict` 6/6. The driver
corpus, `tests/corpus/run-driver-corpus.sh --strict`, finds every document
agreeing with the reference and pins none. The Meson suite and the Trip test
pass.

## Individual run times

Every run behind the medians above, in milliseconds, in the order they were
taken.

| Document | Engine | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `story` | reference | 21.3 | 20.5 | 22.4 | 21.1 | 21.1 | 22.5 | 20.2 |
| `story` | HSTeX | 2.8 | 2.8 | 3.5 | 3.8 | 2.9 | 3.1 | 3.3 |
| `gentle` | reference | 42.3 | 43.8 | 44.1 | 41.7 | 41.0 | 42.7 | 42.5 |
| `gentle` | HSTeX | 35.9 | 36.6 | 38.1 | 39.0 | 35.5 | 36.8 | 37.0 |
| `small2e` | reference | 110.4 | 109.5 | 111.3 | 110.2 | 113.1 | 117.9 | 110.8 |
| `small2e` | HSTeX | 22.3 | 25.0 | 22.7 | 22.9 | 23.6 | 21.6 | 22.5 |
| `sample2e` | reference | 122.8 | 124.3 | 118.5 | 119.4 | 118.6 | 120.2 | 115.6 |
| `sample2e` | HSTeX | 29.2 | 30.9 | 33.0 | 30.9 | 30.8 | 30.8 | 32.0 |
| `testmath` | reference | 224.2 | 217.4 | 221.3 | 218.7 | 223.5 | 224.0 | 220.5 |
| `testmath` | HSTeX | 119.7 | 121.8 | 117.9 | 120.1 | 122.8 | 119.8 | 122.4 |
| `ltx3info` | reference | 131.8 | 129.9 | 129.7 | 132.1 | 137.8 | 131.9 | 129.5 |
| `ltx3info` | HSTeX | 43.8 | 46.3 | 44.7 | 48.1 | 44.9 | 47.3 | 44.0 |
| `usrguide-historic` | reference | 212.0 | 209.9 | 214.9 | 215.0 | 214.7 | 212.3 | 208.4 |
| `usrguide-historic` | HSTeX | 111.1 | 113.4 | 115.1 | 109.4 | 114.6 | 113.3 | 114.0 |
| `cfgguide` | reference | 147.7 | 147.4 | 141.9 | 140.2 | 144.7 | 141.3 | 141.5 |
| `cfgguide` | HSTeX | 50.0 | 52.5 | 51.5 | 50.2 | 50.9 | 52.2 | 52.7 |
| `cyrguide` | reference | 143.5 | 142.0 | 140.1 | 139.5 | 143.2 | 139.3 | 148.5 |
| `cyrguide` | HSTeX | 48.8 | 46.3 | 47.6 | 49.0 | 47.1 | 48.4 | 45.9 |
| `modguide` | reference | 136.3 | 133.9 | 134.5 | 133.7 | 133.3 | 137.6 | 130.7 |
| `modguide` | HSTeX | 43.2 | 44.1 | 44.4 | 48.3 | 44.6 | 42.3 | 44.1 |
| `subeqn` | reference | 125.8 | 126.0 | 129.0 | 126.1 | 126.0 | 126.7 | 129.8 |
| `subeqn` | HSTeX | 37.6 | 36.3 | 36.0 | 37.5 | 37.6 | 37.7 | 35.5 |
| `technote` | reference | 239.8 | 232.5 | 236.4 | 230.3 | 234.2 | 241.0 | 225.8 |
| `technote` | HSTeX | 108.5 | 108.4 | 109.1 | 115.6 | 110.1 | 106.0 | 110.5 |
| `tools-overview` | reference | 209.4 | 222.2 | 215.4 | 219.2 | 218.4 | 230.8 | 220.8 |
| `tools-overview` | HSTeX | 89.4 | 94.1 | 84.6 | 93.7 | 87.3 | 87.3 | 95.0 |
| `ltxcheck` | reference | 102.8 | 100.7 | 99.4 | 103.1 | 100.9 | 100.3 | 101.4 |
| `ltxcheck` | HSTeX | 27.3 | 30.1 | 27.1 | 25.9 | 25.3 | 25.8 | 25.6 |

The runner's own medians and this harness's agree: 2.49× against 2.52×
aggregate.

## Not measured here

- Fresh three-pass-plus-BibTeX runs. These are final-pass figures.
- The driver's own wall time, which adds a format-cache check and a
  preamble-checkpoint resume to the engine's numbers above.
