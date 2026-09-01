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
- Tokens use a compact 32-bit common representation with separate character
  and control-sequence layouts.
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
feeds the same source stack used by macro replacement lists.

Macro bodies are immutable token arrays addressed by integer identifiers.
Control-sequence meanings live in a growable indexed table, and replacement
lists are instantiated into owned input frames. The expansion loop collects
arguments without expanding them, supports literal and delimited parameter
texts, strips one enclosing argument group, and substitutes parameter token
arrays without linked lists. A compact save stack restores local meanings at
group exit; global assignments supersede pending local restores by level.

Integer parameters, count registers, and catcodes use the same level-tagged
save discipline. Numeric scanning expands only while acquiring a value and
supports decimal, character constants, register aliases, and internal catcode
queries. Conditionals are tracked in a separate contiguous stack; false
branches are skipped without macro expansion while nested conditionals remain
balanced.

Dimensions are signed scaled-point integers. Their scanner uses checked integer
fixed-point arithmetic for decimal factors, physical units, internal dimension
values, and TeX's bounded range. Units are matched as backtracking keywords
rather than as letter runs, and a decimal factor is quantized to scaled points
before any unit conversion; `docs/DECISIONS.md` records the observations behind
both rules.

Page state — `\deadcycles`, `\insertpenalties`, and the page dimensions — lives
directly on the engine context with no save-stack entries, because it belongs
to the page rather than to a group. The page counts as empty until a box or
rule reaches the main vertical list, and while it is empty the page dimensions
report a fixed `\maxdimen` goal with zero totals. Reading them after that point
requires the page builder and is refused rather than answered with a stored
zero. Glue stores width, stretch, shrink, and the two
infinite-order tags inline; registers and named dimension/glue parameters share
the meaning and save-stack machinery used by integer state. Character code
tables are flat 256-entry arrays with level tags, keeping assignments and
lookups contiguous.

Assigned token lists are immutable arrays addressed by integer IDs. Token
registers and named token parameters therefore copy one ID and restore one ID
at group exit, while old values remain valid without reference-count traffic.
`the` inserts token-list control sequences with a one-expansion marker: expanded
definitions and writes receive the original tokens, while ordinary execution
requeues them for normal expansion or command dispatch.

Expanded definitions reuse the ordinary immutable macro representation but
drive the replacement scanner through the expansion loop. Integer `the` and
`number` expansions materialize compact other-character token arrays;
conditionals execute inside that loop, so expanded definitions see the same
branch semantics as ordinary execution. Protected macros remain opaque during
expanded definitions and writes. Checked 64-bit intermediates implement 32-bit
`advance`, `multiply`, and `divide` assignments.

File input first checks the process and calling-file directories, then searches
TeX Live's `ls-R` databases through an in-process index. Ambiguous or custom
search configurations use `kpsewhich` as a safe argv-based fallback. Resolved
files enter the same owned source stack.

The bootstrap stream layer owns sixteen input and output `FILE` handles inside
the engine context. Immediate writes expand a sentinel-terminated token frame,
serialize characters and control sequences, and flush deterministically; line
reads tokenize through the same mutable mouth before defining the destination
macro. Diagnostic INITEX runs isolate generated files under the ignored build
tree.

The SIMD scanner recognizes the default lexical-boundary bytes and selects AVX2
at runtime on supported x86-64 CPUs. It is a batching substrate for the mouth;
the semantic path remains authoritative at mutable boundaries. Scalar and
dispatched implementations are tested at every offset and length class.

## PDF backend

HSTeX emits PDF directly and supports the observable pdfTeX primitives used by
the compatibility corpus. Shipped pages become immutable display jobs. Workers
may compress streams, prepare page-local objects, and collect font glyph usage.
The coordinator resolves cross-page destinations, annotations, shared fonts,
object IDs, the xref structure, and final ordering.

The font surface includes TFM metrics, Type 1 fonts, PK bitmap fonts,
encoding vectors, map files, and the microtype behavior exercised by the
strict corpus. Image inclusion is outside the current strict compatibility
surface.

When a Type 1 font is first reached, the engine reads its PFB or PFA program
directly. The reader validates PFB segments or PFA hexadecimal/binary eexec
data, decrypts eexec and charstrings, and produces the engine's canonical
textual representation without a utility process. PFB and PFA line endings
are normalized at their text boundaries; binary lengths remain authoritative.

At document close, Type 1 subset programs are prepared independently by a
bounded worker batch, after the physical-font set and each font's glyph set
are immutable. Object allocation and serialization remain in sorted order.
The subset's textual charstrings are encoded and both Type 1 cipher layers
are applied inside HSTeX. Neither Type 1 disassembly nor assembly launches an
external process.
On Linux the worker count defaults to the process's available processors; a
platform without an available processor-count interface defaults to one.
The count is capped at eight and at the number of physical fonts;
`HSTEX_FONT_WORKERS` selects a positive count explicitly, including `1` for
the scalar fallback.

## Format and repeated passes

HSTeX constructs formats from source and writes a representation-native format
cache. The cache is versioned and checksummed; it is never interpreted as a
pdfTeX format dump.

Ordinary TeX passes receive fresh mutable engine state. Experimental
checkpoint/fleet mode forks page-boundary snapshots and shares immutable
format, file-content, parsed-font, and compiled-macro artifacts. Its results
are reported separately from ordinary process-per-pass latency under the
benchmark contract.
