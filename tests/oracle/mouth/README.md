# Mouth compatibility record

The mouth implementation follows the externally documented behavior in Victor
Eijkhout, *TeX by Topic*, Chapter 2, especially sections 2.2–2.7. The reference
is available from CTAN:

<https://mirrors.ibiblio.org/pub/mirrors/CTAN/info/texbytopic/TeXbyTopic.pdf>

The INITEX starting catcodes are also documented at the beginning of the
ordinary macro input `plain.tex`. On the benchmark machine they were confirmed
without reading engine source by executing pdfTeX 1.40.25 in INITEX mode after
making brace characters group delimiters. The observed values were:

```text
C0=9,C9=12,C10=12,C13=5,C32=10,C35=12,C36=12,C37=14,C38=12,
C92=0,C94=12,C95=12,C123=1,C125=2,C126=12,C127=15
```

The values for braces reflect the two setup assignments performed before the
query; INITEX gives both braces category 12 before those assignments.

An e-TeX `\showtokens` black-box probe, after assigning the common plain-format
catcodes, used this token-list input:

```text
  abc  def \foo   x\! y ^^41 ~
```

and reported:

```text
>  abc def \foo x\! y A ~.
```

The unit tests cover the information that this rendering does not expose:
regular versus active control-sequence identity, canonical character code 32
for category-10 input, standalone invalid-character errors, invalid control
symbols, mutable catcodes during an input line, physical line normalization,
suppressed `\endlinechar`, embedded category-5 characters, and `^^` hexadecimal
and 64-offset conversion.
