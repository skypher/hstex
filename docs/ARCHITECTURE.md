# Architecture

## Execution pipeline

```text
regular file / buffered stream
            │
            ▼
byte spans and source locations
            │
            ▼
catcode-aware tokenizer ── scalar fallback for semantic boundaries
            │
            ▼
compact token streams and control-sequence IDs
            │
            ▼
macro-expansion VM
            │
            ▼
typesetter, paragraph breaker, and page builder
            │
            ▼
immutable shipped-page display jobs
            │
            ▼
parallel PDF stream/font preparation
            │
            ▼
deterministic object assignment and PDF emission
```

Expansion, assignment, paragraph construction, and page building execute in
semantic order. Parallel work begins only after a page or resource becomes
immutable. The output coordinator owns global PDF object numbering and emits a
stable order independent of scheduling.

## Data representation

- Source files are immutable byte spans. Large regular files use `mmap`; small
  files are copied into owned slabs. Streams receive buffered storage.
- Tokens will use a compact 32-bit common representation with an explicit
  escape representation when the payload does not fit.
- Strings occupy append-only byte arenas and are addressed by integer offset
  and length.
- Control sequences receive stable integer IDs from an open-addressed hash
  table.
- Nodes occupy typed, chunked arenas and refer to one another by 32-bit handles.
  A zero handle is null. Hot headers and cold payloads may be separated when a
  profile demonstrates the benefit.
- Engine-global TeX state belongs to one explicit engine context. Immutable
  format state may be shared between fresh contexts.

## SIMD boundary

The tokenizer may vectorize only runs for which the current catcode state gives
a fixed classification. Escape characters, grouping characters, comments,
spaces, line endings, active characters, superscript notation, and any catcode
mutation return control to the semantic path immediately.

The mouth reads physical lines lazily, trims trailing byte-32 spaces, snapshots
the current `endlinechar`, applies mutable catcodes and `^^` conversion as bytes
are requested, and implements TeX's new-line/middle-line/skip-spaces automaton.
It interns regular and active control sequences into separate namespaces and
feeds the same source stack used by future macro replacement lists.

The SIMD scanner recognizes the default lexical-boundary bytes and selects AVX2
at runtime on supported x86-64 CPUs. It is a batching substrate for the mouth;
the semantic path remains authoritative at mutable boundaries. Scalar and
dispatched implementations are tested at every offset and length class.

## PDF backend

Milestone one emits PDF directly and supports the observable pdfTeX primitives
used by the benchmark. Shipped pages become immutable display jobs. Workers may
compress streams, prepare page-local objects, and collect font glyph usage.
The coordinator resolves cross-page destinations, annotations, shared fonts,
object IDs, the xref structure, and final ordering.

The first font surface includes TFM metrics, Type 1 fonts, PK bitmap fonts,
encoding vectors, map files, and the microtype behavior exercised by the
benchmark. Image support is not on the milestone-one critical path because the
pinned corpus contains no `\includegraphics` inputs.

## Format and repeated passes

HSTeX constructs formats from source and writes a representation-native format
cache. The cache is versioned and checksummed; it is never interpreted as a
pdfTeX format dump.

Each TeX pass receives fresh mutable engine state. A future persistent process
may share only immutable format, file-content, parsed-font, and compiled-macro
artifacts whose invalidation keys are explicit. The benchmark reports both
ordinary process-per-pass latency and any persistent-mode result.
