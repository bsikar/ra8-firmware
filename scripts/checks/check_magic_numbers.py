#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: no magic numbers -- every integer literal must live in a named
typed enum; floating-point constants (which C enums cannot hold) use a
``const``.  Macros are NOT an acceptable home for an integer constant
(CLAUDE.md "Constants and Macros": enums always, macros only for code
de-duplication, conditional compilation, or build-configuration flags).

The project's `.clang-tidy` already configures
``readability-magic-numbers``, but clang-tidy only checks files that are
present in the build's ``compile_commands.json``.  ``clang_tidy.sh`` runs
against the host unit-test build, which drops every ARM-cross-compiled
translation unit and -- crucially -- contains **no** example ``main.c``
at all.  The result was that every ``examples/<tier>/.../<app>/main.c``
was invisible to the magic-number check (a bare ``ra8_delay_ms(500U)``
sailed straight through both the pre-commit hook and CI).

This checker is a backstop that walks the source text directly so the
rule is enforced for **every** ``.c`` file under ``libs/``, ``src/``,
``port/``, and ``examples/`` regardless of which compile database it
ended up in.  It is the magic-number analogue of
``check_function_size.py``.

Detection rules (kept deliberately aligned with the ``.clang-tidy``
``readability-magic-numbers`` configuration):

* Numeric literals (decimal, ``0x`` hex, ``0b`` binary, octal, and
  floating-point, with optional ``u``/``l``/``f`` suffixes) are flagged.
* The ignored-value set matches ``.clang-tidy``:
  integers ``0;1;2;3;4;6;8;16;32`` and floats ``0.0;1.0``.
* Literals inside an ``enum`` body are the *allowed* home for a constant
  and are never flagged.
* Preprocessor directives (``#define``, ``#if`` ...) are skipped here --
  but note that an object-like ``#define FOO 500`` is itself a policy
  violation (integer constants must be enums); the pre-commit hook's
  "bare numeric #define" gate is what flags that pattern.
* Numbers embedded in identifiers (``uint8_t``, ``crc32``, ``s_buf2``)
  are not literals and are never flagged.
* Comments and string / character literals are stripped before scanning.
* clang-tidy ``NOLINT`` suppressions are honoured: a
  ``NOLINT`` / ``NOLINTNEXTLINE`` / ``NOLINTBEGIN`` .. ``NOLINTEND`` that
  is bare or lists ``readability-magic-numbers`` (or the
  ``cppcoreguidelines-avoid-magic-numbers`` alias) suppresses the same
  lines a clang-tidy run would.  A backstop for clang-tidy must respect
  clang-tidy's own waiver mechanism -- the codebase uses it heavily to
  exempt the crypto, JPEG-codec, and BLE-host translation units.

Per-line opt-out: append ``MAGIC-OK: <reason>`` to a line to suppress
it (mirrors the ``LEGACY-OK`` / ``WAVE-OK`` / ``AI-OK`` opt-outs used by
the sibling gates).  Use it sparingly and only with a written reason;
prefer a scoped ``NOLINT`` where a whole block is genuinely exempt.

Run::

    check_magic_numbers.py                      # scan the whole tree
    check_magic_numbers.py path/to/file.c ...   # scan listed files

Exit 0 if no magic numbers remain, exit 1 (with a diagnostic table) if
any are found.
"""

from __future__ import annotations

import math
import re
import shutil
import subprocess
import sys
from collections.abc import Iterable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Under ``examples/`` only the application ``main.c`` is scanned -- this
# matches the scope ``clang_tidy.sh`` already uses for examples.  The
# per-app boot boilerplate (``vector_table.c``, ``system_init.c``,
# ``secure_exception.c``, ``trustzone_init.c``) is copied verbatim into
# every app and is full of sequential IRQ-slot indices and fixed vector
# addresses; flagging it would bury real application magic numbers under
# ~200 boilerplate hits per app.  Pass such a file explicitly to scan it.
# Per-app boot boilerplate -- copied verbatim into every app under examples/
# AND src/app/, and full of sequential IRQ-slot indices and fixed vector
# addresses. Exempt by filename anywhere (not just examples/), matching the
# examples/<app> main.c-only scope above. Pass such a file explicitly to scan.
BOOT_BOILERPLATE = frozenset(
    {"vector_table.c", "system_init.c", "secure_exception.c", "trustzone_init.c"}
)

# Path fragments that exclude the file from the scan.  Vendor / build
# trees are SOUP and exempt; their literals are the upstream
# maintainer's call, not ours.  Test code is exempt too: unit tests are
# built from literal stimulus and expected-value vectors
# (``TEST_ASSERT_EQ(0x9D5A1A, ...)``) -- naming every test constant as a
# typed enum would obscure the vectors, not clarify them.
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "libs/fonts/",
    "port/threadx/",
    "tests/",
)

# Integer values exempt from the rule.  Mirrors
# ``.clang-tidy``: readability-magic-numbers.IgnoredIntegerValues.
IGNORED_INT = {0, 1, 2, 3, 4, 6, 8, 16, 32}

# Floating-point values exempt from the rule.  Mirrors
# ``.clang-tidy``: readability-magic-numbers.IgnoredFloatingPointValues.
IGNORED_FLOAT = {0.0, 1.0}

# Per-line opt-out marker.
OPT_OUT = "MAGIC-OK"

# clang-tidy check names whose NOLINT suppressions this gate honours.  The
# project uses NOLINTBEGIN/END(readability-magic-numbers, ...) extensively
# to waive whole files / functions (crypto, JPEG codec, BLE host, ...); a
# backstop for clang-tidy must respect clang-tidy's own suppression
# mechanism or it would override author-sanctioned exemptions.
MAGIC_CHECK_NAMES = (
    "readability-magic-numbers",
    "cppcoreguidelines-avoid-magic-numbers",
)

_NOLINT_BEGIN_RE = re.compile(r"NOLINTBEGIN(?:\(([^)]*)\))?")
_NOLINT_END_RE = re.compile(r"NOLINTEND(?:\(([^)]*)\))?")
_NOLINT_NEXT_RE = re.compile(r"NOLINTNEXTLINE(?:\(([^)]*)\))?")
_NOLINT_INLINE_RE = re.compile(r"NOLINT(?!BEGIN|END|NEXTLINE)(?:\(([^)]*)\))?")


def _nolint_applies(arg: str | None) -> bool:
    """Return True if a NOLINT marker with arg list `arg` suppresses the
    magic-number check.  A bare ``NOLINT`` (arg is None/empty) suppresses
    every check, so it applies too."""
    if arg is None or arg.strip() == "":
        return True
    return any(name in arg for name in MAGIC_CHECK_NAMES)


# A numeric literal: hex, binary, octal, float, or decimal, each with an
# optional integer/float suffix.  Order matters -- hex/binary/float must
# be tried before the bare-decimal alternative.
_NUM_RE = re.compile(
    r"""
    (?P<hex>0[xX][0-9a-fA-F]+(?:[uUlL]*))
  | (?P<bin>0[bB][01]+(?:[uUlL]*))
  | (?P<flt>(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?[fF])
  | (?P<dec_flt>(?:\d+\.\d*|\.\d+)(?:[eE][+-]?\d+)?[lL]?)
  | (?P<dec>\d+(?:[uUlL]*))
    """,
    re.VERBOSE,
)

# Characters that, immediately before a match, mean the digits are part
# of an identifier (uint8_t, crc32) or a larger numeric token rather
# than a standalone literal.
_IDENT_BEFORE = re.compile(r"[0-9A-Za-z_.]")

# A "data row": a line (after comment/string stripping) that holds only
# numeric literals, commas, braces, signs and whitespace -- i.e. a row
# of a const lookup table (font glyphs, JPEG quant tables, crypto
# S-boxes, MIPI PHY register sequences).  These are data, not logic; the
# magic-number rule is meaningless for them, exactly as clang-tidy never
# saw them (the data-table TUs are absent from the host compile-db).  A
# real magic number always rides on an identifier or operator
# (`ra8_delay_ms(500U)`, `buf[0] = 500`, `x << 7`), which breaks the
# pattern below and is still flagged.
_DATA_ROW_RE = re.compile(r"^[\s{}\[\],0-9a-fA-FxXuUlL.+\-]*$")

# A declaration line carries a storage class, qualifier, or type token.
# clang-tidy ignores an array-*dimension* literal in a declaration
# (``uint8_t z[64]``) but still flags an array *subscript* (``b[5]``);
# the dimension only counts as a magic number on a declaration line.
_DECL_HINT = re.compile(
    r"\b(static|const|extern|volatile|register|struct|union|enum|"
    r"unsigned|signed|void|char|short|int|long|float|double|bool|"
    r"[A-Za-z_][A-Za-z0-9_]*_t)\b"  # uint8_t, int32_t, size_t, my_type_t, ...
)

# A `const` float/double definition is the sanctioned home for a
# floating-point constant (C enums cannot hold floats -- see CLAUDE.md
# "Constants and Macros").  A float literal initialising such a
# definition is the named value itself, analogous to an enum member, and
# is not a magic number -- exactly as `1.0`/`0.0` are already ignored.
_CONST_FLOAT_DEF = re.compile(r"\bconst\b")
_FLOAT_TYPE = re.compile(r"\b(float|double)\b")


def _strip_comments_and_strings(text: str) -> str:
    """Replace comment and string/char-literal bytes with spaces while
    preserving newlines (and therefore line and column positions)."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and nxt == "*":
            out.append("  ")
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append("  ")
                i += 2
            continue
        if c in {'"', "'"}:
            quote = c
            out.append(" ")
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append(" ")
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _literal_value(match: re.Match) -> tuple[bool, float]:
    """Return (is_float, value) for a numeric-literal match, or
    (False, NaN) if it should be ignored as un-parseable."""
    raw = match.group(0)
    group = match.lastgroup
    # Strip only true literal suffixes -- and never strip `f` from a hex
    # literal, where it is a hex digit (0xAF), not a float suffix.
    if group == "hex":
        body = raw.rstrip("uUlL")
        return False, float(int(body, 16))
    if group == "bin":
        body = raw.rstrip("uUlL")
        return False, float(int(body, 2))
    if group in ("flt", "dec_flt"):
        body = raw.rstrip("fFlL")
        try:
            return True, float(body)
        except ValueError:
            return False, float("nan")
    body = raw.rstrip("uUlL")
    # Leading-zero decimals are octal in C (0700 == 448); fall back to
    # base 8, then base 10, before giving up.
    for base in (0, 8, 10):
        try:
            return False, float(int(body, base))
        except ValueError:  # noqa: PERF203  # per-base fallback; loop is only 3 iterations
            continue
    return False, float("nan")


def _is_array_dimension(code_line: str, start: int, end: int) -> bool:
    """Return True if the literal at [start, end) is the sole content of a
    ``[ ... ]`` on a declaration line -- an array dimension, which
    clang-tidy does not treat as a magic number."""
    i = start - 1
    while i >= 0 and code_line[i].isspace():
        i -= 1
    if i < 0 or code_line[i] != "[":
        return False
    j = end
    while j < len(code_line) and code_line[j].isspace():
        j += 1
    if j >= len(code_line) or code_line[j] != "]":
        return False
    return bool(_DECL_HINT.search(code_line))


def _is_ignored(is_float: bool, value: float) -> bool:
    if math.isnan(value):  # could not parse, do not flag
        return True
    if is_float:
        return value in IGNORED_FLOAT
    return int(value) in IGNORED_INT


class _SuppressionState:
    """Tracks clang-tidy NOLINT scope across the lines of one file.

    Honours the project's own readability-magic-numbers waivers, so a literal
    this gate would flag but clang-tidy is already told to ignore is not
    reported twice under two different names.
    """

    def __init__(self) -> None:
        self.in_region = False  # inside a magic-relevant NOLINTBEGIN/END block
        self.skip_next = False  # previous line was a magic-relevant NOLINTNEXTLINE

    def suppresses(self, raw: str) -> bool:
        """True when ``raw`` is inside, or is itself, a NOLINT suppression."""
        m_beg = _NOLINT_BEGIN_RE.search(raw)
        if m_beg and _nolint_applies(m_beg.group(1)):
            self.in_region = True
        m_end = _NOLINT_END_RE.search(raw)
        if m_end and _nolint_applies(m_end.group(1)):
            self.in_region = False
        if self.in_region or m_beg or m_end:
            return True
        if self.skip_next:
            self.skip_next = False
            return True
        m_nl = _NOLINT_NEXT_RE.search(raw)
        if m_nl and _nolint_applies(m_nl.group(1)):
            self.skip_next = True
            return True
        m_inl = _NOLINT_INLINE_RE.search(raw)
        return bool(m_inl and _nolint_applies(m_inl.group(1)))


class _EnumState:
    """Tracks brace depth inside an enum body.

    Literals inside an enum ARE the named constants this gate exists to
    require, so the body is skipped rather than reported.
    """

    def __init__(self) -> None:
        self.depth = 0
        self.pending = False  # saw `enum`, awaiting its `{`

    def inside(self, code_line: str) -> bool:
        """Advance the tracker over ``code_line``; True if it is enum body."""
        if self.depth == 0 and re.search(r"\benum\b", code_line):
            self.pending = True
        if not (self.pending or self.depth > 0):
            return False
        opens = code_line.count("{")
        closes = code_line.count("}")
        if self.pending and opens > 0:
            self.pending = False
            self.depth += opens - closes
            return True  # the `... enum ... {` line itself is a definition
        self.depth = max(self.depth + opens - closes, 0)
        return self.depth > 0


def _line_literals(code_line: str) -> list[tuple[int, str]]:
    """Return (column, literal) for every magic number on one line of code."""
    out: list[tuple[int, str]] = []
    for m in _NUM_RE.finditer(code_line):
        start = m.start()
        if start > 0 and _IDENT_BEFORE.match(code_line[start - 1]):
            continue
        if _is_array_dimension(code_line, start, m.end()):
            continue
        is_float, value = _literal_value(m)
        if _is_ignored(is_float, value):
            continue
        if is_float and _CONST_FLOAT_DEF.search(code_line) and _FLOAT_TYPE.search(code_line):
            continue  # named const float/double definition
        out.append((start + 1, m.group(0)))
    return out


def _skip_line(code_line: str, orig_line: str, enums: _EnumState) -> bool:
    """True when this line cannot hold a reportable literal."""
    if enums.inside(code_line):
        return True
    stripped = code_line.lstrip()
    # Preprocessor directives are governed by other gates.
    if stripped.startswith("#"):
        return True
    # Pure data rows of a const lookup table are not logic.
    if stripped and _DATA_ROW_RE.match(code_line):
        return True
    # Per-line opt-out (checked against the original source line).
    return OPT_OUT in orig_line


def _scan_file(path: Path) -> list[tuple[int, int, str]]:
    """Return a list of (line, column, literal) for every magic number in `path`."""
    try:
        text = path.read_text()
    except (OSError, UnicodeDecodeError):
        return []

    code_lines = _strip_comments_and_strings(text).splitlines()
    orig_lines = text.splitlines()

    violations: list[tuple[int, int, str]] = []
    nolint = _SuppressionState()
    enums = _EnumState()

    for idx, code_line in enumerate(code_lines):
        raw = orig_lines[idx] if idx < len(orig_lines) else ""
        if nolint.suppresses(raw):
            continue
        if _skip_line(code_line, raw, enums):
            continue
        violations.extend((idx + 1, col, lit) for col, lit in _line_literals(code_line))

    return violations


def _is_excluded(path: Path) -> bool:
    p = str(path)
    return is_build_output_path(p) or any(frag in p for frag in EXCLUDE_FRAGMENTS)


def _enumerate_targets(arg_paths: Iterable[str]) -> list[Path]:
    """Resolve the list of files to scan from CLI arguments."""
    args = [a for a in arg_paths if a not in ("--check", "--all")]
    if args:
        out: list[Path] = []
        for raw in args:
            p = Path(raw)
            if not p.is_absolute():
                p = REPO_ROOT / p
            if p.is_dir():
                out.extend(p.rglob("*.c"))
            elif p.suffix == ".c":
                out.append(p)
        return [p for p in out if not _is_excluded(p)]

    # No-argument mode scans every GIT-VISIBLE .c so nothing first-party is
    # silently skipped (new top-level dirs, tools/, port/, etc. are covered
    # automatically -- there is no allowlist to forget to update).
    # Enumerating via ``git ls-files`` (tracked plus untracked-but-not-
    # ignored) instead of a filesystem walk keeps gitignored artifacts out:
    # a raw rglob scanned stray CMake compiler-probe files under app
    # build-*/ dirs and the .claude/worktrees checkouts, failing commits on
    # files CI can never see. The per-app boot boilerplate (vector_table.c,
    # system_init.c, ...) is copied verbatim into every app and is full of
    # vector/IRQ-slot indices, so it is dropped BY FILENAME via
    # BOOT_BOILERPLATE. Vendor trees are dropped via EXCLUDE_FRAGMENTS.
    #
    # There is deliberately no longer an "examples/ scans only main.c" rule.
    # BOOT_BOILERPLATE already names the copied boot files precisely, so that
    # extra filename test protected nothing -- it silently dropped every OTHER
    # example TU: each app's src/*.c, plus cpu1_main.c, ns_main.c and ns_usb.c.
    # 47 real magic numbers were sitting in those files, among them a whole
    # block of hand-addressed Armv8-M SAU registers, while this gate reported
    # the tree clean. Scope by what a file IS, never by what it is named
    # (the #296 / #332 / #358 / #369 defect family).
    git_tool = shutil.which("git") or "git"
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [git_tool, "ls-files", "--cached", "--others", "--exclude-standard", "--", "*.c"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"git ls-files failed (exit {proc.returncode})\n")
        sys.exit(2)
    out = []
    for line in proc.stdout.splitlines():
        rel = line.strip()
        if not rel:
            continue
        c = REPO_ROOT / rel
        if c.name in BOOT_BOILERPLATE:
            continue
        out.append(c)
    return [p for p in out if not _is_excluded(p)]


def main(argv: list[str]) -> int:
    targets = _enumerate_targets(argv[1:])
    if not targets:
        print("check_magic_numbers.py: no files to scan", file=sys.stderr)
        return 0

    findings: list[tuple[str, int, int, str]] = []
    for path in targets:
        for line_no, col, literal in _scan_file(path):
            rel = path.relative_to(REPO_ROOT) if path.is_relative_to(REPO_ROOT) else path
            findings.append((str(rel), line_no, col, literal))

    if not findings:
        print(f"check_magic_numbers.py: {len(targets)} file(s) scanned, no magic numbers found.")
        return 0

    findings.sort()
    print(
        f"check_magic_numbers.py: {len(findings)} magic number(s) found "
        "-- every integer literal must be a named typed enum (use a const "
        "only for floating-point; macros are not allowed for constants):\n",
        file=sys.stderr,
    )
    print("  file:line:col  literal", file=sys.stderr)
    for path, line_no, col, literal in findings:
        print(f"  {path}:{line_no}:{col}  {literal}", file=sys.stderr)
    print(
        "\nReplace each literal with a named value. Genuine exceptions may "
        f"carry a trailing `{OPT_OUT}: <reason>` comment on the line.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
