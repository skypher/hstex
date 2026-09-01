# HSTeX project rules

## Goal

Build an independent, pdfTeX-compatible typesetting engine in C with
substantially lower wall-clock latency than pdfTeX. The first end-to-end
milestone requires both correct reproduction of a real document corpus and at
least a 5× speedup over the local pdfTeX baseline.

## Source-use boundary

- Public implementation source from TeX/WEB, pdfTeX, and other TeX engines may
  be consulted for behavior, algorithms, and edge cases.
- Do not paste or mechanically translate incompatibly licensed engine code.
  Any compatible code or data incorporated into HSTeX must retain its required
  notice and be recorded in `THIRD_PARTY_NOTICES.md`.
- TeX Live macro, font, metric, encoding, and map files are input data, not
  engine implementation material.
- Record the exact specification, controlled experiment, or implementation
  source—including version or commit, file, and section—behind every
  non-obvious compatibility decision.
- Do not preserve a reference engine's internal representation or algorithm
  merely to simplify differential testing.
- Production HSTeX must not invoke another TeX engine or depend on its internal
  state. See `SOURCE_POLICY.md` for the full provenance and licensing policy.

## Implementation rules

- The engine is C17. Do not introduce Rust or C++ into the engine.
- Linux x86-64 is the first platform. Every SIMD path requires a tested scalar
  fallback and runtime feature dispatch.
- Core TeX arithmetic uses explicit-width integer or fixed-point types. Do not
  enable unsafe floating-point transformations such as `-ffast-math`.
- Prefer contiguous storage, typed arenas, integer handles, and explicit
  ownership. Persistent pointers into resizable storage are forbidden.
- Keep engine state in explicit context objects; avoid pervasive mutable
  globals.
- Preserve deterministic output across thread counts.
- A performance optimization lands only with a benchmark showing its effect
  and compatibility tests covering its semantic boundary.
- Assembly is permitted only for a measured hot spot after intrinsics have
  been tested.

## Benchmark rules

- Treat `tests/corpus/documents.tsv` as the corpus identity: every document is
  pinned by digest and fetched, never vendored.
- Never modify a corpus document to make the engine pass. A document that
  fails is a finding, not a defect in the document.
- Correctness is evaluated semantically: auxiliary state, page geometry,
  line/page breaking, glyph placement, destinations, links, bookmarks, text,
  and rendered pages. Raw PDF byte identity is not required.
- Report final-pass and fresh three-pass-plus-BibTeX timings separately.
- Record compiler, flags, CPU affinity, worker count, machine load, peak RSS,
  and source manifest for every published performance result.
