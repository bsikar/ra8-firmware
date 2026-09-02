#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_c23_patterns.py -- enforce four C23 source patterns on first-party code.

These four rules previously lived ONLY as inline ``grep`` loops inside the
local ``scripts/git/pre-commit`` hook and were never run by the CI gate
``pre-commit-checks`` (``gate_pre_commit_checks`` in
``scripts/ci/gates/checks.sh``).  The workflow claimed the gate mirrored the
hook, but these four checks were absent from it -- a "local green, CI red"
drift in the other direction, where a violation the hook rejects sails through
CI on a machine whose hook is not installed.  This checker is the single
first-party implementation both the hook and the gate now call, so there is
exactly one definition of each rule.

The rules (CLAUDE.md "C23 Syntax" and "Constants and Macros"):

  1. ``_Static_assert(`` -- C11 spelling; C23 provides ``static_assert``.
  2. ``= {0}`` and typed variants such as ``= {0U}`` -- legacy
     zero-initializers; C23 uses ``= {}``.
  3. ``#include <stdbool.h>`` -- unnecessary; ``bool`` is a C23 keyword.
  4. Object-like ``#define NAME <bare-numeric-literal>`` -- the value must be
     paren-wrapped (``#define NAME (1000)``) so it stays a single token in
     every expression context.  Function-like macros, bare feature flags, and
     already-parenthesised values are ignored; hex / binary / float / suffix
     literal forms are all recognised.

Scope:
  C / C++ sources (.c .h .cpp .hpp) under libs/, src/, examples/, port/,
  tools/, tests/.  Vendored SOUP (libs/third_party/) and generated font data
  (libs/ra8_fonts/) are exempt, as are build trees.

Matches inside comments and string / character literals are ignored. All four
rules share one raw-aware phase-2 logical lexer and physical-source origin map.
Its zero-initializer view retains each literal as a non-whitespace token, so a
string or character literal remains a real second initializer.

Usage:
    python3 scripts/checks/check_c23_patterns.py FILE [FILE ...]
    python3 scripts/checks/check_c23_patterns.py            # staged files
    python3 scripts/checks/check_c23_patterns.py --all      # every tracked file
    python3 scripts/checks/check_c23_patterns.py --selftest # prove the rules fire

Returns 0 on clean, 1 on findings, 2 on usage / selftest failure.
"""

from __future__ import annotations

import argparse
import bisect
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path

REPO_ROOT = Path(__file__).resolve().parents[2]

# First-party roots that carry hand-authored C. Mirrors check_no_gnu_attribute.
ROOTS = ("libs", "examples", "port", "tools", "apps", "tests")
EXTS = (".c", ".h", ".cpp", ".hpp")
# Path fragments that exclude a file: vendored SOUP and generated font tables.
EXEMPT_DIRS = ("/third_party/", "/ra8_fonts/")

# Rule 1: C11 _Static_assert at the start of a line (after leading whitespace).
# C23 spells it `static_assert`.
_STATIC_ASSERT_RE = re.compile(r"^\s*_Static_assert\s*\(")

# Rule 2: legacy single-zero initializers. The literal may carry an integer
# type suffix (0U, 0UL, 0ULL, 0wb, 0z) or use an all-zero
# decimal/octal/hexadecimal/binary spelling with C23 digit separators. C23's
# empty initializer is the one canonical first-party form. The scanner owns
# both C23 and C++23 sources, so the suffix union includes C++'s z/Z forms.
_STANDARD_INT_SUFFIX = r"(?:[uU](?:(?:ll|LL)|[lL])?|(?:(?:ll|LL)|[lL])[uU]?|)"
_BIT_PRECISE_SUFFIX = r"(?:[uU]?(?:wb|WB)|(?:wb|WB)[uU]?)"
_SIZE_SUFFIX = r"(?:[uU]?[zZ]|[zZ][uU]?)"
_ZERO_SUFFIX = rf"(?:{_STANDARD_INT_SUFFIX}|{_BIT_PRECISE_SUFFIX}|{_SIZE_SUFFIX})"
_ZERO_DIGITS = r"0(?:'?0)*"
_ZERO_BODY = rf"(?:{_ZERO_DIGITS}|0[xX]{_ZERO_DIGITS}|0[bB]{_ZERO_DIGITS})"
_ZERO_INIT_RE = re.compile(rf"=\s*\{{\s*{_ZERO_BODY}{_ZERO_SUFFIX}\s*,?\s*\}}")

# Rule 3: `#include <stdbool.h>` -- unnecessary, `bool` is a C23 keyword.
_STDBOOL_RE = re.compile(r"^\s*#\s*include\s+<stdbool\.h>")

# Rule 4: object-like `#define NAME <bare-numeric-literal>` whose value is not
# paren-wrapped. Faithful translation of the ERE in scripts/git/pre-commit:
# a bare integer or float literal (with optional U/L/F suffix, and hex / binary
# / exponent forms), ignoring function-like macros, bare feature flags, and
# already-parenthesised values. The trailing comment group is retained for
# fidelity; the blanking below turns any real comment to spaces, which the
# leading `\s*` absorbs.
_BARE_DEFINE_RE = re.compile(
    r"^\s*#\s*define\s+[A-Za-z_][A-Za-z0-9_]*\s+"
    r"[+-]?(?:"
    r"0[xX][0-9a-fA-F]+[uUlL]*"
    r"|0[bB][01]+[uUlL]*"
    r"|[0-9]+\.[0-9]*(?:[eE][+-]?[0-9]+)?[fFlL]?"
    r"|\.[0-9]+(?:[eE][+-]?[0-9]+)?[fFlL]?"
    r"|[0-9]+(?:[eE][+-]?[0-9]+)?[fFlLuU]*"
    r")\s*(?:/[/*].*)?$"
)

# Each rule: (id, compiled regex, human-facing message). The id doubles as the
# selftest key so a rule cannot be added without a fixture proving both
# directions.
_RULES: tuple[tuple[str, re.Pattern[str], str], ...] = (
    ("static_assert", _STATIC_ASSERT_RE, "C11 _Static_assert -- use C23 static_assert"),
    ("zero_init", _ZERO_INIT_RE, "legacy single-zero initializer -- use C23 = {}"),
    ("stdbool", _STDBOOL_RE, "unnecessary #include <stdbool.h> -- bool is a C23 keyword"),
    ("bare_define", _BARE_DEFINE_RE, "bare numeric #define value -- wrap with parens, e.g. (1000)"),
)

_RAW_PREFIXES = ('u8R"', 'uR"', 'UR"', 'LR"', 'R"')
_RAW_DELIMITER_FORBIDDEN = frozenset(" ()\\\t\v\f\r\n")
_RAW_DELIMITER_MAX = 16
_DIGIT_CHARS = frozenset("0123456789abcdefABCDEF")
_PP_NUMBER_CHARS = frozenset("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_.'")


def _is_digit_separator(text: str, index: int) -> bool:
    """Return whether one apostrophe separates two preprocessing digits."""
    if text[index] != "'" or index == 0 or index + 1 >= len(text):
        return False
    if text[index - 1] not in _DIGIT_CHARS or text[index + 1] not in _DIGIT_CHARS:
        return False
    start = index - 1
    while start > 0 and text[start - 1] in _PP_NUMBER_CHARS:
        start -= 1
    prefix = text[start:index]
    return prefix[0].isdigit() or (
        prefix.startswith(".") and len(prefix) > 1 and prefix[1].isdigit()
    )


def _mask_position(
    text: str,
    code_out: list[str],
    zero_out: list[str],
    index: int,
    *,
    literal: bool,
) -> None:
    """Mask one logical byte in both policy views."""
    if text[index] == "\n":
        code_out[index] = zero_out[index] = "\n"
        return
    code_out[index] = " "
    zero_out[index] = "L" if literal else " "


def _mask_literal_byte(
    text: str,
    code_out: list[str],
    zero_out: list[str],
    index: int,
    quote: str,
) -> tuple[int, str]:
    """Mask one literal byte and return the next offset and lexical state."""
    char = text[index]
    _mask_position(text, code_out, zero_out, index, literal=True)
    if char == "\\" and index + 1 < len(text):
        index += 1
        _mask_position(text, code_out, zero_out, index, literal=True)
        return index + 1, quote
    if char == quote:
        return index + 1, "code"
    return index + 1, quote


def _phase2_source(text: str) -> tuple[str, tuple[int, ...]]:
    """Remove phase-2 line splices and retain each logical byte's origin."""
    logical: list[str] = []
    origins: list[int] = []
    index = 0
    while index < len(text):
        if text[index] == "\\" and index + 1 < len(text) and text[index + 1] == "\n":
            index += 2
            continue
        logical.append(text[index])
        origins.append(index)
        index += 1
    return "".join(logical), tuple(origins)


def _raw_literal_end(
    text: str,
    index: int,
    physical_text: str,
    origins: tuple[int, ...],
) -> int | None:
    """Return the end of a C++ raw literal, masking malformed forms to EOF.

    ``None`` means no raw-literal prefix starts at ``index``. Once a standalone
    prefix is present, malformed delimiters or absent terminators return EOF so
    fake code inside an invalid literal cannot leak into the detector.
    """
    if index > 0 and (text[index - 1].isalnum() or text[index - 1] == "_"):
        return None
    prefix = next((item for item in _RAW_PREFIXES if text.startswith(item, index)), None)
    if prefix is None:
        return None
    cursor = index + len(prefix)
    delimiter: list[str] = []
    terminator: str | None = None
    for _bound in range(_RAW_DELIMITER_MAX + 1):
        if cursor >= len(text):
            break
        char = text[cursor]
        if char == "(":
            terminator = f'){"".join(delimiter)}"'
            break
        if char in _RAW_DELIMITER_FORBIDDEN:
            break
        delimiter.append(char)
        cursor += 1
    if terminator is None:
        return len(text)
    physical_open = origins[cursor]
    close = physical_text.find(terminator, physical_open + 1)
    if close < 0:
        return len(text)
    physical_stop = close + len(terminator)
    return bisect.bisect_left(origins, physical_stop)


def _mask_token_range(
    text: str, code_out: list[str], zero_out: list[str], start: int, stop: int
) -> None:
    """Mask one raw-literal range in both policy views."""
    for offset in range(start, stop):
        _mask_position(text, code_out, zero_out, offset, literal=True)


def _mask_comment_byte(
    text: str,
    code_out: list[str],
    zero_out: list[str],
    index: int,
    state: str,
) -> tuple[int, str]:
    """Mask one comment byte and return the next offset and lexical state."""
    char = text[index]
    nxt = text[index + 1] if index + 1 < len(text) else ""
    if state == "line_comment":
        if char == "\n":
            return index + 1, "code"
        _mask_position(text, code_out, zero_out, index, literal=False)
        return index + 1, state
    if char == "*" and nxt == "/":
        _mask_position(text, code_out, zero_out, index, literal=False)
        _mask_position(text, code_out, zero_out, index + 1, literal=False)
        return index + 2, "code"
    _mask_position(text, code_out, zero_out, index, literal=False)
    return index + 1, state


def _c23_lexical_views(text: str) -> tuple[str, str, tuple[int, ...]]:
    """Build shared line-rule and zero-rule views of phase-2 source.

    The line-rule view blanks comments and literals. The zero-rule view blanks
    comments but retains literals as non-whitespace ``L`` tokens so a literal
    remains a real second initializer. Both views share one raw-aware lexer,
    and the origin table maps each logical byte to its physical source offset.

    Args:
        text: Original C-family source text.

    Returns:
        The line-rule view, zero-rule view, and physical-source origin table.
    """
    logical_text, origins = _phase2_source(text)
    code_out = list(logical_text)
    zero_out = list(logical_text)
    state = "code"
    index = 0
    while index < len(logical_text):
        char = logical_text[index]
        nxt = logical_text[index + 1] if index + 1 < len(logical_text) else ""
        if state == "code":
            raw_end = _raw_literal_end(logical_text, index, text, origins)
            if raw_end is not None:
                _mask_token_range(logical_text, code_out, zero_out, index, raw_end)
                index = raw_end
                continue
            if char == "/" and nxt in {"/", "*"}:
                _mask_position(logical_text, code_out, zero_out, index, literal=False)
                _mask_position(logical_text, code_out, zero_out, index + 1, literal=False)
                state = "line_comment" if nxt == "/" else "block_comment"
                index += 2
                continue
            if char == "'" and _is_digit_separator(logical_text, index):
                index += 1
                continue
            if char in {'"', "'"}:
                state = char
                _mask_position(logical_text, code_out, zero_out, index, literal=True)
        elif state in {"line_comment", "block_comment"}:
            index, state = _mask_comment_byte(logical_text, code_out, zero_out, index, state)
            continue
        else:
            index, state = _mask_literal_byte(logical_text, code_out, zero_out, index, state)
            continue
        index += 1
    return "".join(code_out), "".join(zero_out), origins


def _physical_line(text: str, origins: tuple[int, ...], logical_offset: int) -> int:
    """Map one surviving logical byte to its 1-based physical source line."""
    return text.count("\n", 0, origins[logical_offset]) + 1


def _line_rule_violations(
    text: str,
    code_text: str,
    origins: tuple[int, ...],
    orig_lines: list[str],
) -> list[tuple[int, str, str]]:
    """Evaluate the three line rules on shared phase-2 logical source."""
    violations: list[tuple[int, str, str]] = []
    logical_offset = 0
    for code in code_text.split("\n"):
        first_token = len(code) - len(code.lstrip())
        for rule_id, pattern, _msg in _RULES:
            if rule_id == "zero_init" or pattern.search(code) is None:
                continue
            line_no = _physical_line(text, origins, logical_offset + first_token)
            snippet = (orig_lines[line_no - 1] if line_no <= len(orig_lines) else code).strip()
            violations.append((line_no, rule_id, snippet))
        logical_offset += len(code) + 1
    return violations


def find_violations(path: Path) -> list[tuple[int, str, str]]:
    """Report every C23-pattern violation in one file.

    The scan runs over a comment/string-blanked view of the source so a match
    inside a comment or a string literal is never reported. The phase-2 origin
    table maps logical matches back to exact physical source lines.

    Args:
        path: File to scan.

    Returns:
        A list of ``(line_no, rule_id, snippet)`` tuples, one per finding;
        an unreadable file yields an empty list rather than raising.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    orig_lines = text.splitlines()
    code_text, zero_init_text, origins = _c23_lexical_views(text)
    violations = _line_rule_violations(text, code_text, origins, orig_lines)
    for match in _ZERO_INIT_RE.finditer(zero_init_text):
        line_no = _physical_line(text, origins, match.start())
        snippet = (
            orig_lines[line_no - 1] if line_no <= len(orig_lines) else match.group(0)
        ).strip()
        violations.append((line_no, "zero_init", snippet))
    violations.sort(key=lambda finding: finding[0])
    return violations


def needs_check(path: Path) -> bool:
    """Whether a path is first-party C/C++ subject to the C23 pattern rules.

    Args:
        path: Candidate file path.

    Returns:
        True when the suffix is a C/C++ one and the path is neither a build
        artifact nor under a vendored / generated tree.
    """
    if path.suffix.lower() not in EXTS:
        return False
    posix = path.as_posix()
    if is_build_output_path(posix):
        return False
    return not any(frag in f"/{posix}/" for frag in EXEMPT_DIRS)


def _git_lines(*pathspec: str) -> list[str]:
    """Return tracked repo-relative paths matching `pathspec`.

    Args:
        pathspec: git pathspec arguments (e.g. ``"*.c"``).

    Returns:
        Repo-relative path strings; exits 2 on a git failure.
    """
    # subprocess-security waivers for this one call:
    #   S603 -- the argv is a fixed literal list and shell is never used;
    #           `pathspec` contributes further git pathspec words only, never
    #           an executable name.
    #   S607 -- "git" is left partial deliberately. The gate must run whichever
    #           git the surrounding toolchain resolves (Homebrew on macOS,
    #           /usr/bin/git on the Ubuntu runners, a third path inside the
    #           devcontainer image), and infra/fleet.yml pins no absolute git
    #           path for any declared host.
    proc = subprocess.run(  # noqa: S603 -- fixed literal argv, shell is never used
        ["git", "ls-files", "-z", "--", *pathspec],  # noqa: S607 -- PATH-resolved git is intended
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"git ls-files failed (exit {proc.returncode})\n")
        sys.exit(2)
    return [p for p in proc.stdout.split("\0") if p]


def iter_all_files() -> list[Path]:
    """Every tracked first-party C/C++ file, for the ``--all`` sweep.

    Returns:
        Absolute paths under the first-party roots that pass ``needs_check``.
    """
    out: list[Path] = []
    for root in ROOTS:
        for rel in _git_lines(*(f"{root}/**/*{ext}" for ext in EXTS)):
            p = REPO_ROOT / rel
            if needs_check(p):
                out.append(p)
    return sorted(set(out))


def iter_staged_files() -> list[Path]:
    """Every staged first-party C/C++ file, for the default (hook) mode.

    Returns:
        Absolute paths of added / copied / modified / renamed staged files
        that pass ``needs_check`` and still exist on disk.
    """
    proc = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"],  # noqa: S607 -- trusted git
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"git diff --cached failed (exit {proc.returncode})\n")
        sys.exit(2)
    out: list[Path] = []
    for rel in proc.stdout.split("\0"):
        if not rel:
            continue
        p = REPO_ROOT / rel
        if needs_check(p) and p.is_file():
            out.append(p)
    return out


# ---------------------------------------------------------------------------
# Selftest
#
# Every rule is asserted in BOTH directions: a bad fixture must fire exactly
# that rule, and the matching correct-C23 form must stay silent. A comment and
# a string-literal fixture prove the blanking suppresses matches that are not
# really code. Fixtures are scanned in-memory via a temp file, so nothing a
# checker's own tree scan could later trip over is written into the repo.
# ---------------------------------------------------------------------------

# (rule_id, source, should_fire). ``should_fire`` names the rule the source is
# expected to trip; the negative cases assert a clean scan of the whole file.
_SELFTEST_CASES: tuple[tuple[str, str, bool], ...] = (
    # Rule 1: _Static_assert.
    ("static_assert", '_Static_assert(sizeof(int) == 4, "width");\n', True),
    ("static_assert", '  _Static_assert(1, "x");\n', True),
    ("static_assert", 'static_assert(sizeof(int) == 4, "width");\n', False),
    # Rule 2: legacy single-zero initializers, including typed spellings.
    ("zero_init", "int table[4] = {0};\n", True),
    ("zero_init", "unsigned table[4] = {0U};\n", True),
    ("zero_init", "unsigned long table[4] = { 0UL };\n", True),
    ("zero_init", "unsigned long long table[4]={0ULL};\n", True),
    ("zero_init", "unsigned table[4] = {0x00u};\n", True),
    ("zero_init", "unsigned table[4] = {0b00U};\n", True),
    ("zero_init", "unsigned _BitInt(2) table[1] = {0wb};\n", True),
    ("zero_init", "unsigned _BitInt(2) table[1] = {0uwb};\n", True),
    ("zero_init", "unsigned _BitInt(2) table[1] = {0WBU};\n", True),
    ("zero_init", "unsigned table[4] = {0x00ULL,};\n", True),
    ("zero_init", "unsigned table[4] = {\n  0b00UL,\n};\n", True),
    ("zero_init", "int table[4] = {};\n", False),
    ("zero_init", "int table[4] = { };\n", False),
    ("zero_init", "int table[4] = {0U, 1U};\n", False),
    ("zero_init", "int table[4] = {\n  0U,\n  1U,\n};\n", False),
    ("zero_init", "unsigned table[1] = {0'1U};\n", False),
    ("zero_init", "unsigned table[1] = {0x0'1U};\n", False),
    ("zero_init", "unsigned table[1] = {0b0'1U};\n", False),
    ("zero_init", "long table[2] = {0z, 1z};\n", False),
    # Rule 3: #include <stdbool.h>.
    ("stdbool", "#include <stdbool.h>\n", True),
    ("stdbool", "#  include <stdbool.h>\n", True),
    ("stdbool", "#include <stdint.h>\n", False),
    # Rule 4: bare numeric #define.
    ("bare_define", "#define K_TIMEOUT_MS 1000\n", True),
    ("bare_define", "#define K_MASK 0xFF\n", True),
    ("bare_define", "#define K_FLAGS 0b1010u\n", True),
    ("bare_define", "#define K_SCALE 1.5f\n", True),
    ("bare_define", "#define K_TIMEOUT_MS (1000)\n", False),
    ("bare_define", "#define K_TIMEOUT_MS 1000  // trailing comment ok\n", True),
    ("bare_define", "#define RA8_HAS_MVE\n", False),
    ("bare_define", "#define MAX(a, b) ((a) > (b) ? (a) : (b))\n", False),
    # Comment / string suppression: no rule may fire on non-code text.
    ("_clean", "/* _Static_assert(x); int a[1] = {0U}; #define K 5 */\n", False),
    ("_clean", 'const char* s = "= {0ULL}";\n', False),
    ("_clean", "// #include <stdbool.h>\n", False),
)

# (source, exact opening line, should_fire). These fixtures exercise the
# multiline lexical view independently of the rule-presence table above.
_ZERO_INIT_LINE_CASES: tuple[tuple[str, int, bool], ...] = (
    ('unsigned table[2] = {0U, "x"};\n', 1, False),
    ('unsigned table[2] = {\n  0U,\n  "x",\n};\n', 2, False),
    ("unsigned table[2] = {0U, 'x'};\n", 1, False),
    ("unsigned table[2] = {\n  0U,\n  'x',\n};\n", 2, False),
    ("\nunsigned table[1] = {\n  0U, // only a comment follows\n};\n", 2, True),
    ("\nunsigned table[1] = { /* before */ 0U, /* after */ };\n", 2, True),
    ("\nunsigned table[1] = {\n  0x00ULL,\n};\n", 2, True),
    ('const char *s = "unsigned table[1] = {0U};";\n', 1, False),
    ("/* unsigned table[1] = {0U}; */\n", 1, False),
    ("// unsigned table[1] = {0U};\n", 1, False),
    ('const char *s = R"(before " = {0U} after)";\n', 1, False),
    ('const char *s = u8R"tag(before ")" = {0U} after)tag";\n', 1, False),
    ('const char *s = uR"tag(before = {0U} after)tag";\n', 1, False),
    ('const char *s = UR"tag(before = {0U} after)tag";\n', 1, False),
    ('const char *s = LR"tag(before = {0U} after)tag";\n', 1, False),
    ('const char *s = R"tag(\nbefore " = {0U}\nafter\n)tag";\n', 1, False),
    ('const char *s = R"unterminated(fake = {0U};\n', 1, False),
    ('const char *s = R"abcdefghijklmnopq(fake = {0U})abcdefghijklmnopq";\n', 1, False),
    ('const char *s = R"(fake = {0U})";\nint real[1] = {0U};\n', 2, True),
    ("unsigned table[1] = {\\\n0U};\n", 1, True),
    ("/\\\n/ fake = {0U};\nint real;\n", 1, False),
    ("/\\\n* fake = {0U}; */\nint real;\n", 1, False),
    ("/* fake = {0U}; *\\\n/\nint real;\n", 1, False),
    ('const char *s = R\\\n"(fake = {0U})";\n', 1, False),
    ('const char *s = R"tag(fake )tag\\\n" = {0U})tag";\n', 1, False),
    ("// fake \\\nint hidden[1] = {0U};\n", 1, False),
    ("// fake \\\nstill hidden\nint real[1] = {0U};\n", 3, True),
)

_PHASE2_CASES: tuple[tuple[str, str, tuple[int, ...]], ...] = (
    ("plain", "plain", (0, 1, 2, 3, 4)),
    ("\\", "\\", (0,)),
    ("\\n", "\\n", (0, 1)),
    ("\\\n", "", ()),
    ("a\\\nb", "ab", (0, 3)),
)

_VALID_ZERO_SUFFIXES = (
    "",
    "u",
    "U",
    "l",
    "L",
    "ll",
    "LL",
    "ul",
    "uL",
    "Ul",
    "UL",
    "ull",
    "uLL",
    "Ull",
    "ULL",
    "lu",
    "lU",
    "Lu",
    "LU",
    "llu",
    "llU",
    "LLu",
    "LLU",
    "wb",
    "WB",
    "uwb",
    "Uwb",
    "uWB",
    "UWB",
    "wbu",
    "wbU",
    "WBu",
    "WBU",
    "z",
    "Z",
    "uz",
    "Uz",
    "uZ",
    "UZ",
    "zu",
    "zU",
    "Zu",
    "ZU",
)

_VALID_ZERO_LITERALS = (
    *(f"0{suffix}" for suffix in _VALID_ZERO_SUFFIXES),
    "0'0U",
    "0x0'0ULL",
    "0b0'0uwb",
    "0x0'0z",
)

_LINE_RULE_CASES: tuple[tuple[str, tuple[tuple[int, str], ...]], ...] = (
    ('_Sta\\\ntic_assert(1, "ok");\n', ((1, "static_assert"),)),
    ('\\\n_Static_assert(1, "ok");\n', ((2, "static_assert"),)),
    ("#inc\\\nlude <stdbool.h>\n", ((1, "stdbool"),)),
    ("#define K_TIME \\\n1000\n", ((1, "bare_define"),)),
    ("/\\\n/ _Static_assert(1, x);\n", ()),
    ("/\\\n* #include <stdbool.h> */\n", ()),
    ("/* #define K 1000 *\\\n/\n", ()),
    ('const char*s=R"(before "\n_Static_assert(1, x);\n)";\n', ()),
    ('const char*s=R"(before "\n#include <stdbool.h>\n)";\n', ()),
    ('const char*s=R"(before "\n#define K 1000\n)";\n', ()),
    ('const char*s=R"(one\ntwo)";\n\n_Static_assert(1, "ok");\n', ((4, "static_assert"),)),
    ("char8_t c = u8'0';\n_Static_assert(1, \"ok\");\n", ((2, "static_assert"),)),
)


def _zero_literal_failures(tmp: Path) -> list[str]:
    """Return failures from the complete valid-zero literal table."""
    failures: list[str] = []
    for i, literal in enumerate(_VALID_ZERO_LITERALS):
        fixture = tmp / f"zero_literal_case_{i}.cpp"
        fixture.write_text(f"long table[1] = {{{literal}}};\n", encoding="utf-8")
        fired = [rule_id for _line, rule_id, _snippet in find_violations(fixture)]
        if fired != ["zero_init"]:
            failures.append(f"  zero literal case {i}: got {fired} for {literal!r}")
    return failures


def _line_rule_failures(tmp: Path) -> list[str]:
    """Return failures from phase-2, raw-literal, and line-map cases."""
    failures: list[str] = []
    for i, (source, expected) in enumerate(_LINE_RULE_CASES):
        fixture = tmp / f"line_rule_case_{i}.cpp"
        fixture.write_text(source, encoding="utf-8")
        actual = tuple((line, rule_id) for line, rule_id, _snippet in find_violations(fixture))
        if actual != expected:
            failures.append(
                f"  line-rule case {i}: got {actual!r}, expected {expected!r}: {source!r}"
            )
    return failures


def selftest(tmp: Path) -> int:
    """Prove each rule fires on a bad fixture and stays silent on the C23 form.

    Args:
        tmp: Writable scratch directory for the fixture files.

    Returns:
        0 when every case matches its expectation, 1 otherwise.
    """
    failures: list[str] = []
    for i, (expect_id, source, should_fire) in enumerate(_SELFTEST_CASES):
        fixture = tmp / f"case_{i}.c"
        fixture.write_text(source, encoding="utf-8")
        fired = {rule_id for _line, rule_id, _snip in find_violations(fixture)}
        if should_fire:
            if expect_id not in fired:
                failures.append(f"  case {i}: rule '{expect_id}' did not fire on: {source!r}")
        elif fired:
            failures.append(
                f"  case {i}: rules {sorted(fired)} fired but none expected: {source!r}"
            )
    for i, (source, expected_line, should_fire) in enumerate(_ZERO_INIT_LINE_CASES):
        fixture = tmp / f"zero_line_case_{i}.c"
        fixture.write_text(source, encoding="utf-8")
        lines = [
            line for line, rule_id, _snippet in find_violations(fixture) if rule_id == "zero_init"
        ]
        expected = [expected_line] if should_fire else []
        if lines != expected:
            failures.append(
                f"  zero-line case {i}: got lines {lines}, expected {expected}: {source!r}"
            )
    for i, (source, expected_text, expected_origins) in enumerate(_PHASE2_CASES):
        actual_text, actual_origins = _phase2_source(source)
        if (actual_text, actual_origins) != (expected_text, expected_origins):
            failures.append(
                f"  phase-2 case {i}: got {(actual_text, actual_origins)!r}, "
                f"expected {(expected_text, expected_origins)!r}"
            )
    failures.extend(_zero_literal_failures(tmp))
    failures.extend(_line_rule_failures(tmp))
    if failures:
        print("check_c23_patterns.py --selftest: FAILED\n", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    fires = sum(1 for c in _SELFTEST_CASES if c[2])
    total = (
        len(_SELFTEST_CASES)
        + len(_ZERO_INIT_LINE_CASES)
        + len(_PHASE2_CASES)
        + len(_VALID_ZERO_LITERALS)
        + len(_LINE_RULE_CASES)
    )
    fires += sum(1 for case in _ZERO_INIT_LINE_CASES if case[2])
    fires += len(_VALID_ZERO_LITERALS)
    fires += sum(1 for _source, expected in _LINE_RULE_CASES if expected)
    print(
        f"check_c23_patterns.py --selftest: PASS "
        f"({total} cases: {fires} must fire, {total - fires} must stay silent)"
    )
    return 0


def _resolve_targets(args: argparse.Namespace) -> list[Path]:
    """Decide which files to scan from the parsed arguments.

    Args:
        args: Parsed command-line arguments.

    Returns:
        The list of files to scan, filtered through ``needs_check``.
    """
    if args.all:
        return iter_all_files()
    if args.files:
        return [Path(p) for p in args.files if needs_check(Path(p)) and Path(p).is_file()]
    return iter_staged_files()


def main(argv: list[str]) -> int:
    """Scan first-party C/C++ for the four C23 patterns, or run the selftest.

    With no ``--all`` and no explicit files the staged set is scanned, which is
    how the pre-commit hook stays fast; ``--all`` sweeps every tracked file,
    which is how the CI gate covers the whole tree.

    Args:
        argv: Full argument vector (``sys.argv``).

    Returns:
        0 when clean, 1 on findings or a failing selftest, 2 on a usage error.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--all", action="store_true", help="scan all tracked C/C++ files")
    parser.add_argument(
        "--selftest", action="store_true", help="prove each rule fires and stays silent correctly"
    )
    parser.add_argument("files", nargs="*", help="explicit file list (e.g. staged files)")
    args = parser.parse_args(argv[1:])

    if args.selftest:
        with tempfile.TemporaryDirectory() as td:
            return selftest(Path(td))

    targets = _resolve_targets(args)

    messages = {rule_id: msg for rule_id, _pat, msg in _RULES}
    total = 0
    for path in targets:
        rel = path.relative_to(REPO_ROOT) if path.is_relative_to(REPO_ROOT) else path
        for line_no, rule_id, snippet in find_violations(path):
            print(f"{rel}:{line_no}: {messages[rule_id]}: {snippet}", file=sys.stderr)
            total += 1

    if total:
        print(
            f"\ncheck_c23_patterns.py: {total} C23-pattern violation(s). "
            "Use static_assert, `= {}`, drop <stdbool.h>, and wrap bare "
            "numeric #define values in parens.",
            file=sys.stderr,
        )
        return 1
    print(f"check_c23_patterns.py: {len(targets)} file(s) scanned, 0 findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
