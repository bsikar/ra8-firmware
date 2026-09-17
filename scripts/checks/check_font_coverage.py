#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: the baked font subsets cover exactly the Unicode set the tree declares.

Which characters the e-reader can draw with no SD card at all is decided by the
``cmap`` of the subset checked in under ``libs/ra8_fonts/``. Until this gate
there was no declaration of that set anywhere: the only record was a docstring
recipe in ``scripts/gen/font_to_c.py`` naming the ``pyftsubset --unicodes``
argument somebody once typed in a throwaway venv. Nothing read the font back,
so the recipe and the committed bytes were free to disagree -- and they do, by
33 codepoints: the recipe says ``0020-00FF`` while the committed subset stops
at U+007E and resumes at U+00A0, because the unencoded C1 block is dropped. A
subset regenerated from that recipe would therefore not be the file in the
tree, and a subset regenerated with a NARROWER set would be accepted in
silence, taking glyphs off the panel with a green build.

So the declaration is ``.github/font-coverage-declaration.txt``, and this gate
holds it against the fonts:

1. **Every declared codepoint is really in the font.** A range nobody can
   render is a promise the panel breaks.
2. **Every codepoint in the font is declared.** Coverage that grew without a
   declaration row is coverage nobody decided on, and it is what makes rule 1
   worth running: together they pin the set exactly, in both directions.
3. **The subset is reproducible from the committed face.** Each subset declares
   the source face it was cut from, and every declared codepoint must exist in
   that face too, so the recipe this gate emits (``--recipe``) can actually be
   re-run against the bytes in the tree.
4. **The counts the declaration states are its own.** The ``#!`` directives are
   re-derived from the rows, so a row added or deleted without updating them is
   a finding rather than a quiet edit.
5. **An unreadable or undeclared font is a finding, not an empty bound.** A
   missing file, a font whose ``cmap`` will not parse, or a declaration with no
   rows exits non-zero instead of reporting a clean run over nothing.

``--selftest`` runs first in the gate: it drives every rule against synthetic
coverage in both directions and asserts the committed declaration is quiet, so
"0 problems" cannot mean "measured nothing".

Part of #687 (Tier 2 font coverage). The wider baked subset and the fallback
face are the rest of that issue; this is the declaration and the gate they need
in order to be reviewable changes rather than a new blob.
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections.abc import Callable, Iterable, Sequence
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DECLARATION = REPO_ROOT / ".github" / "font-coverage-declaration.txt"

#: Unicode's last valid scalar value; anything above it cannot be a codepoint.
MAX_CODEPOINT = 0x10FFFF

#: The three counts the declaration restates about itself.
DIRECTIVE_KEYS = ("fonts", "ranges", "codepoints")

_SFNT_HEADER = 12
_TABLE_RECORD = 16

#: The format-4 terminating segment maps this codepoint, and it is not coverage.
_SEGMENT_TERMINATOR = 0xFFFF

#: A ``font`` row is exactly ``font <path> source <path>``.
_FONT_ROW_FIELDS = 3

#: Unicode ``cmap`` platform ids, and the Windows encodings that are Unicode.
_PLATFORM_UNICODE = 0
_PLATFORM_WINDOWS = 3
_WINDOWS_UNICODE_ENCODINGS = (1, 10)


class FontParseError(Exception):
    """A font file could not be read far enough to enumerate its ``cmap``."""


def _raise(reason: str, cause: Exception | None = None) -> None:
    """Raise :class:`FontParseError` with ``reason``, chaining ``cause`` when given."""
    raise FontParseError(reason) from cause


def _table_offsets(data: bytes) -> dict[str, int]:
    """Return the sfnt table directory of ``data`` as tag -> file offset."""
    if len(data) < _SFNT_HEADER:
        _raise("shorter than an sfnt header")
    (num_tables,) = struct.unpack_from(">H", data, 4)
    out: dict[str, int] = {}
    for index in range(num_tables):
        rec = _SFNT_HEADER + _TABLE_RECORD * index
        if rec + _TABLE_RECORD > len(data):
            _raise("table directory runs past end of file")
        tag = data[rec : rec + 4].decode("latin-1")
        (offset,) = struct.unpack_from(">I", data, rec + 8)
        out[tag] = offset
    return out


def _format4(data: bytes, base: int) -> set[int]:
    """Enumerate a segment-mapping (format 4) subtable, skipping glyph 0."""
    (seg_x2,) = struct.unpack_from(">H", data, base + 6)
    segments = _Segments(data, base, seg_x2)
    out: set[int] = set()
    for seg in range(segments.count):
        if segments.starts[seg] > segments.ends[seg]:
            _raise("format 4 segment with start above end")
        for cp in range(segments.starts[seg], segments.ends[seg] + 1):
            if cp != _SEGMENT_TERMINATOR and segments.glyph(data, cp, seg) != 0:
                out.add(cp)
    return out


class _Segments:
    """The four parallel arrays of a format-4 subtable, plus where they start."""

    def __init__(self, data: bytes, base: int, seg_x2: int) -> None:
        """Read the segment arrays of the subtable at ``base`` out of ``data``."""
        count = seg_x2 // 2
        self.count = count
        self.ends = struct.unpack_from(f">{count}H", data, base + 14)
        self.starts = struct.unpack_from(f">{count}H", data, base + 16 + seg_x2)
        self.deltas = struct.unpack_from(f">{count}h", data, base + 16 + 2 * seg_x2)
        self.range_base = base + 16 + 3 * seg_x2
        self.offsets = struct.unpack_from(f">{count}H", data, self.range_base)

    def glyph(self, data: bytes, cp: int, seg: int) -> int:
        """Resolve one codepoint's glyph id inside segment ``seg``."""
        if self.offsets[seg] == 0:
            return (cp + self.deltas[seg]) & 0xFFFF
        at = self.range_base + 2 * seg + self.offsets[seg] + 2 * (cp - self.starts[seg])
        if at + 2 > len(data):
            _raise("format 4 glyph array runs past end of file")
        (glyph,) = struct.unpack_from(">H", data, at)
        return (glyph + self.deltas[seg]) & 0xFFFF if glyph != 0 else 0


def _format12(data: bytes, base: int) -> set[int]:
    """Enumerate a segmented-coverage (format 12) subtable, skipping glyph 0."""
    (groups,) = struct.unpack_from(">I", data, base + 12)
    out: set[int] = set()
    for group in range(groups):
        at = base + 16 + 12 * group
        first, last, glyph = struct.unpack_from(">III", data, at)
        if first > last or last > MAX_CODEPOINT:
            _raise("format 12 group outside the Unicode range")
        out |= {cp for cp in range(first, last + 1) if glyph + (cp - first) != 0}
    return out


def _format6(data: bytes, base: int) -> set[int]:
    """Enumerate a trimmed-table (format 6) subtable, skipping glyph 0."""
    first, count = struct.unpack_from(">HH", data, base + 6)
    glyphs = struct.unpack_from(f">{count}H", data, base + 10)
    return {first + i for i, glyph in enumerate(glyphs) if glyph != 0}


_SUBTABLE_READERS: dict[int, Callable[[bytes, int], set[int]]] = {
    4: _format4,
    6: _format6,
    12: _format12,
}


def font_codepoints(path: Path) -> frozenset[int]:
    """Return every codepoint ``path``'s Unicode ``cmap`` subtables map to a glyph.

    Raises :class:`FontParseError` on anything this gate cannot read, rather
    than returning an empty set: an empty set would satisfy "nothing
    undeclared" and report a clean run over a font it never opened.
    """
    try:
        data = path.read_bytes()
    except OSError as exc:
        _raise(f"unreadable: {exc.strerror}", exc)
    cmap = _table_offsets(data).get("cmap")
    if cmap is None:
        _raise("no cmap table")
    (records,) = struct.unpack_from(">H", data, cmap + 2)
    out: set[int] = set()
    seen_unicode = False
    for index in range(records):
        platform, encoding, offset = struct.unpack_from(">HHI", data, cmap + 4 + 8 * index)
        windows_unicode = platform == _PLATFORM_WINDOWS and encoding in _WINDOWS_UNICODE_ENCODINGS
        if platform != _PLATFORM_UNICODE and not windows_unicode:
            continue
        (fmt,) = struct.unpack_from(">H", data, cmap + offset)
        reader = _SUBTABLE_READERS.get(fmt)
        if reader is None:
            continue
        seen_unicode = True
        out |= reader(data, cmap + offset)
    if not seen_unicode:
        _raise("no readable Unicode cmap subtable")
    return frozenset(out)


def as_ranges(codepoints: Iterable[int]) -> list[tuple[int, int]]:
    """Collapse ``codepoints`` into ascending, non-adjacent inclusive ranges."""
    out: list[list[int]] = []
    for cp in sorted(set(codepoints)):
        if out and cp == out[-1][1] + 1:
            out[-1][1] = cp
        else:
            out.append([cp, cp])
    return [(lo, hi) for lo, hi in out]


def range_text(lo: int, hi: int) -> str:
    """Render one inclusive range the way the declaration spells it."""
    return f"{lo:04X}" if lo == hi else f"{lo:04X}-{hi:04X}"


def recipe(codepoints: Iterable[int]) -> str:
    """Return the ``pyftsubset --unicodes`` argument for ``codepoints``."""
    return ",".join(range_text(lo, hi) for lo, hi in as_ranges(codepoints))


class Block:
    """One declared font: its path, the face it was cut from, and its ranges."""

    def __init__(self, font: str, source: str, line: int) -> None:
        """Record one declared font, its source face, and the row it came from."""
        self.font = font
        self.source = source
        self.line = line
        self.ranges: list[tuple[int, int, int]] = []

    @property
    def codepoints(self) -> set[int]:
        """Every codepoint the declared ranges cover."""
        return {cp for lo, hi, _ in self.ranges for cp in range(lo, hi + 1)}


def _parse_range(rest: str) -> tuple[int, int]:
    """Parse ``0020-007E`` (or a bare ``2026``) into an inclusive pair."""
    spec = rest.split(None, 1)[0]
    lo_text, _, hi_text = spec.partition("-")
    lo = int(lo_text, 16)
    hi = int(hi_text, 16) if hi_text else lo
    if lo > hi or hi > MAX_CODEPOINT:
        message = f"range {spec} is not an ascending Unicode range"
        raise ValueError(message)
    return lo, hi


def parse_declaration(text: str) -> tuple[list[Block], dict[str, int], list[str]]:
    """Read the declaration into blocks, ``#!`` directives, and malformed rows."""
    blocks: list[Block] = []
    directives: dict[str, int] = {}
    errors: list[str] = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if line.startswith("#!"):
            errors += _read_directive(line, number, directives)
        elif not line or line.startswith("#"):
            continue
        else:
            errors += _read_row(line, number, blocks)
    return blocks, directives, errors


def _read_directive(line: str, number: int, directives: dict[str, int]) -> list[str]:
    """Record one ``#! key: N`` count directive, or report it as malformed."""
    key, _, value = line[2:].partition(":")
    if key.strip() in DIRECTIVE_KEYS and value.strip().isdigit():
        directives[key.strip()] = int(value.strip())
        return []
    return [f"line {number}: unknown count directive {line!r}"]


def _read_row(line: str, number: int, blocks: list[Block]) -> list[str]:
    """Record one ``font`` or ``range`` row, or report it as malformed."""
    head, _, rest = line.partition(" ")
    if head == "font":
        parts = rest.split()
        if len(parts) != _FONT_ROW_FIELDS or parts[1] != "source":
            return [f"line {number}: expected 'font <path> source <path|->'"]
        blocks.append(Block(parts[0], parts[2], number))
        return []
    if head == "range" and blocks:
        try:
            lo, hi = _parse_range(rest)
        except (ValueError, IndexError) as exc:
            return [f"line {number}: {exc}"]
        blocks[-1].ranges.append((lo, hi, number))
        return []
    return [f"line {number}: unexpected row {line!r}"]


def _range_order_failures(block: Block) -> list[str]:
    """Report ranges that are out of order, overlapping, or mergeable."""
    out: list[str] = []
    previous: tuple[int, int, int] | None = None
    for current in block.ranges:
        if previous is not None and current[0] <= previous[1] + 1:
            out.append(
                f"{block.font}: line {current[2]}: range "
                f"{range_text(current[0], current[1])} is not above "
                f"{range_text(previous[0], previous[1])} with a gap"
            )
        previous = current
    return out


def _coverage_failures(block: Block, declared: set[int], present: frozenset[int]) -> list[str]:
    """Report the two directions of drift between declaration and font."""
    out: list[str] = []
    missing = sorted(declared - present)
    extra = sorted(present - declared)
    if missing:
        out.append(
            f"{block.font}: declared but NOT in the font: {recipe(missing)} "
            f"({len(missing)} codepoint(s))"
        )
    if extra:
        out.append(
            f"{block.font}: in the font but NOT declared: {recipe(extra)} "
            f"({len(extra)} codepoint(s))"
        )
    return out


def _block_failures(block: Block, reader: Callable[[Path], frozenset[int]]) -> list[str]:
    """Hold one declared font against its own bytes and its source face."""
    out = _range_order_failures(block)
    declared = block.codepoints
    try:
        present = reader(REPO_ROOT / block.font)
    except FontParseError as exc:
        return [*out, f"{block.font}: line {block.line}: {exc}"]
    out += _coverage_failures(block, declared, present)
    if block.source == "-":
        return out
    try:
        source = reader(REPO_ROOT / block.source)
    except FontParseError as exc:
        return [*out, f"{block.source}: line {block.line}: source face {exc}"]
    absent = sorted(declared - source)
    if absent:
        out.append(
            f"{block.source}: source face cannot supply {recipe(absent)}, "
            f"so {block.font} is not reproducible from it"
        )
    return out


def _directive_failures(blocks: Sequence[Block], directives: dict[str, int]) -> list[str]:
    """Re-derive the ``#!`` counts from the rows and report any disagreement."""
    measured = {
        "fonts": len(blocks),
        "ranges": sum(len(block.ranges) for block in blocks),
        "codepoints": sum(len(block.codepoints) for block in blocks),
    }
    out: list[str] = []
    for key in DIRECTIVE_KEYS:
        if key not in directives:
            out.append(f"declaration states no '{key}' count directive")
        elif directives[key] != measured[key]:
            out.append(f"directive '{key}: {directives[key]}' but rows measure {measured[key]}")
    return out


def evaluate(
    blocks: Sequence[Block],
    directives: dict[str, int],
    reader: Callable[[Path], frozenset[int]],
) -> list[str]:
    """Hold the declaration against the fonts and return every finding."""
    out: list[str] = []
    for block in blocks:
        if not block.ranges:
            out.append(f"{block.font}: line {block.line}: declares no range")
            continue
        out += _block_failures(block, reader)
    return out + _directive_failures(blocks, directives)


def _fixture_reader(coverage: dict[str, frozenset[int]]) -> Callable[[Path], frozenset[int]]:
    """Return a reader serving ``coverage`` by repo-relative path, for the selftest."""

    def read(path: Path) -> frozenset[int]:
        key = path.relative_to(REPO_ROOT).as_posix()
        if key not in coverage:
            _raise("unreadable: No such file or directory")
        return coverage[key]

    return read


_FIXTURE = """\
#! fonts: 1
#! ranges: 2
#! codepoints: 4
font a.ttf source b.ttf
range 0020-0021
range 0041-0042
"""


def _fixture_case(text: str, coverage: dict[str, frozenset[int]]) -> list[str]:
    """Evaluate one synthetic declaration against one synthetic set of fonts."""
    blocks, directives, errors = parse_declaration(text)
    return errors + evaluate(blocks, directives, _fixture_reader(coverage))


def _synthetic_cases() -> list[tuple[str, str, dict[str, frozenset[int]], bool]]:
    """Return (label, declaration, coverage, expect_findings) for every rule."""
    full = frozenset({0x20, 0x21, 0x41, 0x42})
    both = {"a.ttf": full, "b.ttf": full}
    no_range = "#! fonts: 1\n#! ranges: 0\n#! codepoints: 0\nfont a.ttf source b.ttf\n"
    return [
        ("correct fixture quiet", _FIXTURE, both, False),
        ("declared codepoint absent", _FIXTURE, {"a.ttf": full - {0x42}, "b.ttf": full}, True),
        ("undeclared codepoint present", _FIXTURE, {"a.ttf": full | {0x43}, "b.ttf": full}, True),
        ("source face short", _FIXTURE, {"a.ttf": full, "b.ttf": full - {0x20}}, True),
        ("font file missing", _FIXTURE, {"b.ttf": full}, True),
        ("source file missing", _FIXTURE, {"a.ttf": full}, True),
        ("stale range count", _FIXTURE.replace("ranges: 2", "ranges: 3"), both, True),
        ("stale codepoint count", _FIXTURE.replace("codepoints: 4", "codepoints: 5"), both, True),
        ("dropped directive", _FIXTURE.replace("#! fonts: 1\n", ""), both, True),
        ("adjacent rows", _FIXTURE.replace("range 0041-0042", "range 0022-0023"), both, True),
        ("descending range", _FIXTURE.replace("range 0041-0042", "range 0042-0041"), both, True),
        ("unknown row", _FIXTURE + "glyphs 40\n", both, True),
        ("font row with no range", no_range, both, True),
    ]


def selftest_failures() -> list[str]:
    """Assert every rule fires when it should and stays quiet when it should not."""
    out: list[str] = []
    for label, text, coverage, expect in _synthetic_cases():
        findings = _fixture_case(text, {k: frozenset(v) for k, v in coverage.items()})
        if bool(findings) != expect:
            out.append(f"selftest: {label}: findings={findings!r}")
    return out + _live_selftest_failures()


def _live_selftest_failures() -> list[str]:
    """Assert the committed declaration and the committed fonts agree right now."""
    blocks, directives, errors = parse_declaration(DECLARATION.read_text(encoding="utf-8"))
    if errors:
        return [f"selftest: committed declaration does not parse: {errors!r}"]
    if not blocks:
        return ["selftest: committed declaration declares no font"]
    out: list[str] = []
    findings = evaluate(blocks, directives, font_codepoints)
    if findings:
        out.append(f"selftest: committed declaration is not quiet: {findings!r}")
    for block in blocks:
        narrowed = Block(block.font, block.source, block.line)
        narrowed.ranges = block.ranges[:-1]
        if not evaluate([narrowed], directives, font_codepoints):
            out.append(f"selftest: dropping a range from {block.font} was not reported")
    return out


def _report(blocks: Sequence[Block], as_recipe: bool) -> int:
    """Print the measured coverage of each declared font, or its subset recipe."""
    for block in blocks:
        try:
            present = font_codepoints(REPO_ROOT / block.font)
        except FontParseError as exc:
            sys.stderr.write(f"{block.font}: {exc}\n")
            return 2
        if as_recipe:
            print(f"{block.font}: --unicodes='{recipe(present)}'")
            continue
        print(f"{block.font}: {len(present)} codepoint(s)")
        for lo, hi in as_ranges(present):
            print(f"  {range_text(lo, hi)}")
    return 0


def _run_selftest() -> int:
    """Print the selftest verdict and return its exit status."""
    failures = selftest_failures()
    for failure in failures:
        sys.stderr.write(f"{failure}\n")
    print(f"check_font_coverage: selftest {'FAILED' if failures else 'passed'}")
    return 1 if failures else 0


def main() -> int:
    """Run the gate, its selftest, or print what the declaration covers."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true", help="assert every rule both ways")
    parser.add_argument("--list", action="store_true", help="print the measured coverage")
    parser.add_argument("--recipe", action="store_true", help="print the pyftsubset arguments")
    args = parser.parse_args()

    if args.selftest:
        return _run_selftest()

    blocks, directives, errors = parse_declaration(DECLARATION.read_text(encoding="utf-8"))
    if not blocks:
        sys.stderr.write(f"{DECLARATION}: declares no font; refusing a vacuous pass\n")
        return 2
    if args.list or args.recipe:
        return _report(blocks, args.recipe)
    findings = errors + evaluate(blocks, directives, font_codepoints)
    for finding in findings:
        sys.stderr.write(f"{DECLARATION.name}: {finding}\n")
    total = sum(len(block.codepoints) for block in blocks)
    print(
        f"check_font_coverage: {len(blocks)} font(s), {total} declared codepoint(s), "
        f"{len(findings)} finding(s)"
    )
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
