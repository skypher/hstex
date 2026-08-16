# HSTeX project rules

## Goal

Build a clean-room, pdfTeX-compatible typesetting engine in C with substantially
lower wall-clock latency on the pinned `document.tex` benchmark. The first
end-to-end milestone requires both correct reproduction and at least a 5×
speedup over the local pdfTeX baseline.

## Clean-room boundary

- Do not read, translate, adapt, or copy implementation source from TeX/WEB,
  pdfTeX, or any other TeX engine.
- Reference engines may be executed only as black-box behavioral oracles.
- TeX Live macro, font, metric, encoding, and map files are input data, not
  engine implementation material.
- Record the public specification or black-box experiment behind every
  non-obvious compatibility decision.
- Do not preserve a reference engine's internal representation or algorithm
  merely to simplify differential testing.

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

- Treat `tests/corpus-manifest.sha256` as the immutable milestone-one
  corpus identity.
- Never modify the source snapshot to make the engine pass.
- Correctness is evaluated semantically: auxiliary state, page geometry,
  line/page breaking, glyph placement, destinations, links, bookmarks, text,
  and rendered pages. Raw PDF byte identity is not required.
- Report final-pass and fresh three-pass-plus-BibTeX timings separately.
- Record compiler, flags, CPU affinity, worker count, machine load, peak RSS,
  and source manifest for every published performance result.
