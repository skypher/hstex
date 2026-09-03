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

## Finding a file

Kpathsea 6.3.5 documents `kpsewhich -interactive` as asking for additional
filenames on standard input. HSTeX keeps one such process for unresolved names
instead of launching one process per question. Its end-of-answer marker is
`latex.ltx`, which the engine's already loaded `ls-R` data can verify without
another process. The marker's resolved path remains private to the lookup
protocol and does not determine HSTeX's storage or search representation.

A black-box pdfTeX 1.40.25 probe first opens an absent name for input, closes
it, creates that name with an immediate output stream, and inputs it in the
same process directory. The probe reports the initial miss and then reads the
new definition. HSTeX therefore checks ordinary local inputs before cached
installation answers. Its configured artifact directory receives the same
direct check for files produced by the driver. An engine output increments
the generation used to reconsider a cached negative answer, but it does not
restart the persistent installation finder. An allowed restricted-shell
command increments a separate external generation because it can modify any
search directory, so the next unresolved lookup starts a fresh finder.

The restricted-shell mode remains immediately observable as 2 through
`\pdfshellescape`, but the public `shell_escape_commands` value is queried
only when a syntactically valid `\write18` command reaches execution. A unit
probe pins that delayed load and the existing allowed/disabled outcomes.
Another probe caches a missing input, creates it, reads its contents, and
checks that the finder process identifier is unchanged. The GCC release suite,
the 14-document strict corpus, and the six-document stress corpus all pass
with the change.

The marker is verified by the finder it starts rather than by a process of
its own. `kpsewhich -interactive latex.ltx` answers its command-line name
before it reads anything from standard input, so the first line the finder
says is the marker's answer: a finder that says nothing there is a finder
that cannot be talked to this way, which is what the separate check was
looking for. The check could not be skipped on the strength of the `ls-R`
data alone, because a stock TeX Live 2023 tree lists `latex.ltx` twice --
under `tex/latex/base` and `tex/latex-dev/base` -- and a name held twice is
one the lookup declines to settle. Measured on that installation, the
separate check ran on every LaTeX run and cost 11.1 ms of a 71.9 ms
`small2e`. Both strict corpora agree with the reference without it.

## The trees a format remembers

The lookup needs to know which trees keep an `ls-R`, which is the value of
`TEXMFDBS`. Asking `kpsewhich` for it costs a child process that reads and
hashes those same lists to answer a question about configuration: measured
on TeX Live 2023, 9.7 ms, paid by every run including plain ones that ask
nothing else.

A format records the value its build was given, and a run starting from that
format is offered it instead of asking. What makes this sound is that
`hstex-pdflatex` already keys its format cache on the `TEXMFDBS` string and
on the device, inode, size and modification time of every `ls-R` it names, so
a format is reused only where those are unchanged. For a format used directly,
the offer carries a stamp over the same sizes and modification times, checked
before the list is believed; a stamp that no longer matches falls back to the
child process. A list that is wrong in the other direction costs nothing
either: the lookup answers only names it can settle unambiguously, so a name
absent from a stale list, or held twice across one, goes to the tool exactly
as it did before.

Measured over the strict corpus, the trees a format remembers and the marker
above together take `small2e` from 71.9 ms to 49.7 ms and `gentle` -- which
asked for the value and nothing else -- from 52.1 ms to 41.6 ms.

## A format is read where it lies

A format was read by taking its length, allocating that many bytes, and
reading the file into them, after which every record was copied a second
time out of that buffer and into the room the engine keeps it in. The first
copy buys nothing: the reader already takes one record at a time, and asking
for thirteen megabytes of fresh anonymous memory makes the kernel clear
pages that are immediately overwritten. Annotating a run of an empty LaTeX
document put the read and the faulting it caused at about a seventh of the
whole run.

The file is mapped instead, through the same `src/input.c` that maps a
document, and the records are taken straight out of the mapping. Nothing
retains a pointer into it -- every array is copied into the engine's own
storage as before -- so the mapping is closed as soon as the format has been
read. Measured on TeX Live 2023, loading the 13 MB LaTeX format fell from
22.9 ms to 17.1 ms, and both strict corpora agree with the reference.

## Switching on a node without waiting for its copy

The four loops that walk a finished list -- two writing DVI, two writing PDF
-- take each node by value, because what they call while holding it may move
the arena the node lives in. The switch that follows then read the kind back
out of the copy just written to the stack, and the branch had to wait for
those stores to reach memory and come back. Annotating a `gentle` run put
that one comparison and its branch at more than half of what the DVI walker
cost, which was itself the largest single cost in the run.

The kind is now read from the arena directly, as a separate load, and only
the switch uses it; the cases still read the copy, so nothing holds a pointer
across a call that could move the arena. The copy and the branch then have no
dependency between them. `gentle`, the corpus's one long plain document, went
from 52.1 ms to 41.6 ms with this and the tracing gate below, against the
reference's 40.6 ms.

## What a run that traces nothing pays

`\tracingcommands` is off in every run that is not being looked at, and the
fifteen places that trace a command sit on the path every command takes.
The test was inside the traced function, so a run that traces nothing still
made the call: 0.75% of a corpus run, spent deciding to do nothing. The test
is now an inline gate on the parameter, which is a field already in cache,
and the body it guards is unchanged.

Separately, the filename database sized its hash table by doubling from
32,768 slots, rehashing everything held at each step. A stock installation
holds about forty thousand names, so half of them were hashed twice over
before the last was in. The lists are mapped before they are parsed, so the
number of lines in them -- an upper bound on the names they hold -- is
counted first and the table is sized once.

Both strict corpora agree with the reference with all of these in place.

## Register banks are written as far as they are set

An engine accepts some thirty-two thousand of each kind of register, and the
banks that hold them -- counts, dimens, glues, muglues, token registers,
boxes, and a level beside each -- were written to a format at their full
length whatever a format had put in them. Measured on a stock LaTeX format,
that was 3.4 MB of a 13.0 MB file, nearly all of it zero, which is most of
why the file compressed to 988 KB.

A fresh engine callocs those banks, so an element that is all zero is one the
reader would have made for itself. The writer now finds how far into the
banks anything has been set -- judging an element by the same holes it clears
before writing, so that padding a compiler left is not mistaken for content
-- and writes one length and that many elements of each bank. The reader
callocs the full capacity and reads the prefix into the front of it, so what
the engine sees afterwards is what it saw before, and the rest of the bank
costs neither a copy nor a page.

The file's layout changed, so the magic is `HSTEX format 3`: a format written
by an earlier build is refused with the message that already exists for one
whose records are laid out differently, rather than being misread.

The LaTeX format falls from 13,024,969 to 9,643,737 bytes, a quarter smaller,
and peak RSS on `small2e` from 27.2 MB to 24.0 MB. The effect on warm
document-pass timing is not distinguishable from run-to-run noise -- 0.6%
over the corpus, with individual documents moving either way -- because the
pages this saves were being touched once and never read. It is recorded here
as a size and residency change, which is what it demonstrably is.

## Where a body is kept

A definition's body is a run of tokens, and the engine keeps runs in free
lists by length: the room a body is given is the least power of two that
holds it, never fewer than eight, so that a body can be given back knowing
only how long it is. Nothing carries a header saying how big it is, which is
what makes `token_block_free(body, count)` enough.

That rule is why a format's bodies were read one allocation at a time and
rounded up the same way: a body the pool might later be given must be exactly
as long as the pool will assume. A stock LaTeX format holds 46,251
definitions, so reading one meant some ninety thousand allocations before a
document had been looked at, each rounded up -- a body of nine tokens taking
sixteen.

A format's bodies are now cut from a few blocks the engine keeps, and the
definition that holds one says which of its two bodies came from there. The
bit sits in a byte `struct hstex_macro` already had spare, so the record is
no wider and the file no different -- it is written as zero and settled again
on the way in, because where a body was cut from is this process's business
and not the file's. The two places that give a body back, `release_definition`
and the engine's teardown, ask first: a borrowed body goes back to neither
the pool nor the library, and the blocks are freed with the engine.

Because a borrowed body is never given back, it need not be rounded to a
length the pool would recognize, and is cut at exactly the size it needs. The
cost is that a definition a run redefines does not return its room until the
run ends; that is bounded by the format, which the run is holding anyway.

Measured on the corpus, thirteen of the fourteen documents got faster and one
moved 0.5% the other way, for 1.7% over the corpus -- aggregate 1.70x to
1.75x against pdfTeX, median 1.82x to 1.84x. `gentle` reaches parity with the
reference at 1.00x. Loading a format alone drops peak RSS from 24,576 KB to
23,680 KB and minor page faults from 3,952 to 3,692. It is a smaller effect
than the count of allocations suggested: glibc satisfies ninety thousand
small requests out of a heap it already has, so what was removed was
bookkeeping and rounding rather than work with the kernel.

## Stopping at the first error

`-halt-on-error` was accepted by the driver and then dropped on the floor: the
argument matched a list whose only action was `continue`, nothing was set, and
nothing reached the engine, which had no such mode to reach. A document with
three undefined control sequences therefore came out of `hstex-pdflatex
-halt-on-error` as a finished PDF and a zero exit, where the reference gives
one fault, no PDF, and a status of 1. `docs/COMPATIBILITY.md` listed the flag
as supported throughout, and `--help` advertised it. A build script gating on
that status passed a broken document.

The reference is the specification, and a probe of pdfTeX 1.40.25 pins it: the
error and its context line are printed and the help text after them is not,
because there is nobody being helped; the run then reports what it came to as
`!  ==> Fatal error occurred, no output PDF file produced!`, writes no output
file, and exits 1.

HSTeX now does the same. The engine takes the mode from
`HSTEX_HALT_ON_ERROR`, which the driver sets from the flag -- the engine's own
arguments are positional, so a variable is what there is room for. At the
first error `tex_error_with_help` prints the error and its context, skips the
help, and gives up the way the hundredth error already did; the end of the run
says what it came to; and what was written of an output file is removed, so
that "no output PDF file produced" is true of the directory and not only of
the log. The checkpoint cache is not written either: a cache of half a
document is not one a later run should resume.

`tests/pdflatex/run-driver.sh` holds it to all four of those -- a failing
status, no output file, the fatal line in the log, and no report of the error
after the stop.

WHAT IS STILL NOT DONE. The exit status says nothing about errors unless a run
halted, though the engine's own comment beside `history` says that history is
what the exit status is made of. The reference exits 1 for a document with
errors in any interaction mode; HSTeX exits 0. Making that true here means
`tests/corpus/run-corpus.sh` must stop reading a nonzero status from HSTeX as a
disagreement, since several stress documents are expected to report faults and
the reference is already run with its status ignored. That is a change to the
corpus runner as much as to the engine, and it is not made here.

## Every option the driver takes does something

An option the driver accepted and then ignored was as much a lie as one it
did not document. Three were being dropped -- `-interaction=errorstopmode`,
`-file-line-error`, and `-output-format=pdf` -- against a README that says
unsupported options are refused rather than silently ignored. Worse, the
`-interaction` values a build script actually passes were refused: only
`errorstopmode` was matched, so `-interaction=nonstopmode`, the commonest of
them, ended a run with "unsupported option" while the one mode this engine
cannot perform, since nothing here reads a terminal, was the one accepted.

Each now does what it says.

`-interaction` takes the reference's four modes and hands the named one to
the engine, which has had them all along and started in `errorstopmode`
regardless. A fifth name is refused by name -- `unknown interaction mode` --
rather than being rounded to whichever mode is nearest.

`-file-line-error` opens an error with the file and line it was met in
instead of with `! `, the context line under it unchanged. A probe of pdfTeX
1.40.25 pins the form, including that a file named without a directory is
reported as the reference opened it: `t.tex` on the command line comes back
as `./t.tex`, while a name that already says where it is keeps what it says.
The innermost *file* is what gets named, so an error inside a macro body
names the file the expansion came from rather than nothing at all.

`-output-format=pdf` is answered rather than ignored: it asks for what the
driver produces. `-output-format=dvi` asks for what it does not, and is
refused with everything else the driver has no answer for.

Both new flags reach the engine as environment variables, for the reason
`-halt-on-error` does: what the engine is given on its command line is
positional. `tests/pdflatex/run-driver.sh` holds all of it -- a mode that
works, a mode that is refused and says why, an error that names its file and
line, and no error still opening with `! ` when the flag is on.

## What makes a checkpoint cache warm

Two of the four documents the driver gate pinned came from the same
misunderstanding of what a run reads. A cache was judged still good by
hashing the document and the files somebody wrote beside it -- `.tex`,
`.sty`, `.cls` and their like. But a run reads more than that: the auxiliary
state a previous pass left is read back at the start of the next one, and
reading a different `.toc` is the whole reason a second pass differs from a
first. Leaving it out made a second run look warm when it was not, so pages
set before the table of contents existed were reused and the contents never
appeared.

Those files now count towards the hash, and they are also never a change an
incremental rebuild may reuse pages across: an edit to a source has a place
in the document and pages before it can stand, while what a pass left is read
before the first page is set and can move any of them.

Separately, a destination name restored from a checkpoint was allocated
without room for a terminator and left unterminated, where every other path
allocates `length + 1` and terminates. What reads a name reads it as a
string, so `technote` came back with `section.0.1?"{` in place of
`section.0.1`, and lost the bookmark that pointed at it. `technote` agrees
with the reference now and its pin is gone.

WHAT IS LEFT. `cfgguide` and `cyrguide` still differ, and not for either of
those reasons. A second pass over them run sequentially agrees with the
reference; the same pass run in parallel does not, whichever directory the
output goes to, so it is neither the cache nor where files are written. Chunk
workers open write streams the way the carrier does -- nothing tests
`parallel_is_worker` on that path -- so more than one process truncates a
file that one of them has still to read, and the `.toc` a chunk reads for
page one can be the empty one another chunk just opened. Deciding which
process owns an auxiliary write in a chunked run is a change to the design of
that feature rather than a repair to it, and is not made here.

## The path an installation actually takes

The public corpus drives the engine directly, `hstex --format`, one pass. That
is not what an installed HSTeX does. The supported command is
`hstex-pdflatex`, which builds a format cache and then compiles through the
checkpoint path -- `bool parallel = getenv("HSTEX_NO_PARALLEL") == NULL` -- so
every green corpus run was exercising code a reader never reaches, and the
code a reader does reach was gated by nothing.

Run that way, four of the eleven LaTeX documents disagree with the reference
given the same two passes. Three are the checkpoint path's own: `cfgguide`
and `cyrguide` come out with the wrong table of contents, and `technote` with
a corrupted named destination and a lost bookmark title; all three agree
under `HSTEX_NO_PARALLEL=1`, which is what places the fault. The fourth,
`usrguide-historic`, differs the same way with the checkpoint path off: it
drops a marginal note on the second pass, so it belongs to the ordinary
engine on a pass the one-pass corpus never reaches. All four reproduce on the
engine as it stood at `6870489`, before any of this session's work.

`tests/corpus/run-driver-corpus.sh` runs the corpus that way and holds each
document to `driver-expectations.tsv`. It is a CI job of its own: run as
another step on the build job it put that job past its forty-five minute
timeout, because it compiles every document twice on each side rather than
once. Pinning rather than skipping is what
makes it a gate: it fails whichever direction a document moves, so repairing
one of these is noticed as surely as breaking another, and the pin is deleted
when the document is fixed. A pinned `differs` records what comes out wrong
and where the fault lives, so that it reads as a finding rather than a
licence.

## A cached format a build cannot read

The driver keeps a built format under a key hashed from the HSTeX version,
the resolved `latex.ltx` and `pdftexconfig.tex`, the search environment, and
the identity of every `ls-R`. It decides to reuse one by `stat` alone --
it does not read the file -- so a format the engine would refuse was still
handed to it.

Nothing in that key described the shape of the file. Two builds of the same
version whose stream or record layout differed therefore computed the same
key, and the second was given the first's format and failed on it: measured,
`hstex-pdflatex` exited 1 with `pdflatex.hfmt is not a format` where a
rebuild was what the situation called for. `--rebuild-format` recovered it,
but only for someone who knew to ask.

The key now includes what makes a format readable: the name the file gives
itself and the widths of the records it carries, combined by
`hstex_format_identity`. Both moved into `hstex/engine.h` so that the driver
can read them without linking the engine. A format an engine could not read
now lies under a key that engine never looks in, so it is built afresh, and
the unreadable one is left where it is rather than being offered. Measured
across the two builds either side of the change: exit 1 with the error
before, exit 0 and a new key after.

## Why a run still starts one kpsewhich child

The remaining child was measured to see whether the lookup could be taught to
answer the names that start it. It cannot, on a stock TeX Live 2023.

Of the names a LaTeX run puts to the finder, two kinds could be answered
here: ones held twice in the lists, which the search path would settle
(`latex-dev` is not on it, so `tex/latex/base` wins), and ones absent from
lists that cover the whole of a searched path, as the `vf` path is. The kind
that cannot is a bitmap font: `tcrm1000.600pk` resolves under
`~/.texlive2023/texmf-var/fonts/pk`, a tree that is not in `TEXMFDBS` and is
reached by walking the disk rather than by reading an `ls-R`. A black-box
probe confirms the reference opens that same file, and `tcrm1000` has no
Type 1 to use instead, so the lookup is real work and not a wasted probe.

Six of the eight LaTeX corpus documents measured ask for such a name, so
answering everything else would move where the child starts without removing
it. Teaching the lookup to walk a non-`ls-R` tree would mean reproducing the
tool's own recursive search, which is what asking the tool avoids. The child
stays.

An exact fresh-directory process trace over `testmath` compared baseline
commit `fb3a1b8` with the candidate. The baseline used five lookup children:
the restricted-shell allowlist, `TEXMFDBS`, a marker lookup, and two persistent
finder instances separated by engine output. The candidate used two:
`TEXMFDBS` and one persistent finder. No command from the document was
executed.

The targeted warm-cache timing used GCC 13.3.0 C17 release builds with
`-O3 -flto=auto -DNDEBUG -fno-stack-protector -fno-plt
-fno-semantic-interposition`, pinned to CPU 3 of an AMD EPYC 7551 with one
font worker. One warm-up preceded eleven alternating baseline/candidate pairs,
each in a fresh output directory. Baseline times in milliseconds were
915.830, 905.822, 901.681, 931.808, 916.849, 901.149, 1007.709, 919.327,
939.778, 935.603, and 913.813; candidate times were 930.314, 895.950,
894.746, 943.777, 904.375, 885.869, 901.852, 903.236, 898.924, 941.840,
and 882.348. The medians were 916.849 ms and 901.852 ms, a 1.6% reduction;
the median paired reduction was 1.4%. Median user CPU time fell from 0.84 s
to 0.83 s and median system CPU time from 0.06 s to 0.05 s. Peak RSS was
28,672 KiB for every measured run. Load averages were 37.04/37.95/38.33
before and 36.95/37.87/38.29 after. The baseline and candidate executable
SHA-256 values were
`bf6ddc831a1c17c51a412820d09ab45bf450046e204635715518baa68daee602`
and
`94d6662775589a1efc6b26132a612ed0cf08ae40f650932fa89898b3206c88a6`.
The corpus manifest and input SHA-256 values were
`8681f4df7424f7ac585a7a508047eaf266751d3264b6f049781a961b3040a26f`
and
`9b311f1835266833ad40130e7a7a6361a950c965d308c02d567361e72ce74aa5`.
Every measured PDF had SHA-256
`1b9be60d6142c3bbe9bfad669e9863007034517b2e42dbfef44bf57233482def`.
This is a targeted result, not the full-corpus headline benchmark.

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

## PDF file modification dates

Section 7.18 of the public pdfTeX user manual specifies expandable
`\pdffilemoddate`, the PDF date syntax, and the reproducible-build UTC rule.
HSTeX resolves the expanded general text through the same public TeX file
lookup used by `\input` and `\pdffilesize`, reads the resulting file metadata,
and emits local civil time with a `+HH'MM'` or `-HH'MM'` offset. Zero offset is
written as `Z`. An unresolved file expands to nothing.

Black-box pdfTeX 1.40.25 probes against a fixed file pin the remaining forms:
the active `+08:00` zone produces `D:20260901173949+08'00'`, while `TZ=UTC`
produces `D:20260901093949Z`. When both `SOURCE_DATE_EPOCH` and
`FORCE_SOURCE_DATE` are present, HSTeX follows the manual and converts the
file's own modification time to UTC; their values do not replace that time.

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

Sections 6.2--6.4 and 7.1--7.3 of the same specification define Type 1
integer and command encodings, the charstring cipher with key 4330 and
`lenIV` prefix, and the eexec cipher with key 55665 and four-byte prefix.
HSTeX implements those encodings and cipher-feedback steps directly. Two
controlled runs of `t1asm` 1.41 over the same `cmr10.pfb` disassembly produced
identical bytes; decrypting the result showed zero-valued prefix bytes for
both cipher layers, a clear segment ending after `currentfile eexec`, and a
binary segment ending after `mark currentfile closefile`. HSTeX uses those
deterministic prefix values and emits those two PDF `FontFile` segments
without constructing an intermediate PFB file or launching an assembler.
A separate `t1asm` 1.41 run with `/lenIV -1` emitted the five encoded bytes of
the `.notdef` fixture directly, without prefix bytes or charstring encryption;
HSTeX treats that value as the same explicit unencrypted mode.

The input-side codec follows the same Adobe cipher and charstring sections.
For the PFB/PFA container boundary and canonical editable spelling, source
study used LCDF t1utils tag `v1.41`, commit
`e16fda51c46ee40c0d63af4b18a65e3070a99c87`: `process_pfb` and `process_pfa`
in `t1lib.c`; `decrypt_charstring`, `eexec_line`,
`disasm_output_binary`, and `disasm_output_ascii` in `t1disasm.c`; and
`set_lenIV` and `set_cs_start` in `t1asmhelp.h`. Those files are under the
MIT-derived Click License. They were used as an auditable behavioral source;
HSTeX uses its own bounded buffers, segment state, operator table, error
model, and C17 API, and incorporates no source or data from them.

HSTeX validates every PFB marker, kind, little-endian segment length, ordering
transition, and end marker. For PFA it distinguishes hexadecimal and binary
eexec by the first four payload bytes, accepts hexadecimal whitespace and
line wrapping, and rejects malformed or odd-length data. Both paths decrypt
the four-byte eexec prefix, locate the private-section close, discover the
font's charstring reader and `lenIV`, decode signed integer and operator forms,
normalize CR, LF, and CRLF at text boundaries, and omit the conventional
all-zero trailer lines. Unit vectors round-trip PFB, hexadecimal PFA, and
binary PFA containers back to identical assembled programs.

The differential audit used t1utils 1.41-4build3 and every one of the 348 PFB
fonts in the installed TeX Live 2023 Type 1 tree. HSTeX and `t1disasm` 1.41
produced byte-identical editable output for all 348, with no rejected font or
unknown operator. Converting each font to PFA with `t1ascii` 1.41 and repeating
the comparison produced another 348/348 byte-identical results. A process
trace over `testmath` then showed no `t1disasm` or `t1asm` execution, while the
PDF retained SHA-256
`1b9be60d6142c3bbe9bfad669e9863007034517b2e42dbfef44bf57233482def`.

The targeted warm-cache `testmath` timing compared baseline commit `403bf47`
with the in-process reader candidate. Both were GCC 13.3.0 C17 release builds
using `-O3 -flto=auto -DNDEBUG -fno-stack-protector -fno-plt
-fno-semantic-interposition`, pinned to CPU 3 of an AMD EPYC 7551 with one
font worker. One warm-up preceded eleven alternating baseline/candidate pairs,
each in a fresh output directory. Baseline times in milliseconds were
1007.115, 988.159, 996.903, 1006.978, 1016.263, 1004.195, 1009.465, 997.117,
1009.470, 989.773, and 1009.758; candidate times were 913.945, 915.581,
911.799, 931.684, 962.859, 928.195, 915.712, 902.216, 907.550, 924.237, and
917.025. The medians were 1006.978 ms and 915.712 ms, a 9.1% reduction; the
median paired reduction was 8.5%. Median user CPU time fell from 0.90 s to
0.85 s and median system CPU time from 0.10 s to 0.06 s. Peak RSS was 28,672
KiB for every measured run. Load averages were 39.96/40.99/40.61 before and
40.18/40.98/40.62 after. The baseline and candidate executable SHA-256 values
were `81f1bbbbe0c9c617b7fbacfca3dfe5407276f263b671ef994f524209c42189a0`
and `bf6ddc831a1c17c51a412820d09ab45bf450046e204635715518baa68daee602`.
The corpus manifest and input SHA-256 values were
`8681f4df7424f7ac585a7a508047eaf266751d3264b6f049781a961b3040a26f` and
`9b311f1835266833ad40130e7a7a6361a950c965d308c02d567361e72ce74aa5`.
Every measured PDF had the digest above. This is a targeted result, not the
full-corpus headline benchmark.

The targeted warm-cache `testmath` comparison used the pre-change commit
`a20f489` and the candidate built with GCC 13.3.0 as C17 release binaries with
`-O3 -flto=auto -DNDEBUG -fno-stack-protector -fno-plt
-fno-semantic-interposition`. Both were pinned to CPU 3 of an AMD EPYC 7551
with one font worker; each received one warm-up followed by eleven runs in
fresh output directories. Baseline times in milliseconds were 1074.0,
1083.7, 1063.0, 1061.3, 1068.5, 1065.0, 1055.5, 1054.6, 1077.9, 1076.5,
and 1119.2; candidate times were 1017.2, 1023.8, 1026.5, 1028.3, 1015.6,
1067.8, 1010.8, 1002.3, 1004.3, 1082.2, and 1023.1. The medians were
1068.5 ms and 1023.1 ms, a 4.2% reduction; the median paired reduction was
5.0%. Median system CPU time fell from 0.14 s to 0.10 s, while peak RSS stayed
at 28,672 KiB. Load averages were 41.39/40.77/41.65 before and
41.62/40.88/41.66 after. The baseline and candidate executable SHA-256 values
were `61a0d6b78f25b0f2b4fff0ab9407943d841c8cbd84c07075c626fce2bc459d6b`
and `aeb90df61226328b87fbb0ba6fdc377a663072a58bebbfa34e0725106166d491`.
The corpus manifest digest was
`8681f4df7424f7ac585a7a508047eaf266751d3264b6f049781a961b3040a26f`,
the document digest was
`9b311f1835266833ad40130e7a7a6361a950c965d308c02d567361e72ce74aa5`,
and both PDFs had SHA-256
`1b9be60d6142c3bbe9bfad669e9863007034517b2e42dbfef44bf57233482def`.
This targeted result is not the full-corpus headline benchmark.

A later clock profile of `testmath` found that the two `strstr` calls used to
locate every Subrs and CharStrings entry terminator scanned beyond the entry's
known section limit. Type 1 subsetting accounted for 20.3% of total CPU time
in that profile. HSTeX now scans newlines once, compares both accepted closing
spellings at each line, and stops at the supplied limit. The same profile then
attributed 2.1% of total CPU time to Type 1 subsetting.

The matched warm-cache timing compared baseline commit `839ffbf` with the
bounded-scan candidate. Both were GCC 13.3.0 C17 release builds using
`-O3 -flto=auto -DNDEBUG -fno-stack-protector -fno-plt
-fno-semantic-interposition`, pinned to CPU 3 of an AMD EPYC 7551 with one
font worker. One warm-up preceded eleven alternating baseline/candidate pairs,
each in a fresh output directory. Baseline times in milliseconds were
937.839, 942.090, 932.749, 874.569, 971.585, 939.326, 927.572, 931.412,
911.139, 925.659, and 949.102; candidate times were 781.890, 754.243,
758.018, 730.898, 757.205, 762.640, 782.307, 765.982, 755.249, 761.781,
and 748.407. The medians were 932.749 ms and 758.018 ms, an 18.7%
reduction; the median paired reduction was 17.8%. Median user CPU time fell
from 0.87 s to 0.69 s, while median system CPU time remained 0.05 s. Peak RSS
was 28,672 KiB for every measured run. Load averages were 37.20/37.92/38.46
before and 37.31/37.88/38.43 after. The baseline and candidate executable
SHA-256 values were
`94d6662775589a1efc6b26132a612ed0cf08ae40f650932fa89898b3206c88a6`
and
`e3b66ae953156337605005e78fb1c15fe345df68ddeae22f6271d8e3882bfe8b`.
The corpus manifest and input SHA-256 values were
`8681f4df7424f7ac585a7a508047eaf266751d3264b6f049781a961b3040a26f`
and
`9b311f1835266833ad40130e7a7a6361a950c965d308c02d567361e72ce74aa5`.
Every measured PDF had SHA-256
`1b9be60d6142c3bbe9bfad669e9863007034517b2e42dbfef44bf57233482def`.
The release and stress corpora remained 14/14 and 6/6. A separate run of the
existing per-document timing mode reduced the sum of HSTeX document medians
from 5,973.3 ms to 4,918.5 ms while the corresponding reference sums were
4,031.4 ms and 4,021.3 ms. That sum is broad-impact evidence, not the
full-corpus headline benchmark.

After the glyph-to-Unicode index removed the next dominant cost, an aggregate
3.402-second `testmath` clock profile attributed 10.9% of total CPU time to
formatted-output machinery called by Type 1 private-section disassembly. Each
decoded charstring integer used a general `snprintf` conversion. HSTeX now
renders the bounded `int32_t` domain directly into a 12-byte decimal buffer;
unsigned magnitude arithmetic includes `INT32_MIN` without signed overflow.
The exact Type 1 specification vectors cover both signed extremes, every
compact-number boundary, zero, and all three supported font containers. A
second aggregate profile attributed 0.7% to the formatted-output machinery
and 5.6% inclusive to private-section disassembly, down from 13.2%.

The matched warm-cache timing compared baseline commit `839a4ad` with the
bounded-integer candidate. Both were GCC 13.3.0 C17 release builds using
`-O3 -flto=auto -DNDEBUG -fno-stack-protector -fno-plt
-fno-semantic-interposition`, pinned to CPU 3 of an AMD EPYC 7551 with one
font worker. They loaded the same format. One warm-up preceded eleven
alternating baseline/candidate pairs, each in a fresh output directory.
Baseline times in milliseconds were 412.392, 414.194, 412.287, 414.371,
408.780, 412.350, 423.234, 414.343, 426.764, 415.318, and 408.547; candidate
times were 385.710, 392.533, 380.617, 378.748, 388.556, 387.056, 377.789,
385.913, 376.987, 377.682, and 380.515. The medians were 414.194 ms and
380.617 ms, an 8.1% reduction; the median paired reduction was 6.9%. Median
user CPU time fell from 0.35 s to 0.32 s, while median system CPU time
remained 0.05 s. Peak RSS was 28,672 KiB for every measured run. Load averages
were 35.73/35.52/36.77 before and 35.61/35.51/36.75 after. The baseline and
candidate executable SHA-256 values were
`96204bc6d188474c2e776fc91c78fe038ca1b3d59305a779fc1283b5ed3b417f`
and
`76281a3c2859a412e2c85f7560c47d2393951c622523c4115d2472ea10b31235`.
The corpus manifest and input SHA-256 values were
`8681f4df7424f7ac585a7a508047eaf266751d3264b6f049781a961b3040a26f`
and
`9b311f1835266833ad40130e7a7a6361a950c965d308c02d567361e72ce74aa5`.
Every measured PDF had SHA-256
`1b9be60d6142c3bbe9bfad669e9863007034517b2e42dbfef44bf57233482def`.
The release and stress corpora remained 14/14 and 6/6.

A separate pinned run of the existing per-document timing mode summed to
3,689.5 ms for the reference and 2,658.7 ms for HSTeX, a 1.388x aggregate
ratio in HSTeX's favor. The preceding HSTeX sum was 2,838.7 ms, so the
bounded formatter reduced that diagnostic by 6.3%. This is broad-impact
evidence, not the full-corpus headline benchmark.

An aggregate `testmath` clock profile after that change attributed 13.58% of
total CPU time, inclusively, to zlib's `compress2`/`deflate` path at the
document's default `\pdfcompresslevel=9`. PDF emission already supplies each
complete uncompressed stream as a contiguous buffer. When Meson finds
libdeflate, HSTeX now compresses that buffer with libdeflate's zlib-format
encoder and reuses the compressor between streams at the same level. The
existing zlib path remains the fallback when libdeflate is unavailable,
explicitly disabled, or cannot allocate a compressor. Ubuntu 24.04 CI installs
libdeflate, while the Clang job configures `-Dlibdeflate=disabled` to exercise
both build paths.

The GCC release suite passed with each backend. With libdeflate 1.19 and zlib
1.3, the strict release and stress corpora remained 14/14 and 6/6. The matched
warm-cache `testmath` timing compared baseline commit `1a1e411` with the final
candidate. Both were GCC 13.3.0 C17 release builds using
`-O3 -flto=auto -DNDEBUG -fno-stack-protector -fno-plt
-fno-semantic-interposition`, pinned to CPU 3 of an AMD EPYC 7551 with one
font worker. One warm-up preceded eleven alternating baseline/candidate pairs,
each in a fresh output directory. Baseline times in milliseconds were
388.462, 378.477, 377.165, 379.766, 382.634, 385.420, 391.692, 382.090,
388.203, 376.529, and 379.160; candidate times were 377.782, 379.886,
362.039, 363.821, 368.705, 372.411, 370.484, 368.672, 374.183, 365.129,
and 371.857. The medians were 382.090 ms and 370.484 ms, a 3.0% reduction;
the median paired reduction was 3.5%, and the candidate was faster in ten of
eleven pairs. Median user CPU time fell from 0.32 s to 0.31 s, median system
CPU time remained 0.05 s, and peak RSS was 28,672 KiB for every measured run.
Load averages were 36.30/36.51/35.96 before and 36.26/36.49/35.96 after.

The baseline and candidate executable SHA-256 values were
`76281a3c2859a412e2c85f7560c47d2393951c622523c4115d2472ea10b31235`
and
`0516ddedf2b1283eedad7e849234e4762ca96bb0af57c83d63f97e32b5b13d42`.
Their native-format SHA-256 values were
`d19d99d5c1d73815d531bf811e5112fc97af942fb1fce35e063b5fb13c7072c1`
and
`afe3368069e3fc75dfb7b283f0dff95413492717b4d4111a4deefbc73df2111f`.
The corpus manifest and input SHA-256 values were
`8681f4df7424f7ac585a7a508047eaf266751d3264b6f049781a961b3040a26f`
and
`9b311f1835266833ad40130e7a7a6361a950c965d308c02d567361e72ce74aa5`.
Different valid compression encodings changed the PDF SHA-256 from
`1b9be60d6142c3bbe9bfad669e9863007034517b2e42dbfef44bf57233482def`
to
`beafce8089fdb36a2c4af7b20bed022aefff0e8a34cbbd6aa5b82e72a8a9994d`
and reduced its size from 453,320 to 451,257 bytes; the decompressed semantic
corpus gates agree.

A separate run of the existing per-document timing mode summed to 4,304.3 ms
for the reference and 3,033.6 ms for HSTeX, a 1.419x aggregate ratio in
HSTeX's favor. This and the matched `testmath` result are diagnostic evidence,
not the full-corpus headline benchmark.

## Cold speculative-taint reporting

An aggregate `testmath` profile attributed 3.42% of exclusive CPU time to
`hstex_engine_meaning`. The ordinary lookup had acquired a 128-byte stack
frame and three saved registers because link-time optimization inlined the
speculative carrier's read-before-write fault reporter into it. That reporter
runs only after a carrier arms a nonempty taint map and a watched meaning is
read before it is replaced. Marking the reporter cold and no-inline leaves the
same check and reporter call in place while keeping them out of the ordinary
lookup. In the GCC release executable, the ordinary function shrank from 303
bytes to 96 bytes and no longer allocates a stack frame.

The matched benchmark compared executable SHA-256
`0516ddedf2b1283eedad7e849234e4762ca96bb0af57c83d63f97e32b5b13d42`
with candidate
`adbbcaa7e1f7dd5c0b4e4bcd6da6ccc06014012f941c2e0c30e518c71a135973`.
Both were GCC 13.3.0 C17 release builds using
`-O3 -flto=auto -DNDEBUG -fno-stack-protector -fno-plt
-fno-semantic-interposition`, pinned to CPU 24 of an AMD EPYC 7551 with one
font worker. Each document received one warm-up followed by alternating
baseline/candidate runs in fresh output directories. On 31 `testmath` pairs,
the medians were 464.279 ms and 463.283 ms, a 0.21% reduction; paired median
and mean reductions were 0.21% and 0.27%, and the candidate won 20 pairs.
Across 21 pairs each of `technote`, `tools-overview`, and
`usrguide-historic`, every document median improved by 0.27--0.41%. The 63
combined pairs had a 0.37% paired median reduction, a 0.39% paired mean
reduction, and 46 candidate wins. Median peak RSS remained 28,672 KiB. The
format SHA-256 was
`afe3368069e3fc75dfb7b283f0dff95413492717b4d4111a4deefbc73df2111f`;
each pair produced one identical PDF digest. Load averages ranged from
35.84 to 36.39 over the three-document run and from 36.96 to 37.66 over the
`testmath` run.

The ordinary release suite passed, with its expected driver skip. The strict
release and stress corpora remained 14/14 and 6/6.

## PDF font Unicode maps

A controlled pdfTeX 1.40.25 `encguide` run with `\pdfgentounicode=1` maps the
T1 encoding's code 127 to U+002D, codes 149 and 181 to U+0162 and U+0163, and
writes those values in the affected `ec-lmr10` ToUnicode resource. The
normalized extracted text is therefore a hyphen and the cedilla forms of
uppercase and lowercase T, even when a newer glyph-name database associates
the same glyph names with U+00AD or the comma-below forms U+021A and U+021B.
HSTeX's shared T1 CMap records the observed encoding semantics directly; CMap
object sharing and entry grouping do not alter those scalar mappings.

A black-box pdfTeX 1.40.25 probe declares `A` first as U+0041 and then as
U+0042. Its uncompressed ToUnicode CMap contains `<41> <0042>`, and extracted
text is `B`: the newest declaration wins. HSTeX preserves the ordered mapping
array carried by a format and keeps a separate transient open-addressed index
of one-based array places. Adding a duplicate replaces only its index slot,
and reading a format reconstructs the index in declaration order. Thus the
index does not become format data or alter the observable override rule.

A clock profile of `testmath` after the bounded Type 1 entry scan attributed
53.5% inclusive CPU time to CMap writing, dominated by string comparison.
Each glyph lookup was walking the format's full glyph-to-Unicode array in
reverse, for as many as 256 character codes per font. The transient index
changes that lookup from a linear walk to a hash probe.

The matched warm-cache timing compared baseline commit `b6e39e7` with the
indexed candidate. Both were GCC 13.3.0 C17 release builds using
`-O3 -flto=auto -DNDEBUG -fno-stack-protector -fno-plt
-fno-semantic-interposition`, pinned to CPU 3 of an AMD EPYC 7551 with one
font worker. Each executable built and loaded its own compatible format. One
warm-up preceded eleven alternating baseline/candidate pairs, each in a fresh
output directory. Baseline times in milliseconds were 717.470, 708.155,
708.912, 698.222, 708.164, 722.290, 712.427, 719.618, 727.413, 707.897, and
728.942; candidate times were 448.461, 418.527, 406.976, 403.387, 418.573,
417.749, 420.554, 414.413, 409.140, 409.360, and 409.603. The medians were
712.427 ms and 414.413 ms, a 41.8% reduction; the median paired reduction was
42.2%. Median user CPU time fell from 0.65 s to 0.35 s, while median system
CPU time remained 0.05 s. Peak RSS was 28,672 KiB for every measured run.
Load averages were 36.45/37.89/38.22 before and 36.05/37.73/38.16 after. The
baseline and candidate executable SHA-256 values were
`e3b66ae953156337605005e78fb1c15fe345df68ddeae22f6271d8e3882bfe8b`
and
`96204bc6d188474c2e776fc91c78fe038ca1b3d59305a779fc1283b5ed3b417f`.
The corpus manifest and input SHA-256 values were
`8681f4df7424f7ac585a7a508047eaf266751d3264b6f049781a961b3040a26f`
and
`9b311f1835266833ad40130e7a7a6361a950c965d308c02d567361e72ce74aa5`.
Every measured PDF had SHA-256
`1b9be60d6142c3bbe9bfad669e9863007034517b2e42dbfef44bf57233482def`.
The release and stress corpora remained 14/14 and 6/6.

A separate pinned run of the existing per-document timing mode summed to
3,712.5 ms for the reference and 2,838.7 ms for HSTeX, a 1.308x aggregate
ratio in HSTeX's favor. The preceding HSTeX sum was 4,918.5 ms, so the indexed
candidate reduced that diagnostic by 42.3%. This is broad-impact evidence,
not the full-corpus headline benchmark.

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

## PDF transformations

Section 7.20 of the public pdfTeX user manual specifies `\pdfsave`,
`\pdfsetmatrix`, and `\pdfrestore`. The save and restore primitives insert the
PDF graphics-state operators `q` and `Q`; the matrix primitive expands four
unitless numbers and inserts them as `a b c d 0 0 cm`. TeX, rather than the
matrix text, supplies the translation about the current list position.

A black-box pdfTeX 1.40.25 probe puts the three primitives in an `\hbox` and
shows three distinct zero-width whatsits. Its uncompressed page stream anchors
each graphics-state operation at the horizontal position where the whatsit
stands: it translates to that point before the operation and translates back
afterward. HSTeX keeps the expanded four-number text on the matrix whatsit and
uses the same origin-relative PDF placement for all three operations.

## Restricted shell escape

The public `shell_escape_commands` variable in `texmf.cnf` names the programs
an installation permits in restricted shell-escape mode. HSTeX reads that
value through `kpsewhich`, reports mode 2 through `\pdfshellescape`, and treats
stream 18 as a system-command whatsit. `-no-shell-escape` selects mode 0.

A black-box pdfTeX 1.40.25 probe under the same TeX Live configuration reports
`\pdfshellescape=2`; `printf` is logged as `disabled (restricted)`, while
`kpsewhich --version` is logged as `executed safely (allowed)`. With
`-no-shell-escape`, the value is zero and the same allowed command is logged as
`disabled`. HSTeX preserves those three outcomes. Restricted commands are
split into arguments with quote and backslash handling and passed directly to
the allowlisted executable. No command shell interprets metacharacters or
expansions.

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

A separate pdfTeX 1.40.25 probe opens an empty file and reads after setting
`\endlinechar=-1`, then repeats with the default value 13. The first target
macro is empty; the second contains `\par`. HSTeX therefore feeds the
synthetic line at file end through the ordinary mouth with the current
`\endlinechar`, rather than inserting `\par` unconditionally. Babel's INI
reader relies on the empty form after its final newline.

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
