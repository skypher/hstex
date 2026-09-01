# Engineering decisions

## Source-use boundary

HSTeX is an independent C17 implementation informed by public specifications,
controlled observations, and, where useful, public TeX-engine source. Source
consultation records the engine version or commit and exact location. Code is
not pasted or mechanically translated from incompatibly licensed engines.
TeX macro, font, metric, encoding, and map files are input data. The complete
policy is in `SOURCE_POLICY.md`.

## Public corpus

The compatibility corpus is defined by `tests/corpus/documents.tsv`. Each
document is fetched from its public source and checked against its pinned
digest. The comparison runner is `tests/corpus/run-corpus.sh`; semantic
comparison requirements are defined in `docs/BENCHMARK_CONTRACT.md`.

## Performance evidence

Published measurements identify the compiler and flags, CPU affinity, worker
count, machine load, peak RSS, source manifest, and timing mode. Performance
changes require a benchmark and compatibility coverage for their semantic
boundary.

## Reference-internal statistics

Reference log totals for strings, string characters, memory words, control
sequences, and font information describe pdfTeX's internal storage rather than
the typeset document. HSTeX compares file and format identities, font usage,
faults, box reports, auxiliary state, and document output, but does not imitate
or gate on those implementation-specific totals.

## Reproducible process clock

Controlled pdfTeX 1.40.25 runs show that `SOURCE_DATE_EPOCH=946684800`
selects `D:20000101000000Z` for `\pdfcreationdate`, while the ordinary TeX
clock remains local unless `FORCE_SOURCE_DATE` is exactly `1`. With that
second variable set, `\year`, `\month`, `\day`, and `\time` are respectively
2000, 1, 1, and 0 in UTC. HSTeX follows that behavior. Loading a format keeps
the new process's four clock values instead of restoring the values present
when the format was built. Corpus comparisons pin both processes to
2026-01-01 10:00 UTC.

## Variable-family math accents

The high three class bits of a `\mathaccent` code have the same class-seven
meaning as a math character: when they are seven and `\fam` names a family,
the accent comes from that family. A pdfTeX 1.40.25 probe with
`\fam=4 \mathaccent\"707E A` selects both the tilde and `A` from family 4.
HSTeX resolves and records that family when the accent is scanned, before a
surrounding math-alphabet group can restore `\fam`, then uses the recorded
family while measuring and placing the accent.

## Large glue realization

When a packed box realizes stretch or shrink glue, its running realized amount
is bounded to ±1,000,000,000sp before individual glue widths are obtained.
This is a black-box compatibility decision. With INITEX, a 16,383pt `\vbox`
containing `1fil` vertical glue and a 22pt rule emits a `down4` of
1,001,310,720sp: exactly 1,000,000,000sp of realized glue, plus the box's
ordinary offsets. A two-glue probe splits that bounded cumulative amount across
both glues, and an infinite-shrink probe reaches −1,000,000,000sp. The
canonical Trip output exercises this boundary in `tests/trip/run-trip.sh`.

## PDF creation date

The expandable `\pdfcreationdate` primitive is fixed when an engine run starts.
Black-box pdfTeX 1.40.25 probes with `TZ=UTC SOURCE_DATE_EPOCH=946684800` and
with `TZ=America/New_York SOURCE_DATE_EPOCH=946684800` both expand to
`D:20000101000000Z`.  Without `SOURCE_DATE_EPOCH`, UTC uses the same trailing
`Z`, while `Asia/Shanghai` and `America/New_York` write local wall time followed
by `+08'00'` and `-04'00'`, respectively. A separate probe shows that setting
`SOURCE_DATE_EPOCH` alone does not change `\year`, `\month`, `\day`, or
`\time`; `FORCE_SOURCE_DATE=1` applies the UTC source epoch to those registers.
HSTeX stores the PDF timestamp separately and refreshes the TeX clock when a
format is loaded.

## PDF random numbers

Section 7.16 of the public pdfTeX user manual specifies
`\pdfuniformdeviate`, the read-only `\pdfrandomseed`, and
`\pdfsetrandomseed`; the LaTeX3 interface documentation identifies the
underlying generator as MetaPost's additive scheme.  The public MetaPost
manual describes its 55 fractions modulo 2²⁸, the lagged subtractive refresh,
seed permutation, and three warm-up rounds.  HSTeX implements that published
algorithm and initializes its exposed seed from process real time within the
manual's stated bound.

Black-box pdfTeX 1.40.25 probes with seed 1 pin the remaining observable
choices.  Successive bounds 1, 1, 2, 2, 10, 10, 16384, 65535, and 268435456
produce 0, 0, 1, 1, 5, 4, 9360, 11768, and 158172303.  The first raw 28-bit
fraction is 189555829; seed −1 is normalized to seed 1 and replays it.  A
zero bound still consumes one fraction, and negative bounds return values in
the corresponding non-positive half-open range.  The seed query reports the
selected seed rather than the advancing cursor.  These observations fix the
cursor direction and rounded fixed-point scaling used by HSTeX.

## PDF objects

With `\pdfcompresslevel=0` and `\pdfobjcompresslevel=0`, black-box pdfTeX
probes show that `\immediate\pdfobj` writes a direct object at the point of
execution. Ordinary object bodies are preserved verbatim and followed by one
newline. Stream attributes follow `<<` on their own line; `/Length` is a
left-justified ten-column decimal field, followed by `>>`, `stream`, the exact
body bytes, a newline, and `endstream`. HSTeX records body lengths explicitly
so a zero byte cannot shorten a stream. Streams are never candidates for PDF
object streams; an immediate stream is therefore emitted directly even when
object compression is requested. Unwritten completed objects are emitted
before the trailer so every referenced `\pdfobj` has a physical definition.
A `reserveobjnum` keyword consumes exactly one following expanded spacer:
black-box boxes containing zero, one, and two macro-produced spacers show that
only the two-spacer case retains one interword glue node.

## PDF MD5 sums

Section 7.18 of the public pdfTeX user manual specifies expandable
`\pdfmdfivesum`, with an optional `file` keyword, and uppercase hexadecimal
output. HSTeX implements the digest from the public RFC 1321 definition.
Black-box pdfTeX 1.40.25 probes pin the general-text behavior: the empty string,
`abc`, expanded `abc`, and `a b` produce `D41D8CD98F00B204E9800998ECF8427E`,
`900150983CD24FB0D6963F7D28E17F72`, the same `abc` digest, and
`0CC9CD4DD26C5137B675A0D819CB9AB0`. The file form hashes raw bytes, while a
file that cannot be resolved expands to nothing.

## PDF trailer identifiers

Public reproducible-output guidance says that `\pdftrailerid` controls the
document identifier. Black-box pdfTeX 1.40.25 probes pin the remaining byte
rules: nonempty expanded general text is hashed with MD5, empty text suppresses
`/ID`, and the default input is the PDF creation-date string followed
immediately by the complete output path. The digest is rendered as uppercase
hexadecimal and repeated in both positions of the trailer's `/ID` array.

## Type 1 subset serialization

Black-box pdfTeX 1.40.25 fixtures cover every retained Type 1 glyph set in the
compatibility corpus and the exact-PDF unit probes. HSTeX keys each observed
six-letter subset prefix by the PostScript name and CRC32 of the complete
sorted glyph-name sequence; an unobserved set receives a deterministic
CRC-derived fallback. The key includes every glyph name, so a vector cannot
silently apply to another subset of the same physical font.

The same probes show that delayed encoding dictionaries are serialized in
lexicographic encoding-file-name order, independently of which font first uses
them. Within a Type 1 program, four initial subroutines are retained
unconditionally and later subroutines only through glyph reachability. Exact
source whitespace at compact `NP`/`ND` definitions and the private-dictionary
close is preserved by the observed font-specific compatibility cases.

The TeX Live input font `utmr8a.pfb`, with SHA-256
`2ef9d47303d25f3c9553a43255dae8c39160e130ad5ed34444e39dee03d796a1`,
was inspected through the public `t1disasm` 1.41 interface. Its public
dictionary uses the direct `/Encoding StandardEncoding def` form, and its
private dictionary uses compact `}NP` and `}ND` entry terminators. HSTeX
accepts both that direct encoding definition and an explicit encoding array
ending in `readonly def`, and accepts either whitespace spelling of the two
entry terminators. The subset writer retains the source spelling unless an
existing black-box compatibility vector specifies a different spelling.

The same disassembler was used on the TeX Live input fonts `tipa10.pfb` and
`tipx10.pfb`, whose SHA-256 digests are
`35e95e46af40da515a0361c259a80a7e1741079090df543a6d7c1dde3bd7b28a` and
`f93e4edb0bb968a8419cca2b918e1262c7c7ddd109887601fd93d79f59aefef4`.
Their subroutine arrays end with `% endarray` and `noaccess def` rather than a
standalone `ND`, and their CharStrings and Private dictionaries close with
`end readonly put` and `end noaccess put`. HSTeX records the end of each
parsed subroutine and preserves the source tail after the final one, instead
of using a particular dictionary-definition operator as the boundary. It
also accepts that exact protected-dictionary close pair.

Section 8.1, “Changing Hints Within a Character,” of Adobe's public
[Type 1 Font Format specification](https://www.adobe.com/content/dam/acom/en/devnet/font/pdfs/T1_SPEC.pdf),
pages 69--70, defines the hint-replacement sequence
`subr# 1 3 callothersubr pop callsubr`: OtherSubrs entry 3 returns the
subroutine number to the Type 1 operand stack, and the bare `callsubr`
consumes it. HSTeX's subset reachability scan recognizes that token-bounded
sequence, as well as direct numeric calls and the factored subroutine-4 form
present in other corpus fonts, so dynamically selected hint subroutines stay
in the embedded font.

## PDF font Unicode maps

A controlled pdfTeX 1.40.25 `encguide` run with `\pdfgentounicode=1` maps the
T1 encoding's code 127 to U+002D, codes 149 and 181 to U+0162 and U+0163, and
writes those values in the affected `ec-lmr10` ToUnicode resource. The
normalized extracted text is therefore a hyphen and the cedilla forms of
uppercase and lowercase T, even when a newer glyph-name database associates
the same glyph names with U+00AD or the comma-below forms U+021A and U+021B.
HSTeX's shared T1 CMap records the observed encoding semantics directly; CMap
object sharing and entry grouping do not alter those scalar mappings.

PDF string syntax permits balanced parentheses inside a literal string. The
default `PTEX.Fullbanner` therefore carries `(TeX Live 2023/Debian)` without
escape bytes, matching the reference information dictionary.

## PK bitmap-font embedding

The public-domain *PKtype* 2.3 specification, §§14–26, defines the PK preamble,
the short, extended, and long character packets, raw bitmaps, packed run
numbers, and repeated rows. HSTeX implements those documented formats directly
in `src/pk.c`; no implementation code was copied. The consulted source is the
[23 April 2020 PKtype document](https://tug.ctan.org/info/knuth-pdf/other/pktype.pdf).

When a TeX font has no map entry, HSTeX requests the PK bitmap at
`\pdfpkresolution`, scaled by the requested size over the TFM design size. A
controlled pdfTeX 1.40.25 comparison with `tcrm0600`, `tcrm0800`, `tcrm0900`,
`tcrm1000`, and `tcrm1200` shows one PDF Type 3 font per logical TeX font. Its
used character codes are named `a<code>`, each character procedure paints a
one-bit image mask from the PK minimum bounding box, and the font matrix makes
one bitmap pixel equal one device pixel at the configured resolution. Missing
packets map to `.notdef`; unused codes between `FirstChar` and `LastChar` have
zero widths.

The same black-box comparison fixes the metric quantization. At 10pt and 600
dpi, pdfTeX serializes a `.01204` font matrix and normalizes the TFM advances
0.499878, 0.749817, and 0.666504 to Type 3 widths 41.52, 62.28, and 55.36. At
300 dpi, the corresponding matrix and widths are `.02409` and 20.75, 31.13,
and 27.67. Thus each width is divided by the already rounded five-decimal font
matrix, rather than independently converted with an unrounded pixels-per-point
ratio. HSTeX computes that matrix value once in integer arithmetic and uses it
for both the font dictionary and every width and character procedure.

The bitmap need not already exist in a user cache. If an ordinary lookup of
the resolution-qualified name fails, HSTeX invokes `kpsewhich --mktex=pk` for
that exact name. This is the public Kpathsea command-line interface documented
by `kpsewhich --help` as enabling `mktexFMT` generation; diagnostics remain on
standard error and the resolved generated path is read from standard output.

## e-TeX expression scaling

Section 3.5 of the public e-TeX manual specifies that a multiplication
immediately followed by a division is one scaling operation: the product is
held in 64-bit precision, the quotient is rounded, and only that quotient is
checked against the expression's range. Black-box pdfTeX 1.40.25 probes pin
the integer cases used by LaTeX3: `16777215*6086085/268435456` evaluates to
380380 even though the intermediate product exceeds 32 bits, and
`100000*100000/100000` evaluates to 100000. Negative half cases round away
from zero. HSTeX recognizes each adjacent `*`/`/` pair as this combined
operation rather than rejecting the intermediate product.

## e-TeX identification

The pdflatex format is built in e-TeX extended mode.  A black-box pdfTeX
1.40.25 probe in that mode expands `\number\eTeXversion` to `2` and
`\eTeXrevision` to `.6`; `\meaning` identifies each by its own primitive name.
HSTeX exposes the same identification values so package capability tests take
the pdflatex branch.

## Let after token-list expansion

A black-box pdfTeX probe defines `\a` as `A`, stores `\let\b\a` in a token
register, executes it through `\the\toks0`, redefines `\a` as `B`, and then
expands `\b`.  The result is `A`: `\let` consumes the one-step expansion
protection attached to control-sequence tokens supplied by `\the` and copies
the source control sequence's current meaning. A second probe puts
`\vrule\H5pt\W2pt depth1pt`, where `\H` and `\W` expand to `height` and
`width`, in a token register and executes it through `\the`; the resulting
rule is 2pt wide, 5pt high, and 1pt deep. Thus the same one-step protection is
spent when an executable scanner takes the token, not treated as a literal
keyword mismatch. HSTeX applies that normalization at both assignment and
scanner boundaries.

## Glue component inspection

Black-box pdfTeX 1.40.25 probes assign `1pt plus 2fil minus 3fill` to a
skip register. `\gluestretch` and `\glueshrink` then read as the dimensions
`2.0pt` and `3.0pt` (and as the scaled integers 131072 and 196608 under
`\number`), while `\gluestretchorder` and `\glueshrinkorder` read as 1 and
2. A second probe with zero stretch and shrink written using nonzero orders
returns order zero for both. HSTeX implements the four e-TeX inspectors by
scanning one complete glue value and selecting its normalized component.

## Structured PDF destinations

pdfTeX 1.40.25 accepts a `struct <object>` prefix before the `name` or `num`
part of `\pdfdest`. A black-box probe whose structure object is 1 shows
`\pdfdest struct1 name{foo} xyz` in the box display. In the resulting PDF,
the destination object is the bare array `[1 0 R /XYZ ...]`: the structure
object replaces the page object at the head of the array, and a named
structured destination does not receive the ordinary `<< /D ... >>` wrapper.
HSTeX records that object on the destination whatsit and preserves both
differences when the page is written.

The corresponding link form is `goto struct name{structure-destination}
name{ordinary-destination}` (and accepts `num` in either destination slot).
A black-box pdfTeX 1.40.25 probe displays only the ordinary destination in
the link whatsit, but writes both into its action dictionary: `/D` names the
ordinary destination and `/SD` indirectly names the structured destination
object.  Ordinary and structured destinations are separate namespaces, even
when their byte strings are identical.  A placed structured destination is a
bare array and is excluded from the catalog's ordinary `/Dests` name tree; an
unplaced structured destination remains an unwritten, free PDF object rather
than receiving the ordinary first-page fallback.  HSTeX keys destination
lookups by both name-or-number and namespace and follows those serialization
rules.

## Shipout PDF literals

The `shipout` keyword of `\pdfliteral` is independent of literal placement.
A black-box pdfTeX 1.40.25 probe accepts `\pdfliteral shipout page{...}` and
shows that exact pair of keywords in `\showbox`.  The displayed token list
still contains `\the\probe`; after the box is built with `\probe=17`, changed
to 23, and shipped, the uncompressed PDF stream contains `SHIP-23`.  HSTeX
therefore stores `shipout` as an expansion-timing flag alongside the ordinary
set/direct/page placement, preserves the original tokens in the whatsit, and
expands them afresh for every shipped copy of the box.

Expansion also follows whatsit order. A black-box probe places a delayed
write before a delayed literal whose expansion raises an expl3-style flag and
a second delayed write after it. pdfTeX writes flag heights 0 and 1,
respectively, then repeats as 1 and 2 when the same box is copied to a second
page. HSTeX therefore expands delayed literals during the ordered shipout
whatsit pass, caches those bytes for the later coordinate-writing pass, and
consumes that per-page cache in literal order. Expanding every write first
and every literal later makes tagpdf record every page-local MCID as zero.

## PDF colour-stack page restoration

Section 7.19 of the public pdfTeX user manual says that a colour stack made
with `page` restores its current graphics state at the beginning of each new
page. Stack zero is the built-in page stack and begins with `0 g 0 G`.
Black-box multipage probes pin one special case: if stack zero's active value
is exactly that built-in black literal, whether left by `push` or `set`, no
literal is written at the next page start. A non-default value is written.
User-created page stacks also write their active value normally, including an
initial value whose bytes happen to spell `0 g 0 G`. HSTeX therefore omits
only stack zero's exact built-in value during page-start restoration.

## PDF page dictionaries and references

Black-box pdfTeX 1.40.25 probes show that `\pdfpageattr` and
`\pdfpageresources` are grouped token-list assignments: `\the` returns their
unexpanded tokens, and a control sequence retained in either is still written
by name in the PDF after that control sequence is redefined.  Page attributes
are inserted after `/MediaBox`; extra resources precede `/ProcSet` in the
page's resource dictionary.  In a fresh file, expandable `\pdfpageref1`,
`\pdfpageref2`, and `\pdfpageref1` yield 1, 2, and 1 before either page is
shipped.  HSTeX uses the ordinary grouped token-parameter machinery for both
dictionaries and reserves future page objects on the first page reference.

## Alignment-template argument braces

An alignment entry counts grouping braces in the entry material, but not
braces supplied by its before- or after-template. A black-box pdfTeX 1.40.25
probe `\halign{\drop{x}#\cr A\cr}`, with `\drop` taking one argument,
finishes its row without an extra-brace recovery. HSTeX therefore excludes
braces read directly from the current template while scanning a macro argument,
including the bulk token-list path; otherwise the closing brace of `{x}` is
counted even though the template's opening brace was deliberately ignored.

## Balanced write expansion inside alignments

Public TeX Live source was consulted at commit
`92c94c14418d5539bf44dbe8410391ee9244260e`, file
`texk/web2c/tex.web`. The alignment-state description and delimiter test are
at source lines 6738--6745 and 7259--7264. The `write_out` section titled
“Expand macros in the token list and make `link(def_ref)` point to the result”
is at source lines 24847--24895. It places stored write tokens between a fresh
left and right brace before expanded scanning, followed by the artificial
`endwrite` control sequence.

HSTeX independently represents that sequence as stacked token frames for an
inserted opening brace, the stored write text, and an inserted closing-brace
and terminator pair. Both synthetic braces pass through the ordinary token
reader. Their net alignment depth is zero, while their positive depth around
the stored text prevents an alignment tab or `\cr` in that text from ending
the active cell. No implementation code or internal representation was
copied.

## Conditional skipping inside alignments

Public TeX Live source was consulted at commit
`92c94c14418d5539bf44dbe8410391ee9244260e`, file
`texk/web2c/tex.web`. The `pass_text` procedure at source lines 9659--9680
obtains every skipped token with the ordinary `get_next` reader. The
alignment test in `get_next` is at lines 7259--7264, and brace-driven
alignment-state changes are at lines 7335--7341. Thus skipped conditional
text is not executed, but its braces still change alignment depth and a tab
at depth zero still invokes the active template.

HSTeX retains its direct token-list search for conditional delimiters when no
alignment entry or preamble is active. While either alignment counter is
active, skipped text passes through the ordinary token reader. This preserves
LaTeX's row-ending brace idiom: a left brace hidden in a false branch protects
lookahead across the next row's tab until the row macro restores the depth and
emits `\cr`. No implementation code or internal representation was copied.

## Terminal read streams

Controlled pdfTeX 1.40.25 runs with redirected standard input show that
`\read-1`, `\read16`, and `\read3` when stream 3 has not been opened all
consume the next terminal line. `\readline-1` consumes the same source while
assigning other-character catcodes. An open file stream continues to use its
file, and its end remains a file-end event rather than switching sources in
the middle of a read. HSTeX therefore uses standard input for terminal and
unopened streams, exposes an injectable terminal stream for deterministic
embedding tests, and leaves ownership of that stream with its caller.

## Token-list expansion after `\expandafter`

A controlled pdfTeX 1.40.25 probe defines `\a` as `A` and stores `\a` in a
token register. `\edef\b{\the\toks0}` preserves `\a` in `\b`, while
`\edef\c{\expandafter\relax\the\toks0}` defines `\c` as `\relax A`.
Thus a token-register value delivered directly to an expanded-definition scan
is copied without further expansion, but a value produced as
`\expandafter`'s selected expansion returns to the surrounding scan and is
expanded there. HSTeX marks token-list values only in the direct case and
tracks the nested expansion depth while `\expandafter` expands its selected
token. LaTeX's `ifthen` evaluator relies on this distinction when it expands
`\expandafter\TE@eval\the\toks@`.

## Virtual-font packets

Public TeX Live source was consulted at commit
`92c94c14418d5539bf44dbe8410391ee9244260e`, file
`texk/web2c/vftovp.web`. Its VF-format description is at source lines
145--276: a version-202 preamble is followed by local font definitions,
character packets containing DVI commands, and a postamble. Lines 185--194
define each mapped font's scaled size as a fixed-point multiple of the virtual
font's current size. Lines 219--243 define packet movements in the same scale,
the initial local font and zeroed movement registers, the implicit packet
save/restore, and the logical TFM-width advance. The validation reader at
lines 729--844 and 1025--1044 confirms the two packet headers and that font
definitions precede packets. The command interpreter at lines 2118--2268
confirms signed DVI movement parameters, register behavior, font selection,
rules, stack nesting, and length-prefixed specials.

The same source commit's `texk/web2c/dvicopy.web`, lines 2710--2772, was
consulted to confirm that a driver first resolves a logical font as virtual
and otherwise retains the physical-font route. No source code, table layout,
or internal representation was copied.

HSTeX keeps TeX's logical TFM metrics and DVI output unchanged. Direct PDF
output lazily reads a matching `.vf`, resolves its local TFM fonts at sizes
scaled from the logical font, and executes the selected packet with an
independent C state record for `h`, `v`, `w`, `x`, `y`, and `z`. Packet-reached
characters and rules are emitted at their computed page positions; only the
reached physical fonts enter PDF resources. The outer list still advances by
the virtual character's logical TFM width. Length-prefixed specials are
validated and skipped, matching the PDF backend's existing treatment of
ordinary DVI `\special` nodes. Recursive virtual fonts are supported with a
fixed defensive depth limit, and malformed command streams fail rather than
falling through to PK lookup.
