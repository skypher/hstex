# HSTeX pdflatex overlay

`hstex-pdflatex` is the supported command-line entry point for running a
LaTeX document with HSTeX. It is an overlay on an existing TeX Live or
MacTeX installation: HSTeX supplies the engine and keeps its own native
format cache, while the installed distribution supplies `latex.ltx`, package
inputs, fonts, metrics, encodings, and maps.

The overlay never reads a pdfTeX `.fmt` file and never bundles TeX Live
implementation material. It resolves inputs with `kpsewhich`, the same
public lookup interface used by the engine.

## Install

The first release target is Linux x86-64. Install the release tarball, then
place its `bin` directory on `PATH`:

```sh
tar -xzf hstex-VERSION-linux-x86_64.tar.gz
export PATH="$PWD/hstex-VERSION-linux-x86_64/bin:$PATH"
hstex-pdflatex --version
```

HSTeX needs a TeX installation that provides `kpsewhich`, `latex.ltx`, and
`pdftexconfig.tex`. On Debian and Ubuntu, `texlive-latex-base` supplies that
base. The HSTeX package does not replace the rest of a user's TeX Live or
MacTeX installation.

## Use

```sh
hstex-pdflatex report.tex
hstex-pdflatex -output-directory=build -jobname=report report.tex
```

The driver builds `pdflatex.hfmt` once and then starts document passes from
that native format. By default it stores the cache below
`$XDG_CACHE_HOME/hstex`, or `~/.cache/hstex` when `XDG_CACHE_HOME` is unset.
Set `HSTEX_CACHE_DIR` or pass `--format-cache=DIR` to choose another root.
Pass `--rebuild-format` after a local format-input change that does not update
the TeX file database.

The cache key includes the HSTeX version, the resolved `latex.ltx` and
`pdftexconfig.tex` contents, TeX search environment, and the identities of
the TeX `ls-R` databases. A TeX Live package update that refreshes `ls-R`
therefore creates a fresh native format automatically.

`HSTEX_ENGINE=/absolute/path/to/hstex` selects a particular engine binary,
which is useful for testing an unpacked build alongside an installed release.

## Supported command-line contract

| Option | Status |
| --- | --- |
| `-output-directory=DIR`, `--output-directory=DIR` | Supported |
| `-jobname=NAME`, `--jobname=NAME` | Supported |
| `-interaction=errorstopmode` | Supported; this is HSTeX's current interaction mode |
| `-halt-on-error`, `-file-line-error` | Supported |
| `-no-shell-escape` | Supported; disables the default TeX Live restricted-command profile |
| `-output-format=pdf` | Supported |
| `--format-cache=DIR`, `--rebuild-format` | HSTeX extensions |
| Any other pdfTeX option | Rejected with exit status 2 |

Successful runs write `JOBNAME.pdf` and `JOBNAME.log` under the selected
output directory, together with whichever TeX auxiliary files the document
requests. The log records the engine output for that pass.

HSTeX does not silently invoke pdfTeX or fall back to it. A rejected option or
unsupported document behavior is reported by HSTeX, so a successful run is an
HSTeX result rather than a reference-engine result.

By default the driver uses the active TeX installation's
`shell_escape_commands` allowlist. Allowed commands are started directly with
parsed arguments, without passing document text to a command shell.

## Compatibility and performance evidence

Each release must pass the canonical two-pass TRIP comparison, the public
semantic corpus, and the ordinary engine test suite. A release benchmark must
state the exact TeX tree, source manifest, compiler and flags, CPU affinity,
worker count, machine load, peak RSS, and the final-pass and fresh
three-pass-plus-BibTeX timings separately. See `docs/BENCHMARK_CONTRACT.md`.

The current supported semantic surface is the one exercised by the release
tests and public corpus. The command-line driver is intentionally strict
outside the table above so build systems do not mistake an ignored switch for
pdflatex compatibility.

The native-format smoke gate currently uses Ubuntu 24.04's TeX Live 2023
installation. The Ubuntu 22.04 TeX Live 2021 `latex.ltx` enters a document
transition while creating its format, so it is rejected rather than cached as
a valid HSTeX native format.
