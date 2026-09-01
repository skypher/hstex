#!/usr/bin/env python3
"""Compare two PDFs by stable, externally observable document semantics."""

from __future__ import annotations

import argparse
import dataclasses
import difflib
import math
import re
import shutil
import subprocess
import sys
import tempfile
import unicodedata
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any, Iterable, Sequence


COORDINATE_TOLERANCE = 0.01
RENDER_DPI = 144
PDF_WHITESPACE = b"\x00\x09\x0a\x0c\x0d\x20"
PDF_DELIMITERS = b"()<>[]{}/%"
NUMBER_RE = re.compile(rb"^[+-]?(?:\d+(?:\.\d*)?|\.\d+)$")
SUBSET_PREFIX_RE = re.compile(r"^[A-Z]{6}\+")


class ComparisonError(RuntimeError):
    """A comparison tool or parser could not produce trustworthy evidence."""


@dataclasses.dataclass(frozen=True)
class PdfName:
    value: bytes


@dataclasses.dataclass(frozen=True)
class PdfNumber:
    value: float


@dataclasses.dataclass(frozen=True)
class PdfKeyword:
    value: bytes


@dataclasses.dataclass(frozen=True)
class PdfReference:
    number: int
    generation: int


@dataclasses.dataclass(frozen=True)
class PdfPageReference:
    page: int


@dataclasses.dataclass(frozen=True)
class PdfReferenceCycle:
    pass


@dataclasses.dataclass(frozen=True)
class PdfArray:
    values: tuple[Any, ...]


@dataclasses.dataclass(frozen=True)
class PdfDictionary:
    entries: tuple[tuple[bytes, Any], ...]

    def get(self, key: bytes) -> Any | None:
        for candidate, value in self.entries:
            if candidate == key:
                return value
        return None


@dataclasses.dataclass(frozen=True)
class Token:
    kind: str
    value: Any = None


@dataclasses.dataclass(frozen=True)
class Glyph:
    page: int
    operation: str
    font: str
    writing_mode: str
    identity: str
    transform: tuple[float, ...]
    text_matrix: tuple[float, ...]
    x: float
    y: float
    advance: float


def run_tool(command: Sequence[str]) -> subprocess.CompletedProcess[bytes]:
    try:
        result = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise ComparisonError(f"could not execute {command[0]}: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise ComparisonError(
            f"{' '.join(command)} exited {result.returncode}: {detail}"
        )
    return result


def decode_pdf_name(raw: bytes) -> bytes:
    output = bytearray()
    index = 0
    while index < len(raw):
        if (
            raw[index : index + 1] == b"#"
            and index + 2 < len(raw)
            and all(chr(char) in "0123456789abcdefABCDEF" for char in raw[index + 1 : index + 3])
        ):
            output.append(int(raw[index + 1 : index + 3], 16))
            index += 3
        else:
            output.append(raw[index])
            index += 1
    return bytes(output)


def scan_literal_string(data: bytes, start: int) -> tuple[bytes, int]:
    output = bytearray()
    depth = 1
    index = start + 1
    escapes = {
        ord("n"): ord("\n"),
        ord("r"): ord("\r"),
        ord("t"): ord("\t"),
        ord("b"): ord("\b"),
        ord("f"): ord("\f"),
        ord("("): ord("("),
        ord(")"): ord(")"),
        ord("\\"): ord("\\"),
    }
    while index < len(data):
        char = data[index]
        if char == ord("\\"):
            index += 1
            if index >= len(data):
                break
            escaped = data[index]
            if escaped == ord("\r"):
                index += 1
                if index < len(data) and data[index] == ord("\n"):
                    index += 1
                continue
            if escaped == ord("\n"):
                index += 1
                continue
            if ord("0") <= escaped <= ord("7"):
                end = index
                while (
                    end < len(data)
                    and end < index + 3
                    and ord("0") <= data[end] <= ord("7")
                ):
                    end += 1
                output.append(int(data[index:end], 8) & 0xFF)
                index = end
                continue
            output.append(escapes.get(escaped, escaped))
            index += 1
            continue
        if char == ord("("):
            depth += 1
            output.append(char)
            index += 1
            continue
        if char == ord(")"):
            depth -= 1
            index += 1
            if depth == 0:
                return bytes(output), index
            output.append(char)
            continue
        output.append(char)
        index += 1
    raise ComparisonError("unterminated PDF literal string in mutool output")


def lex_pdf(data: bytes) -> list[Token]:
    tokens: list[Token] = []
    index = 0
    while index < len(data):
        char = data[index]
        if char in PDF_WHITESPACE:
            index += 1
            continue
        if char == ord("%"):
            while index < len(data) and data[index] not in b"\r\n":
                index += 1
            continue
        if data[index : index + 2] == b"<<":
            tokens.append(Token("DICT_START"))
            index += 2
            continue
        if data[index : index + 2] == b">>":
            tokens.append(Token("DICT_END"))
            index += 2
            continue
        if char == ord("["):
            tokens.append(Token("ARRAY_START"))
            index += 1
            continue
        if char == ord("]"):
            tokens.append(Token("ARRAY_END"))
            index += 1
            continue
        if char == ord("("):
            value, index = scan_literal_string(data, index)
            tokens.append(Token("STRING", value))
            continue
        if char == ord("<"):
            end = data.find(b">", index + 1)
            if end < 0:
                raise ComparisonError("unterminated PDF hexadecimal string")
            raw = re.sub(rb"\s+", b"", data[index + 1 : end])
            if len(raw) % 2:
                raw += b"0"
            try:
                value = bytes.fromhex(raw.decode("ascii"))
            except ValueError as error:
                raise ComparisonError("invalid PDF hexadecimal string") from error
            tokens.append(Token("STRING", value))
            index = end + 1
            continue
        if char == ord("/"):
            end = index + 1
            while (
                end < len(data)
                and data[end] not in PDF_WHITESPACE
                and data[end] not in PDF_DELIMITERS
            ):
                end += 1
            tokens.append(Token("NAME", decode_pdf_name(data[index + 1 : end])))
            index = end
            continue
        end = index
        while (
            end < len(data)
            and data[end] not in PDF_WHITESPACE
            and data[end] not in PDF_DELIMITERS
        ):
            end += 1
        if end == index:
            raise ComparisonError(
                f"unexpected byte {data[index:index + 1]!r} in mutool output"
            )
        tokens.append(Token("ATOM", data[index:end]))
        index = end
    return tokens


class PdfParser:
    def __init__(self, tokens: Sequence[Token]):
        self.tokens = tokens
        self.index = 0

    def current(self) -> Token | None:
        if self.index >= len(self.tokens):
            return None
        return self.tokens[self.index]

    def parse(self) -> Any:
        value = self.parse_value()
        if self.current() is not None:
            raise ComparisonError(
                f"extra token {self.current()!r} in mutool object output"
            )
        return value

    def parse_value(self) -> Any:
        token = self.current()
        if token is None:
            raise ComparisonError("unexpected end of mutool object output")
        if token.kind == "DICT_START":
            return self.parse_dictionary()
        if token.kind == "ARRAY_START":
            return self.parse_array()
        self.index += 1
        if token.kind == "NAME":
            return PdfName(token.value)
        if token.kind == "STRING":
            return token.value
        if token.kind != "ATOM":
            raise ComparisonError(f"unexpected token {token!r}")
        if NUMBER_RE.match(token.value):
            if self.index + 1 < len(self.tokens):
                generation = self.tokens[self.index]
                marker = self.tokens[self.index + 1]
                if (
                    generation.kind == "ATOM"
                    and re.match(rb"^\d+$", generation.value)
                    and marker == Token("ATOM", b"R")
                    and re.match(rb"^\d+$", token.value)
                ):
                    self.index += 2
                    return PdfReference(int(token.value), int(generation.value))
            return PdfNumber(float(token.value))
        if token.value == b"true":
            return True
        if token.value == b"false":
            return False
        if token.value == b"null":
            return None
        return PdfKeyword(token.value)

    def parse_dictionary(self) -> PdfDictionary:
        self.index += 1
        entries: list[tuple[bytes, Any]] = []
        while True:
            token = self.current()
            if token is None:
                raise ComparisonError("unterminated PDF dictionary")
            if token.kind == "DICT_END":
                self.index += 1
                return PdfDictionary(tuple(sorted(entries, key=lambda item: item[0])))
            if token.kind != "NAME":
                raise ComparisonError(f"PDF dictionary key is not a name: {token!r}")
            self.index += 1
            entries.append((token.value, self.parse_value()))

    def parse_array(self) -> PdfArray:
        self.index += 1
        values: list[Any] = []
        while True:
            token = self.current()
            if token is None:
                raise ComparisonError("unterminated PDF array")
            if token.kind == "ARRAY_END":
                self.index += 1
                return PdfArray(tuple(values))
            values.append(self.parse_value())


def parse_pdf_value(data: bytes) -> Any:
    return PdfParser(lex_pdf(data)).parse()


def parse_mutool_objects(data: bytes) -> list[Any]:
    values: list[Any] = []
    for line in data.splitlines():
        line = line.strip()
        if not line:
            continue
        match = re.match(rb"^\d+\s+\d+\s+obj\s+(.*)$", line)
        if match:
            line = match.group(1)
        values.append(parse_pdf_value(line))
    return values


class PdfObjectResolver:
    def __init__(self, pdf: Path):
        self.pdf = pdf
        self.pages = self.read_pages()
        self.object_cache: dict[tuple[int, int], Any] = {}

    def read_pages(self) -> dict[tuple[int, int], int]:
        output = run_tool(["mutool", "show", str(self.pdf), "pages"]).stdout
        pages: dict[tuple[int, int], int] = {}
        for line in output.decode("ascii", "replace").splitlines():
            match = re.match(r"page\s+(\d+)\s+=\s+(\d+)\s+(\d+)\s+R$", line)
            if match:
                pages[(int(match.group(2)), int(match.group(3)))] = int(match.group(1))
        if not pages:
            raise ComparisonError(f"mutool did not report pages for {self.pdf}")
        return pages

    def load_object(self, reference: PdfReference) -> Any:
        key = (reference.number, reference.generation)
        if key not in self.object_cache:
            output = run_tool(
                ["mutool", "show", "-g", str(self.pdf), str(reference.number)]
            ).stdout
            values = parse_mutool_objects(output)
            if len(values) != 1:
                raise ComparisonError(
                    f"mutool returned {len(values)} values for object {reference.number}"
                )
            self.object_cache[key] = values[0]
        return self.object_cache[key]

    def resolve(self, value: Any, active: frozenset[tuple[int, int]] = frozenset()) -> Any:
        if isinstance(value, PdfReference):
            key = (value.number, value.generation)
            if key in self.pages:
                return PdfPageReference(self.pages[key])
            if key in active:
                return PdfReferenceCycle()
            return self.resolve(self.load_object(value), active | {key})
        if isinstance(value, PdfArray):
            return PdfArray(tuple(self.resolve(item, active) for item in value.values))
        if isinstance(value, PdfDictionary):
            return PdfDictionary(
                tuple((key, self.resolve(item, active)) for key, item in value.entries)
            )
        return value


def parse_numbers(value: str) -> tuple[float, ...]:
    try:
        return tuple(float(part) for part in value.split())
    except ValueError as error:
        raise ComparisonError(f"invalid numeric attribute {value!r}") from error


def xml_character_is_valid(codepoint: int) -> bool:
    return (
        codepoint in (0x09, 0x0A, 0x0D)
        or 0x20 <= codepoint <= 0xD7FF
        or 0xE000 <= codepoint <= 0xFFFD
        or 0x10000 <= codepoint <= 0x10FFFF
    )


def parse_mupdf_trace(output: Path) -> ET.Element:
    try:
        data = output.read_bytes()
    except OSError as error:
        raise ComparisonError(f"could not read MuPDF trace {output}: {error}") from error

    # MuPDF represents unmapped 8-bit font codes as numeric character
    # references. Codes such as 0x17 are meaningful evidence about that font
    # mapping but are forbidden XML 1.0 characters, so ElementTree rejects the
    # otherwise valid trace. Glyph identity is recorded separately; replace
    # only forbidden Unicode scalar references before parsing the XML.
    def sanitize(match: re.Match[bytes]) -> bytes:
        base = 16 if match.group(1) else 10
        codepoint = int(match.group(2), base)
        return match.group(0) if xml_character_is_valid(codepoint) else b"&#xfffd;"

    data = re.sub(rb"&#(x?)([0-9A-Fa-f]+);", sanitize, data)
    try:
        return ET.fromstring(data)
    except ET.ParseError as error:
        raise ComparisonError(f"could not parse MuPDF trace {output}: {error}") from error


def trace_document(pdf: Path, output: Path) -> tuple[tuple[tuple[float, ...], ...], tuple[Glyph, ...]]:
    run_tool(["mutool", "draw", "-q", "-F", "trace", "-o", str(output), str(pdf)])
    root = parse_mupdf_trace(output)

    media_boxes: list[tuple[float, ...]] = []
    glyphs: list[Glyph] = []
    for page_number, page in enumerate(root.findall("page"), 1):
        media_boxes.append(parse_numbers(page.attrib["mediabox"]))
        for operation in page:
            if not operation.tag.endswith("_text"):
                continue
            transform = parse_numbers(operation.attrib.get("transform", "1 0 0 1 0 0"))
            for span in operation.findall("span"):
                font = SUBSET_PREFIX_RE.sub("", span.attrib.get("font", ""))
                text_matrix = parse_numbers(span.attrib.get("trm", "1 0 0 1"))
                writing_mode = span.attrib.get("wmode", "0")
                for item in span.findall("g"):
                    advance = float(item.attrib.get("adv", "0"))
                    identity = item.attrib.get("glyph")
                    if identity is None and math.isclose(advance, 0.0, abs_tol=1e-12):
                        continue
                    if identity is None:
                        identity = "unicode:" + item.attrib.get("unicode", "")
                    glyphs.append(
                        Glyph(
                            page_number,
                            operation.tag,
                            font,
                            writing_mode,
                            identity,
                            transform,
                            text_matrix,
                            float(item.attrib["x"]),
                            float(item.attrib["y"]),
                            advance,
                        )
                    )
    if not media_boxes:
        raise ComparisonError(f"MuPDF trace contains no pages for {pdf}")
    return tuple(media_boxes), tuple(glyphs)


def page_geometry(pdf: Path, page_count: int) -> dict[tuple[int, str], tuple[float, ...]]:
    output = run_tool(
        ["pdfinfo", "-box", "-f", "1", "-l", str(page_count), str(pdf)]
    ).stdout.decode("utf-8", "replace")
    geometry: dict[tuple[int, str], tuple[float, ...]] = {}
    pattern = re.compile(
        r"^Page\s+(\d+)\s+(rot|MediaBox|CropBox|BleedBox|TrimBox|ArtBox):\s+(.*)$"
    )
    for line in output.splitlines():
        match = pattern.match(line)
        if match:
            geometry[(int(match.group(1)), match.group(2))] = parse_numbers(match.group(3))
    expected = page_count * 6
    if len(geometry) != expected:
        raise ComparisonError(
            f"pdfinfo reported {len(geometry)}/{expected} page-geometry records for {pdf}"
        )
    return geometry


def extracted_text(pdf: Path) -> str:
    output = run_tool(
        ["pdftotext", "-layout", "-enc", "UTF-8", "-eol", "unix", str(pdf), "-"]
    ).stdout
    try:
        text = output.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ComparisonError(f"pdftotext emitted invalid UTF-8 for {pdf}") from error
    text = unicodedata.normalize("NFC", text.replace("\r\n", "\n").replace("\r", "\n"))
    return "\n".join(line.rstrip(" \t") for line in text.split("\n"))


def render_hashes(pdf: Path) -> tuple[str, ...]:
    result = run_tool(
        [
            "mutool",
            "draw",
            "-q",
            "-c",
            "rgb",
            "-A",
            "8",
            "-r",
            str(RENDER_DPI),
            "-s",
            "5",
            str(pdf),
        ]
    )
    diagnostic = result.stderr.decode("utf-8", "replace")
    matches = re.findall(r"^page\s+.+\s+(\d+)\s+([0-9a-fA-F]{32})$", diagnostic, re.MULTILINE)
    if not matches:
        raise ComparisonError(f"mutool emitted no rendered-page hashes for {pdf}")
    numbered = [(int(page), digest.lower()) for page, digest in matches]
    if [page for page, _ in numbered] != list(range(1, len(numbered) + 1)):
        raise ComparisonError(f"mutool emitted a non-contiguous page sequence for {pdf}")
    return tuple(digest for _, digest in numbered)


def normalized_tool_lines(command: Sequence[str], skip_header: bool = False) -> tuple[str, ...]:
    output = run_tool(command).stdout.decode("utf-8", "replace")
    lines = output.splitlines()
    if skip_header and lines:
        lines = lines[1:]
    return tuple(line.rstrip() for line in lines if line.strip())


def link_annotations(pdf: Path, page_count: int) -> tuple[tuple[Any, ...], ...]:
    resolver = PdfObjectResolver(pdf)
    pages: list[tuple[Any, ...]] = []
    for page in range(1, page_count + 1):
        output = run_tool(
            ["mutool", "show", "-g", str(pdf), f"pages/{page}/Annots/*"]
        ).stdout
        links: list[Any] = []
        for value in parse_mutool_objects(output):
            if (
                isinstance(value, PdfDictionary)
                and value.get(b"Subtype") == PdfName(b"Link")
            ):
                links.append(resolver.resolve(value))
        pages.append(tuple(links))
    return tuple(pages)


def first_difference(reference: Any, candidate: Any, path: str = "value") -> str | None:
    if isinstance(reference, PdfNumber) and isinstance(candidate, PdfNumber):
        if math.isclose(
            reference.value,
            candidate.value,
            rel_tol=0.0,
            abs_tol=COORDINATE_TOLERANCE,
        ):
            return None
        return f"{path}: {reference.value:g} != {candidate.value:g}"
    if isinstance(reference, float) and isinstance(candidate, float):
        if math.isclose(
            reference,
            candidate,
            rel_tol=0.0,
            abs_tol=COORDINATE_TOLERANCE,
        ):
            return None
        return f"{path}: {reference:g} != {candidate:g}"
    if type(reference) is not type(candidate):
        return f"{path}: {reference!r} != {candidate!r}"
    if dataclasses.is_dataclass(reference):
        for field in dataclasses.fields(reference):
            difference = first_difference(
                getattr(reference, field.name),
                getattr(candidate, field.name),
                f"{path}.{field.name}",
            )
            if difference:
                return difference
        return None
    if isinstance(reference, dict):
        if set(reference) != set(candidate):
            missing = sorted(set(reference) - set(candidate))
            extra = sorted(set(candidate) - set(reference))
            return f"{path}: keys differ; missing={missing!r}, extra={extra!r}"
        for key in sorted(reference):
            difference = first_difference(reference[key], candidate[key], f"{path}[{key!r}]")
            if difference:
                return difference
        return None
    if isinstance(reference, (tuple, list)):
        if len(reference) != len(candidate):
            return f"{path}: length {len(reference)} != {len(candidate)}"
        for index, (left, right) in enumerate(zip(reference, candidate)):
            difference = first_difference(left, right, f"{path}[{index}]")
            if difference:
                return difference
        return None
    if reference != candidate:
        return f"{path}: {reference!r} != {candidate!r}"
    return None


def text_difference(reference: str, candidate: str) -> str | None:
    if reference == candidate:
        return None
    lines = list(
        difflib.unified_diff(
            reference.splitlines(),
            candidate.splitlines(),
            fromfile="reference.txt",
            tofile="candidate.txt",
            n=2,
            lineterm="",
        )
    )
    excerpt = " | ".join(lines[:8])
    return excerpt or "normalized text differs"


def require_tools() -> None:
    missing = [tool for tool in ("mutool", "pdfinfo", "pdftotext") if shutil.which(tool) is None]
    if missing:
        raise ComparisonError("required comparison tools are missing: " + ", ".join(missing))


def compare(reference: Path, candidate: Path, artifacts: Path) -> list[tuple[str, str | None]]:
    reference_trace, reference_glyphs = trace_document(reference, artifacts / "reference.trace.xml")
    candidate_trace, candidate_glyphs = trace_document(candidate, artifacts / "candidate.trace.xml")
    reference_pages = len(reference_trace)
    candidate_pages = len(candidate_trace)

    reference_text = extracted_text(reference)
    candidate_text = extracted_text(candidate)
    (artifacts / "reference.txt").write_text(reference_text, encoding="utf-8")
    (artifacts / "candidate.txt").write_text(candidate_text, encoding="utf-8")

    checks: list[tuple[str, str | None]] = []
    checks.append(("page media boxes", first_difference(reference_trace, candidate_trace, "pages")))
    checks.append(("glyph identities and positions", first_difference(reference_glyphs, candidate_glyphs, "glyphs")))
    checks.append(("normalized text and line/page breaks", text_difference(reference_text, candidate_text)))

    if reference_pages == candidate_pages:
        checks.append(
            (
                "page geometry",
                first_difference(
                    page_geometry(reference, reference_pages),
                    page_geometry(candidate, candidate_pages),
                    "geometry",
                ),
            )
        )
        checks.append(
            (
                "link annotations",
                first_difference(
                    link_annotations(reference, reference_pages),
                    link_annotations(candidate, candidate_pages),
                    "links",
                ),
            )
        )
    else:
        difference = f"page count {reference_pages} != {candidate_pages}"
        checks.append(("page geometry", difference))
        checks.append(("link annotations", difference))

    checks.extend(
        [
            (
                f"rendered pages at {RENDER_DPI} dpi",
                first_difference(render_hashes(reference), render_hashes(candidate), "renders"),
            ),
            (
                "named destinations",
                first_difference(
                    normalized_tool_lines(["pdfinfo", "-dests", str(reference)], True),
                    normalized_tool_lines(["pdfinfo", "-dests", str(candidate)], True),
                    "destinations",
                ),
            ),
            (
                "URI links",
                first_difference(
                    normalized_tool_lines(["pdfinfo", "-url", str(reference)], True),
                    normalized_tool_lines(["pdfinfo", "-url", str(candidate)], True),
                    "uris",
                ),
            ),
            (
                "bookmarks",
                first_difference(
                    normalized_tool_lines(["mutool", "show", str(reference), "outline"]),
                    normalized_tool_lines(["mutool", "show", str(candidate), "outline"]),
                    "bookmarks",
                ),
            ),
        ]
    )
    return checks


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare page geometry, glyph placement, text, rendering, destinations, "
            "links, and bookmarks without comparing raw PDF serialization."
        )
    )
    parser.add_argument("reference", type=Path, help="reference PDF")
    parser.add_argument("candidate", type=Path, help="candidate PDF")
    parser.add_argument(
        "--artifacts",
        type=Path,
        help="retain extracted traces and normalized text in this directory",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parse_args(sys.argv[1:] if argv is None else argv)
    for path in (arguments.reference, arguments.candidate):
        if not path.is_file():
            print(f"comparison error: PDF does not exist: {path}", file=sys.stderr)
            return 2
    try:
        require_tools()
        if arguments.artifacts is not None:
            arguments.artifacts.mkdir(parents=True, exist_ok=True)
            checks = compare(arguments.reference, arguments.candidate, arguments.artifacts)
        else:
            with tempfile.TemporaryDirectory(prefix="hstex-pdf-compare-") as temporary:
                checks = compare(arguments.reference, arguments.candidate, Path(temporary))
    except ComparisonError as error:
        print(f"comparison error: {error}", file=sys.stderr)
        return 2

    failures = [(name, detail) for name, detail in checks if detail is not None]
    if failures:
        for name, detail in failures:
            print(f"{name}: {detail}")
        print(f"{len(failures)}/{len(checks)} PDF semantic checks differ")
        return 1
    print(f"all {len(checks)} PDF semantic checks agree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
