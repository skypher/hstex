# Measured benchmark results

The corpus was measured under `docs/BENCHMARK_CONTRACT.md` as the median of
seven warm-filesystem-cache runs per engine per document, each run in an
output directory of its own. The measurements describe the document pass
alone, with both engines starting from a format they already had.

Two measurements were taken. The first was `tests/corpus/run-corpus.sh
--time`, run over the same corpus with the previous engine and with this one,
back to back, which is what "Against the previous engine" below compares. The
second issued the same commands in the same directories with the same pinned
clock and source date, and additionally recorded what the runner does not
print: each individual run time, and peak RSS from a run of its own. The
tables below are the second measurement.

Measured 2026-09-03 on the working tree at commit `6870489` plus the
lookup, format-loading and list-walking changes recorded in
`docs/DECISIONS.md`.

## What was measured on

| Item | Value |
| --- | --- |
| CPU | AMD Ryzen 7 7840HS w/ Radeon 780M, 5 logical CPUs online |
| CPU affinity | unrestricted (mask `1f`) |
| Worker count | one engine process per pass; no `HSTEX_PARALLEL`, `HSTEX_FLEET`, or `HSTEX_FONT_WORKERS` set, so the chunked and fleet paths are inert and Type 1 subsetting may use up to the online CPU count |
| OS | Ubuntu 24.04.3 LTS, Linux 6.8.0-124-generic |
| Load average during the run | 0.51 / 0.60 / 0.44 |
| Compiler | gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1) |
| Build | `tools/build-pgo.sh`: `--buildtype=release -Db_lto=true -Db_pie=false -Db_ndebug=true -Dc_args='-O3 -fno-plt -fno-semantic-interposition' -Dc_link_args=-fno-plt`, profile trained on `benchmarks/training/train.tex` and a format build, never on the corpus |
| Engine | `hstex` 0.0.1, SHA-256 `54fc421f4b307a8f3d0dfce7db7378d26f61ba46283c3444c4c619975d272ebe` |
| Libraries | zlib 1.3, libdeflate 1.19 |
| Reference | pdfTeX 3.141592653-2.6-1.40.25 (TeX Live 2023/Debian), kpathsea 6.3.5 |
| Corpus manifest | `tests/corpus/documents.tsv`, SHA-256 `8681f4df7424f7ac585a7a508047eaf266751d3264b6f049781a961b3040a26f`; per-document digests are the manifest's own column |

## Document pass

Median of seven runs, in milliseconds. Peak RSS is from a separate run of the
same command under `/usr/bin/time -v`, so that the measurement does not sit
inside a timed run.

| Document | Format | Reference | HSTeX | Speedup | Reference RSS | HSTeX RSS |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `story` | plain | 21.1 ms | 3.1 ms | 6.81× | 20.0 MB | 3.5 MB |
| `gentle` | plain | 41.9 ms | 43.9 ms | 0.95× | 20.2 MB | 9.6 MB |
| `small2e` | LaTeX | 107.1 ms | 48.3 ms | 2.22× | 39.0 MB | 27.2 MB |
| `sample2e` | LaTeX | 116.6 ms | 59.4 ms | 1.96× | 39.2 MB | 27.2 MB |
| `testmath` | LaTeX | 216.1 ms | 153.0 ms | 1.41× | 39.7 MB | 27.2 MB |
| `ltx3info` | LaTeX | 126.2 ms | 72.3 ms | 1.75× | 39.4 MB | 27.2 MB |
| `usrguide-historic` | LaTeX | 206.1 ms | 141.8 ms | 1.45× | 39.8 MB | 27.2 MB |
| `cfgguide` | LaTeX | 140.3 ms | 80.1 ms | 1.75× | 39.6 MB | 27.2 MB |
| `cyrguide` | LaTeX | 137.0 ms | 77.9 ms | 1.76× | 39.4 MB | 27.2 MB |
| `modguide` | LaTeX | 134.6 ms | 73.6 ms | 1.83× | 39.4 MB | 27.2 MB |
| `subeqn` | LaTeX | 125.4 ms | 64.8 ms | 1.94× | 39.2 MB | 27.2 MB |
| `technote` | LaTeX | 227.3 ms | 141.3 ms | 1.61× | 39.7 MB | 27.2 MB |
| `tools-overview` | LaTeX | 213.6 ms | 121.4 ms | 1.76× | 39.4 MB | 27.2 MB |
| `ltxcheck` | LaTeX | 99.3 ms | 67.1 ms | 1.48× | 38.1 MB | 27.2 MB |

Median per-document speedup: **1.76×**. Summing the medians, the corpus takes
1912.6 ms on the reference and 1148.0 ms on HSTeX, an aggregate of **1.67×**.
HSTeX's peak RSS is lower on every document in the corpus.

Format construction is not counted above, on either side. HSTeX built the
LaTeX format in 3 s and the plain format in under 1 s, once; the reference's
formats were built once by its distribution.

## Against the previous engine

The same runner measured the previous engine and this one back to back on the
same machine. Every document in the corpus got faster.

| Document | Before | After | Time removed |
| --- | ---: | ---: | ---: |
| `story` | 3.9 ms | 3.1 ms | 20.5% |
| `gentle` | 52.1 ms | 41.6 ms | 20.2% |
| `small2e` | 71.9 ms | 49.7 ms | 30.9% |
| `sample2e` | 80.8 ms | 58.9 ms | 27.1% |
| `testmath` | 171.6 ms | 150.3 ms | 12.4% |
| `ltx3info` | 94.2 ms | 71.8 ms | 23.8% |
| `usrguide-historic` | 164.1 ms | 144.7 ms | 11.8% |
| `cfgguide` | 101.8 ms | 78.8 ms | 22.6% |
| `cyrguide` | 98.1 ms | 78.1 ms | 20.4% |
| `modguide` | 93.8 ms | 73.8 ms | 21.3% |
| `subeqn` | 87.3 ms | 64.2 ms | 26.5% |
| `technote` | 162.1 ms | 140.5 ms | 13.3% |
| `tools-overview` | 140.8 ms | 121.4 ms | 13.8% |
| `ltxcheck` | 88.4 ms | 69.0 ms | 21.9% |
| **corpus** | **1410.9 ms** | **1145.9 ms** | **18.8%** |

Median per-document speedup went from 1.41× to 1.77×, and the aggregate from
1.36× to 1.68×, in that paired run.

Where the time went, measured on the same machine:

| | Before | After |
| --- | ---: | ---: |
| `kpsewhich` children, LaTeX run | 3 | 1 |
| `kpsewhich` children, plain run | 1 | 0 |
| Format load alone (13 MB, `\stop`) | 22.9 ms | 17.1 ms |
| Empty LaTeX document, end to end | 65.3 ms | 41.4 ms |

The four changes behind those numbers are recorded in `docs/DECISIONS.md`
under "The trees a format remembers", "Finding a file", "A format is read
where it lies", "Switching on a node without waiting for its copy", and
"What a run that traces nothing pays".

## Format size and residency

A later change writes each register bank only as far as anything in it has
been set, rather than at the full register count. It is recorded under
"Register banks are written as far as they are set".

| | Before | After |
| --- | ---: | ---: |
| LaTeX format on disk | 13,024,969 B | 9,643,737 B |
| Peak RSS, `small2e` | 27.2 MB | 24.0 MB |
| Peak RSS, `testmath` | 27.2 MB | 26.7 MB |
| Corpus document-pass total | 1148.4 ms | 1141.8 ms |

The document-pass effect, 0.6% with individual documents moving either way,
is not distinguishable from run-to-run noise: the pages it saves were written
once and never read again. The size and residency figures are the measured
effect, and are why it was kept.

The one `kpsewhich` child a LaTeX run still starts cannot be removed by
teaching the lookup more: it is started by a bitmap font that resolves in a
tree outside `TEXMFDBS`, reached by walking the disk rather than by an
`ls-R`. "Why a run still starts one kpsewhich child" in `docs/DECISIONS.md`
records the measurement.

## Against the milestone gate

The gate in `docs/BENCHMARK_CONTRACT.md` is at least a 5× reduction in median
end-to-end wall time. At 1.76× median and 1.67× aggregate, HSTeX does not meet
it. `story` is the only document past the gate, at 6.81×, and it is a one-page
document where the whole difference is 18 ms.

What stands between the corpus and the gate is no longer mostly overhead. An
empty LaTeX document still costs 41.4 ms, of which about 17 ms is loading a
13 MB format and about 10 ms is the one remaining `kpsewhich` child; the rest
is the class file being read and obeyed. Beyond that, HSTeX and the reference
spend comparable time actually setting type -- on `testmath`, 153.0 ms against
216.1 ms with a 41.4 ms and a 97 ms floor respectively -- so a corpus of short
documents is decided by the floor and a corpus of long ones by the typesetting.

## Where HSTeX is slower

`gentle` remains the one document in the corpus HSTeX does not win, at 0.95×
here and 0.98× in the paired run: it is now level with the reference rather
than a fifth behind it, but it is not ahead. It is the corpus's only long
plain document, 97 pages against `story`'s one.

Taking each engine's `story` time as its fixed startup and attributing the
rest of `gentle` to its 96 further pages, the marginal cost per page is about
0.43 ms for HSTeX and about 0.22 ms for the reference. That is a two-point
estimate from two documents of different content, not a measurement of
per-page cost, but it is the same shape as before these changes: HSTeX starts
far faster and sets plain pages more slowly. What the DVI walker no longer
waiting on its own node copy did to that shape is narrow it -- the same
estimate over the previous engine gave 0.51 ms against 0.20 ms, so the excess
per page fell by about a third rather than disappearing.

The LaTeX documents do not separate the two effects, because both engines load
a format there and no LaTeX document in the corpus is nearly as long.

## Individual run times

Every run behind the medians above, in milliseconds, in the order they were
taken.

| Document | Engine | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `story` | reference | 20.0 | 22.7 | 21.2 | 18.9 | 21.5 | 20.6 | 21.1 |
| `story` | HSTeX | 4.1 | 3.8 | 3.8 | 2.9 | 3.1 | 3.1 | 3.0 |
| `gentle` | reference | 41.3 | 46.8 | 42.3 | 41.0 | 41.9 | 43.0 | 41.3 |
| `gentle` | HSTeX | 42.6 | 47.1 | 42.0 | 43.9 | 44.1 | 42.2 | 45.2 |
| `small2e` | reference | 112.1 | 108.7 | 105.9 | 107.1 | 103.8 | 105.4 | 107.3 |
| `small2e` | HSTeX | 47.2 | 48.9 | 48.3 | 47.4 | 48.9 | 48.4 | 47.8 |
| `sample2e` | reference | 115.3 | 118.2 | 119.2 | 116.5 | 114.4 | 116.8 | 116.6 |
| `sample2e` | HSTeX | 59.1 | 59.0 | 60.3 | 58.3 | 59.4 | 59.7 | 59.6 |
| `testmath` | reference | 213.8 | 221.0 | 211.4 | 224.3 | 216.1 | 218.7 | 213.2 |
| `testmath` | HSTeX | 152.1 | 150.6 | 156.9 | 145.9 | 153.0 | 153.4 | 153.1 |
| `ltx3info` | reference | 127.5 | 126.5 | 124.8 | 125.6 | 127.6 | 124.9 | 126.2 |
| `ltx3info` | HSTeX | 72.9 | 70.1 | 71.2 | 72.3 | 72.2 | 73.2 | 73.6 |
| `usrguide-historic` | reference | 205.4 | 205.8 | 207.4 | 206.1 | 211.3 | 205.3 | 208.1 |
| `usrguide-historic` | HSTeX | 141.0 | 140.7 | 141.8 | 138.6 | 146.0 | 153.7 | 144.6 |
| `cfgguide` | reference | 143.4 | 139.1 | 141.8 | 142.3 | 136.6 | 138.0 | 140.3 |
| `cfgguide` | HSTeX | 79.2 | 79.3 | 82.6 | 82.1 | 81.9 | 79.2 | 80.1 |
| `cyrguide` | reference | 137.0 | 142.4 | 136.8 | 135.3 | 139.3 | 136.9 | 137.7 |
| `cyrguide` | HSTeX | 74.7 | 78.7 | 77.6 | 77.9 | 77.9 | 81.3 | 84.5 |
| `modguide` | reference | 133.8 | 132.5 | 134.6 | 130.5 | 135.7 | 135.7 | 135.4 |
| `modguide` | HSTeX | 73.6 | 72.3 | 72.7 | 74.8 | 78.0 | 71.9 | 75.4 |
| `subeqn` | reference | 125.1 | 125.4 | 128.1 | 127.6 | 124.2 | 124.9 | 128.1 |
| `subeqn` | HSTeX | 64.8 | 66.7 | 63.9 | 64.5 | 65.5 | 64.8 | 64.2 |
| `technote` | reference | 237.2 | 226.7 | 233.1 | 227.3 | 225.6 | 234.7 | 227.2 |
| `technote` | HSTeX | 141.3 | 138.4 | 139.3 | 141.6 | 143.8 | 145.1 | 140.8 |
| `tools-overview` | reference | 213.6 | 213.1 | 216.5 | 217.3 | 211.4 | 211.6 | 217.0 |
| `tools-overview` | HSTeX | 117.2 | 122.4 | 124.0 | 120.2 | 121.8 | 120.5 | 121.4 |
| `ltxcheck` | reference | 102.8 | 98.8 | 101.7 | 99.4 | 98.6 | 99.0 | 99.3 |
| `ltxcheck` | HSTeX | 67.7 | 68.0 | 64.7 | 65.8 | 66.7 | 68.1 | 67.1 |

The runner's own medians and this harness's agree: 1.68× against 1.67×
aggregate and 1.77× against 1.76× median. `gentle` differs most between them,
41.6 ms against 43.9 ms, which is the same run-to-run spread its seven runs
show above.

## Correctness at the same tree

Both strict corpora agree with the reference: `tests/corpus/run-corpus.sh
--strict` reports 14/14 documents over 216 pages, and `--stress --strict`
reports 6/6 over 98 pages. The Meson suite passes.

## Not measured here

- Persistent-process results, which the contract requires be reported
  separately from these process-per-pass numbers.
- Fresh three-pass-plus-BibTeX runs. These are final-pass figures.
