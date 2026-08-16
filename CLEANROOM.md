# Clean-room implementation policy

## Permitted inputs

Implementation work may use:

- public descriptions of TeX and pdfTeX user-visible behavior;
- package documentation and ordinary TeX input files;
- standardized conformance inputs such as TRIP;
- black-box executions of pdfTeX with controlled inputs;
- output artifacts such as logs, DVI/PDF files, and auxiliary files produced
  by those controlled executions; and
- independently designed algorithms and data structures.

## Prohibited inputs

Implementation work must not use:

- TeX/WEB, pdfTeX, or another TeX engine's implementation source;
- ports, translations, annotated implementations, or code-derived
  pseudocode from those engines;
- internal data layouts or serialized formats reverse-engineered for reuse;
  or
- copied tests whose licensing or provenance is not recorded.

The project does not attempt binary compatibility with a reference engine's
format dumps. HSTeX will construct and serialize its own format state from TeX
source inputs.

## Oracle separation

Reference execution belongs under `benchmarks/` and `tests/oracle/`. Production
engine code must not invoke a reference engine or depend on its generated
internal state. Differential tests compare externally visible behavior and
must retain the exact input, reference command, tool version, and normalized
comparison rule.

TeX Live packages, fonts, TFM files, encoding files, and map files are treated
as document inputs. General-purpose libraries such as zlib may be used under
their own licenses; importing an engine implementation through a library is
not permitted.

## Decision record

Every compatibility rule that is not evident from a public specification must
receive a short record containing:

1. the controlled input;
2. the reference command and version;
3. the observed outputs;
4. the user-visible rule inferred from those outputs; and
5. the independently chosen HSTeX representation or algorithm.
