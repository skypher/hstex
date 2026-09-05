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
| Load average at the start of the run | 0.89, 0.61, 0.41 |
| Compiler | gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| Build | `tools/build-pgo.sh`: `--buildtype=release -Db_lto=true -Db_pie=false -Db_ndebug=true -Dc_args='-O3 -fno-plt -fno-semantic-interposition' -Dc_link_args=-fno-plt`, profile trained on `benchmarks/training/train.tex` and a format build, never on the corpus |
| Engine | `hstex` at `ed2f4df`, SHA-256 `40405f0924bea7292d32d804fdaf0992ed0715e2c1678a5caca22e310e9a283f` |
| Libraries | zlib 1.3, libdeflate 1.19 |
| Reference | pdfTeX 3.141592653-2.6-1.40.25 (TeX Live 2023/Debian), kpathsea 6.3.5 |
| Corpus manifest | `tests/corpus/documents.tsv`, SHA-256 `19a8aa907f84b498c2f762d2050081945af47484d67be82b04eb283090c1999c`; per-document digests are the manifest's own column |

## Document pass

Median of seven runs, in milliseconds. Peak RSS is from a separate run of the
same command under `/usr/bin/time -v`, so that the measurement does not sit
inside a timed run.

| Document | Format | Reference | HSTeX | Speedup | Reference RSS | HSTeX RSS |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `story` | plain | 20.6 ms | 2.8 ms | 7.36× | 20.0 MB | 4.0 MB |
| `gentle` | plain | 41.6 ms | 36.7 ms | 1.13× | 20.2 MB | 7.2 MB |
| `small2e` | LaTeX | 107.1 ms | 19.0 ms | 5.64× | 39.1 MB | 13.5 MB |
| `sample2e` | LaTeX | 117.6 ms | 25.3 ms | 4.65× | 39.1 MB | 14.9 MB |
| `testmath` | LaTeX | 222.2 ms | 111.6 ms | 1.99× | 39.8 MB | 19.2 MB |
| `ltx3info` | LaTeX | 130.5 ms | 35.6 ms | 3.67× | 39.4 MB | 16.6 MB |
| `usrguide-historic` | LaTeX | 208.1 ms | 104.5 ms | 1.99× | 39.7 MB | 18.5 MB |
| `cfgguide` | LaTeX | 140.0 ms | 43.0 ms | 3.26× | 39.5 MB | 16.9 MB |
| `cyrguide` | LaTeX | 139.0 ms | 40.1 ms | 3.47× | 39.4 MB | 16.9 MB |
| `modguide` | LaTeX | 134.9 ms | 36.6 ms | 3.69× | 39.4 MB | 16.6 MB |
| `subeqn` | LaTeX | 128.1 ms | 32.4 ms | 3.95× | 39.1 MB | 14.1 MB |
| `technote` | LaTeX | 236.3 ms | 107.9 ms | 2.19× | 39.5 MB | 18.4 MB |
| `tools-overview` | LaTeX | 217.4 ms | 86.9 ms | 2.50× | 39.5 MB | 16.8 MB |
| `ltxcheck` | LaTeX | 100.8 ms | 26.6 ms | 3.79× | 38.0 MB | 12.0 MB |
| `clsguide` | LaTeX | 235.9 ms | 128.6 ms | 1.83× | 39.2 MB | 18.8 MB |
| `fntguide` | LaTeX | 282.5 ms | 181.7 ms | 1.55× | 40.1 MB | 20.9 MB |
| `amsldoc` | LaTeX | 411.4 ms | 250.0 ms | 1.65× | 40.0 MB | 22.9 MB |

Median per-document speedup: **3.26×**. Summing the medians, the corpus takes
2874.0 ms on the reference and 1269.3 ms on HSTeX, an aggregate of **2.26×**.
HSTeX is faster than the reference on every document and uses less peak
memory on every document. The three documents new to the corpus --
`clsguide`, `fntguide` and `amsldoc` -- are the longest in it and the
closest to the reference, at 1.55× to 1.83×; over the fourteen measured
before them the median is 3.57× and the aggregate 2.74×.

Format construction is not counted above, on either side. HSTeX built the
LaTeX format in 3 s and the plain format in under 1 s, once; the reference's
formats were built once by its distribution.

## Against the previous engine

The previous record in this file was taken at `0a694fd`, on the same
machine, with the same harness, over fourteen documents; three have joined
the corpus since and have no earlier figure.

| Document | Before | After | Time removed |
| --- | ---: | ---: | ---: |
| `story` | 3.1 ms | 2.8 ms | 9.7% |
| `gentle` | 36.5 ms | 36.7 ms | -0.5% |
| `small2e` | 21.8 ms | 19.0 ms | 12.8% |
| `sample2e` | 31.4 ms | 25.3 ms | 19.4% |
| `testmath` | 121.6 ms | 111.6 ms | 8.2% |
| `ltx3info` | 44.3 ms | 35.6 ms | 19.6% |
| `usrguide-historic` | 113.4 ms | 104.5 ms | 7.8% |
| `cfgguide` | 52.4 ms | 43.0 ms | 17.9% |
| `cyrguide` | 47.9 ms | 40.1 ms | 16.3% |
| `modguide` | 43.8 ms | 36.6 ms | 16.4% |
| `subeqn` | 37.6 ms | 32.4 ms | 13.8% |
| `technote` | 113.5 ms | 107.9 ms | 4.9% |
| `tools-overview` | 92.2 ms | 86.9 ms | 5.7% |
| `ltxcheck` | 26.8 ms | 26.6 ms | 0.7% |
| `clsguide` | — | 128.6 ms | new to the corpus |
| `fntguide` | — | 181.7 ms | new to the corpus |
| `amsldoc` | — | 250.0 ms | new to the corpus |
| **the fourteen measured before** | **786.3 ms** | **709.0 ms** | **9.8%** |

Over the fourteen, the median per-document speedup went from 2.99× to
3.57×, and the aggregate from 2.49× to 2.74×. What changed between
the two records is in `docs/DECISIONS.md`: the Type 1 work a run does --
disassembling a font program and cutting a subset of it -- is kept beside
the format and read back rather than done again ("The Type 1 work a run
does again"), which is most of what `testmath`, `technote` and
`usrguide-historic` gave back. The two documents set in bitmap fonts,
`clsguide` and `fntguide`, were measured only after "A font the map does
not have" was recorded: before it they took 3288 ms and 3900 ms, scanning
the whole font map for every glyph of a font the map does not name.

## Warm runs over the checkpoint cache

The contract keeps persistent-state results apart from the numbers above,
and these are those: what it costs to build a document again over the
checkpoint cache the previous run of it left, against the reference asked
for the same work -- a rebuild of a document whose auxiliary state has
settled. Both sides are settled to their fixpoint before anything is timed.
Median of seven, in milliseconds; `cold` is the run that built the cache,
which compiles to its fixpoint and drops a checkpoint every stride.

| Document | Reference | HSTeX cold | HSTeX warm | Warm speedup | Agreement |
| --- | ---: | ---: | ---: | ---: | --- |
| `small2e` | 109.8 ms | 115.0 ms | 51.1 ms | 2.15× | agrees |
| `sample2e` | 119.6 ms | 117.0 ms | 61.2 ms | 1.95× | agrees |
| `testmath` | 220.3 ms | 299.2 ms | 121.1 ms | 1.82× | agrees |
| `ltx3info` | 131.9 ms | 121.2 ms | 71.9 ms | 1.83× | agrees |
| `usrguide-historic` | 217.8 ms | 337.5 ms | 146.4 ms | 1.49× | agrees |
| `cfgguide` | 146.6 ms | 177.8 ms | 81.7 ms | 1.79× | agrees |
| `cyrguide` | 140.3 ms | 176.7 ms | 77.8 ms | 1.80× | agrees |
| `modguide` | 134.7 ms | 171.7 ms | 71.5 ms | 1.88× | agrees |
| `subeqn` | 125.8 ms | 188.5 ms | 58.7 ms | 2.14× | agrees |
| `technote` | 237.6 ms | 263.5 ms | 74.0 ms | 3.21× | agrees |
| `tools-overview` | 215.2 ms | 250.9 ms | 58.8 ms | 3.66× | agrees |
| `clsguide` | 240.1 ms | 398.0 ms | 153.6 ms | 1.56× | agrees |
| `fntguide` | 289.0 ms | 582.6 ms | 174.8 ms | 1.65× | agrees |
| `amsldoc` | 411.3 ms | 836.2 ms | 175.4 ms | 2.34× | agrees |

Every warm run agrees with the reference on all nine semantic checks of
`tests/corpus/compare-pdf.py`. A settled document is one pass on a warm
run, and the pass resumes the page-zero checkpoint, so no chunk reads the
preamble ("A settled document is one pass, and warm on its second run").
The warm run of `amsldoc` runs `makeindex` through `\write18` as the
reference does, in the same directory.

## Correctness at the same tree

Both strict corpora agree with the reference: `tests/corpus/run-corpus.sh
--strict` reports 17/17 documents, and `--stress --strict` 6/6. The driver
corpus, `tests/corpus/run-driver-corpus.sh --strict`, finds every document
agreeing with the reference and pins none. The Meson suite and the Trip test
pass.

## Individual run times

Every run behind the medians above, in milliseconds, in the order they were
taken.

| Document | Engine | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `story` | reference | 20.6 | 22.7 | 20.3 | 20.6 | 20.2 | 24.9 | 21.3 |
| `story` | HSTeX | 3.7 | 2.7 | 2.7 | 3.1 | 3.2 | 2.8 | 2.4 |
| `gentle` | reference | 41.6 | 42.9 | 40.7 | 43.6 | 41.2 | 42.9 | 41.1 |
| `gentle` | HSTeX | 39.2 | 35.7 | 36.8 | 35.2 | 36.4 | 38.1 | 36.7 |
| `small2e` | reference | 106.3 | 106.2 | 106.8 | 107.9 | 107.1 | 108.3 | 114.8 |
| `small2e` | HSTeX | 18.3 | 19.5 | 20.2 | 19.0 | 18.5 | 18.4 | 19.1 |
| `sample2e` | reference | 119.9 | 117.2 | 119.1 | 117.4 | 116.9 | 117.6 | 118.3 |
| `sample2e` | HSTeX | 25.3 | 24.9 | 25.3 | 26.4 | 26.3 | 25.1 | 26.5 |
| `testmath` | reference | 219.1 | 223.6 | 221.3 | 220.8 | 223.1 | 223.2 | 222.2 |
| `testmath` | HSTeX | 110.0 | 107.7 | 112.6 | 110.4 | 111.6 | 115.4 | 112.2 |
| `ltx3info` | reference | 130.5 | 132.7 | 130.9 | 129.7 | 132.6 | 129.2 | 127.2 |
| `ltx3info` | HSTeX | 35.7 | 34.8 | 34.3 | 35.6 | 36.2 | 35.3 | 39.6 |
| `usrguide-historic` | reference | 206.7 | 206.3 | 208.1 | 208.5 | 213.6 | 206.7 | 212.5 |
| `usrguide-historic` | HSTeX | 104.5 | 104.7 | 102.0 | 104.5 | 102.5 | 101.5 | 105.3 |
| `cfgguide` | reference | 146.2 | 140.0 | 139.6 | 142.3 | 143.5 | 138.7 | 138.3 |
| `cfgguide` | HSTeX | 42.2 | 43.0 | 45.2 | 42.3 | 41.0 | 46.4 | 44.1 |
| `cyrguide` | reference | 142.5 | 137.9 | 139.0 | 137.6 | 141.7 | 142.1 | 138.4 |
| `cyrguide` | HSTeX | 43.7 | 42.5 | 39.6 | 39.5 | 39.7 | 40.1 | 40.7 |
| `modguide` | reference | 133.0 | 135.5 | 134.6 | 134.9 | 134.0 | 135.3 | 135.2 |
| `modguide` | HSTeX | 37.4 | 36.6 | 36.1 | 36.0 | 37.8 | 36.1 | 38.4 |
| `subeqn` | reference | 130.4 | 129.7 | 128.1 | 126.1 | 130.2 | 126.3 | 126.0 |
| `subeqn` | HSTeX | 32.1 | 34.1 | 31.9 | 32.9 | 32.4 | 33.0 | 31.2 |
| `technote` | reference | 247.0 | 234.8 | 236.3 | 244.4 | 235.9 | 239.3 | 236.2 |
| `technote` | HSTeX | 105.2 | 109.0 | 107.9 | 105.7 | 107.9 | 106.9 | 109.2 |
| `tools-overview` | reference | 212.6 | 221.5 | 217.4 | 224.4 | 215.3 | 212.9 | 217.4 |
| `tools-overview` | HSTeX | 89.5 | 85.9 | 87.3 | 87.5 | 85.8 | 86.9 | 86.8 |
| `ltxcheck` | reference | 100.6 | 104.8 | 104.3 | 99.5 | 100.8 | 100.9 | 100.6 |
| `ltxcheck` | HSTeX | 26.6 | 25.8 | 25.2 | 27.0 | 26.4 | 26.7 | 26.8 |
| `clsguide` | reference | 235.6 | 235.9 | 236.8 | 231.9 | 238.8 | 236.0 | 235.1 |
| `clsguide` | HSTeX | 133.2 | 128.6 | 126.6 | 128.8 | 128.2 | 130.5 | 127.6 |
| `fntguide` | reference | 293.1 | 290.5 | 282.1 | 281.0 | 282.5 | 285.4 | 281.8 |
| `fntguide` | HSTeX | 173.1 | 178.8 | 181.7 | 182.1 | 183.6 | 182.8 | 181.4 |
| `amsldoc` | reference | 415.6 | 398.6 | 399.3 | 411.4 | 407.4 | 412.3 | 413.7 |
| `amsldoc` | HSTeX | 247.6 | 249.2 | 250.5 | 250.0 | 249.4 | 253.8 | 260.5 |

## Not measured here

- Fresh three-pass-plus-BibTeX runs. These are final-pass figures.
- The driver's own wall time, which adds a format-cache check and a
  preamble-checkpoint resume to the engine's numbers above.
