# Source-use and provenance policy

HSTeX is an independent C17 implementation of TeX and pdfTeX behavior. Public
implementation sources may be consulted, but every use must preserve the
project's Apache-2.0 licensing and make the origin of non-obvious decisions
auditable.

## Permitted evidence

Implementation work may use:

- public specifications and descriptions of TeX and pdfTeX behavior;
- public source code for TeX, pdfTeX, and other TeX engines;
- package documentation and ordinary TeX input files;
- standardized conformance inputs such as Trip;
- controlled executions of reference engines;
- logs, DVI/PDF files, auxiliary files, and other observable outputs; and
- independently designed algorithms and data structures.

Source study may inform behavior, algorithms, and edge cases. It does not by
itself establish compatibility: externally observable behavior still needs a
test or a cited specification.

## Licensing boundary

- Do not paste or mechanically translate code from an incompatibly licensed
  engine into HSTeX.
- Do not preserve another engine's internal representation merely to simplify
  comparison or porting.
- Code or data incorporated under a compatible license must retain its
  required notices and be recorded in `THIRD_PARTY_NOTICES.md` and the vendored
  manifest where applicable.
- Record the engine, version or commit, file, and relevant section whenever
  implementation source materially informs a non-obvious HSTeX decision.
- Copied tests require an explicit license and provenance record.

HSTeX constructs and serializes its own native format state. It does not read
or depend on pdfTeX format dumps.

## Runtime separation

Reference execution belongs under `benchmarks/` and `tests/`. Production
engine code must not invoke another TeX engine or depend on its generated
internal state. Differential tests retain the exact input, command, tool
version, and normalized comparison rule.

TeX Live packages, fonts, TFM files, encoding files, and map files are document
inputs. General-purpose libraries such as zlib may be used under their own
licenses.

## Decision record

Every non-obvious compatibility rule must receive a short record containing:

1. the public specification, source location, or controlled input used;
2. the exact version or commit of every consulted implementation;
3. the relevant observed output or source section;
4. the user-visible rule derived from that evidence;
5. the HSTeX representation or algorithm; and
6. any license or attribution obligation.
