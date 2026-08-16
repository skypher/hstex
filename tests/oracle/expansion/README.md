# Macro expansion compatibility record

The first expansion layer follows the public descriptions in *TeX by Topic*,
chapters 7, 9, 10, 11, 12, and 20: replacement-list parameters are stored as
parameter tokens, macro arguments are collected without expansion, one outer
brace pair is removed, replacement text is reinserted into the input stream,
and assignments are restored when their group ends unless made global.

The following command was run against the installed pdfTeX 1.40.25 executable
as a black-box INITEX oracle. No engine source was inspected.

```sh
pdftex -ini -interaction=nonstopmode \
  '\catcode`\{=1 \catcode`\}=2 \catcode`\#=6
   \def\grab#1,#2;{\message{DELIM=<#2:#1>}}\grab {a,b},c;
   \def\a{G}{\def\a{L}\message{LOCAL=\a}}\message{OUT=\a}
   \def\a{A}\def\b{\def\a{B}}
   \expandafter\message\expandafter{\a}\b\message{AFTER=\a}
   \def\q{Q}\message{NOEXPAND=[\noexpand\q\q]}\end'
```

The relevant transcript was:

```text
DELIM=<c:a,b> LOCAL=L OUT=G A AFTER=B NOEXPAND=[\q Q]
```

Direct unit tests additionally cover undelimited and delimited parameters,
literal parameter markers, literal prefix matching, nested brace stripping,
local/global definitions, meaning-copying `let`, `long` paragraph arguments,
and expansion ordering.
