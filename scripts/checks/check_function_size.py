#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: NASA Power-of-10 Rule 4 -- a function fits in one display page (60 lines).

The project's ``.clang-tidy`` configures ``readability-function-size``, but
clang-tidy only sees files present in ``compile_commands.json``.  The host
unit-test build drops every ARM-cross-compiled translation unit, so ~90% of
``port/``, much of ``libs/ra8_hal/``, and every example ``main.c`` were exempt.
This checker walks source text directly so the rule reaches all of it.

Scope is derived, not listed (#359)
-----------------------------------
The scope was a hand-written ``SCAN_ROOTS`` tuple that omitted ``scripts/``,
and the parser understood only C -- so Rule 4 had never applied to a single
Python or shell function.  Scope now comes from :mod:`lint_targets`
(``git ls-files`` + per-file language detection), and the measurement is
delegated to one parser per language:

    :mod:`funcsize_c`   C / C++ -- textual brace tracking
    :mod:`funcsize_py`  Python  -- a real ``ast`` parse
    :mod:`funcsize_sh`  shell   -- textual, both ``{}`` and subshell ``()`` bodies

Is 60 the right cap for Python and shell?
-----------------------------------------
Yes, and the number is deliberately NOT re-derived per language.  Rule 4's
rationale is that a function fits on one screen so a reviewer holds all of it
at once.  That is a claim about *display lines*, and a screen is the same
height whatever the file extension.  Picking a different number per language
would say the rationale is about something else.

The tempting alternative -- lean on ruff's ``PLR0915`` (statement count) and
skip a line cap for Python -- was measured and rejected.  ``PLR0915`` at its
configured 50 reports **zero** findings on this tree, while 41 Python
functions exceed 60 lines.  The two do not measure the same thing: a
multi-line call, dict literal or ``with`` header is one statement spanning
many lines, so a statement cap is far weaker than a line cap and cannot stand
in for it.  ``PLR0915`` stays at 50 as a complementary bound on dense
one-liner-heavy functions; this gate owns the display-page rule.  (``PLR0912``
branches=12 reports zero and is likewise complementary.  ``C90`` mccabe is a
different rule again -- 52 findings, a restructuring campaign rather than a
size cap -- and remains unselected.)

What is counted is normalised so all three languages measure the same span:
the function body, with its documented contract excluded.  In C the Doxygen
block sits above the signature and is already outside the span.  In Python the
docstring is the body's first statement, so it is subtracted -- otherwise this
gate would penalise documentation in exactly the repository that mandates it.
Blank lines and comments are inside the span in C, so they stay inside it
everywhere.

Run::

    check_function_size.py                      # scan the whole tree
    check_function_size.py path/to/file.c ...   # scan listed files
    check_function_size.py --selftest           # prove it fires and stays quiet

Exit 0 if every function is at or below the threshold, exit 1 otherwise.
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import funcsize_c
import funcsize_py
import funcsize_sh
from lint_targets import REPO_ROOT, files_for, language_of

# NASA Power-of-10 Rule 4: ~60 source lines per function. Matches the value in
# ``.clang-tidy`` (readability-function-size.LineThreshold).
THRESHOLD_LINES = 60

# Maximum signature length printed in the diagnostic table before truncation.
SIG_DISPLAY_MAX = 70

# Languages this gate can measure, and who measures them. A language absent
# here is simply not checked -- CMake, YAML, Make and linker scripts have no
# function construct this rule is about.
PARSERS = {
    "c": funcsize_c.scan,
    "python": funcsize_py.scan,
    "shell": funcsize_sh.scan,
}

# Per-app boot boilerplate is copied verbatim into every app and is dominated
# by vector-table initialisers; flagging it would bury real findings.
BOOT_BOILERPLATE = frozenset(
    {"vector_table.c", "system_init.c", "secure_exception.c", "trustzone_init.c"}
)

EXCLUDE_FRAGMENTS = ("_unsupported/",)


def _rel(path: Path) -> str:
    if path.is_absolute() and path.is_relative_to(REPO_ROOT):
        return str(path.relative_to(REPO_ROOT))
    return str(path)


def _skipped(rel: str) -> bool:
    return Path(rel).name in BOOT_BOILERPLATE or any(
        frag in f"/{rel}" for frag in EXCLUDE_FRAGMENTS
    )


def scan_paths(paths: list[Path]) -> list[tuple[str, int, int, str]]:
    """Return (file, start_line, length, signature) for every over-cap function."""
    findings: list[tuple[str, int, int, str]] = []
    for path in paths:
        rel = _rel(path)
        lang = language_of(rel)
        parser = PARSERS.get(lang)
        if parser is None or _skipped(rel):
            continue
        for start, length, signature in parser(path, THRESHOLD_LINES):
            findings.append((rel, start, length, signature))
    findings.sort(key=lambda f: (-f[2], f[0]))
    return findings


def _targets_from_args(args: list[str]) -> list[Path]:
    out: list[Path] = []
    for raw in args:
        path = Path(raw)
        if not path.is_absolute():
            path = REPO_ROOT / path
        if path.is_dir():
            out.extend(p for p in path.rglob("*") if p.is_file())
        else:
            out.append(path)
    return out


def _all_targets() -> list[Path]:
    grouped = files_for(tuple(PARSERS))
    return [REPO_ROOT / rel for paths in grouped.values() for rel in paths]


# ---------------------------------------------------------------------------
# Selftest -- asserts BOTH directions, per language, before the real scan.
# ---------------------------------------------------------------------------

_LONG = THRESHOLD_LINES + 5

_C_OVER = "void f(void)\n{\n" + "  x();\n" * _LONG + "}\n"
_C_UNDER = "void f(void)\n{\n" + "  x();\n" * 5 + "}\n"
_C_STRING_BRACE = 'void f(void)\n{\n  const char* s = "{";\n' + "  x();\n" * 5 + "}\n"
# An empty one-line body inside a long enclosing scope. Assuming the opening
# line contributes depth 1 made the tracker swallow the whole class.
_CXX_EMPTY_BODY = (
    "class C {\npublic:\n  C(int a)\n      : a_(a)\n  {}\n" + "  void g();\n" * _LONG + "};\n"
)

_PY_OVER = "def f():\n" + "    x = 1\n" * _LONG
_PY_UNDER = "def f():\n" + "    x = 1\n" * 5
_PY_DOCSTRING = 'def f():\n    """\n' + "    doc\n" * _LONG + '    """\n' + "    x = 1\n" * 5
_PY_NESTED = "def outer():\n    def inner():\n" + "        x = 1\n" * _LONG + "    return inner\n"

_SH_OVER = "f() {\n" + "  x\n" * _LONG + "}\n"
_SH_UNDER = "f() {\n" + "  x\n" * 5 + "}\n"
_SH_SUBSHELL_OVER = "f() (\n  set -e\n" + "  x\n" * _LONG + ")\n"
_SH_SUBSHELL_UNDER = "f() (\n  set -e\n" + "  x\n" * 5 + ")\n"
# A short function whose heredoc is full of stray closing braces. If the
# heredoc body were counted, depth would hit zero early and the function
# would measure a handful of lines -- or run away past its real end.
_SH_HEREDOC = "f() {\n  cat <<EOF\n" + "  }\n" * 5 + "EOF\n" + "  x\n" * 5 + "}\n"
_SH_HEREDOC_LONG = "f() {\n  cat <<EOF\n" + "  }\n" * _LONG + "EOF\n" + "  x\n" * 5 + "}\n"

# (filename, body, expected finding count, what the case proves)
_CASES: tuple[tuple[str, str, int, str], ...] = (
    ("over.c", _C_OVER, 1, "C: a long function fires"),
    ("under.c", _C_UNDER, 0, "C: a short function is clean"),
    ("brace.c", _C_STRING_BRACE, 0, "C: a brace in a string literal is not a scope"),
    ("empty.cpp", _CXX_EMPTY_BODY, 0, "C++: a one-line {} body does not swallow its class"),
    ("over.py", _PY_OVER, 1, "Python: a long function fires"),
    ("under.py", _PY_UNDER, 0, "Python: a short function is clean"),
    ("doc.py", _PY_DOCSTRING, 0, "Python: a long docstring does not make a function long"),
    ("nested.py", _PY_NESTED, 2, "Python: a nested function and its parent both count"),
    ("over.sh", _SH_OVER, 1, "shell: a long brace-body function fires"),
    ("under.sh", _SH_UNDER, 0, "shell: a short brace-body function is clean"),
    ("sub_over.sh", _SH_SUBSHELL_OVER, 1, "shell: a long subshell body fires"),
    ("sub_under.sh", _SH_SUBSHELL_UNDER, 0, "shell: a short subshell body is clean"),
    ("heredoc.sh", _SH_HEREDOC, 0, "shell: braces inside a heredoc close no scope"),
    (
        "heredoc_long.sh",
        _SH_HEREDOC_LONG,
        1,
        "shell: a long function is still caught through a heredoc",
    ),
    ("over.md", _PY_OVER, 0, "a non-code file is out of scope"),
)


def _selftest() -> int:
    """Assert every parser fires and stays quiet, and that the scope is live.

    Three textual parsers drive this gate. A parser that stops recognising a
    construct takes that construct's offenders with it and the gate reports a
    clean tree -- so each language is asserted in both directions here, and
    the gate body runs this before the real scan.
    """
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for name, body, expected, description in _CASES:
            path = root / name
            path.write_text(body)
            got = len(scan_paths([path]))
            path.unlink()
            if got != expected:
                failures.append(f"  FAIL {description}: expected {expected}, got {got}")

    grouped = files_for(tuple(PARSERS))
    for lang, paths in grouped.items():
        if not paths:
            failures.append(f"  FAIL live scope: language {lang!r} resolved to zero files")

    if failures:
        print("check_function_size.py: --selftest FAILED", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(
        f"check_function_size.py: --selftest OK ({len(_CASES)} cases across "
        f"{len(PARSERS)} parser(s), both directions; live scope "
        f"{sum(len(p) for p in grouped.values())} file(s))"
    )
    return 0


def _report(findings: list[tuple[str, int, int, str]]) -> None:
    print(
        f"check_function_size.py: {len(findings)} function(s) exceed the "
        f"{THRESHOLD_LINES}-line cap (NASA P10 Rule 4):\n",
        file=sys.stderr,
    )
    print("  lines  location", file=sys.stderr)
    for rel, start, length, signature in findings:
        shown = signature
        if len(shown) > SIG_DISPLAY_MAX:
            shown = shown[: SIG_DISPLAY_MAX - 3] + "..."
        print(f"  {length:5d}  {rel}:{start}  {shown}", file=sys.stderr)
    print(
        "\nExtract a helper, or split the function along its responsibilities.",
        file=sys.stderr,
    )


def main(argv: list[str]) -> int:
    args = argv[1:]
    if "--selftest" in args:
        return _selftest()

    targets = _targets_from_args(args) if args else _all_targets()
    if not targets:
        print("check_function_size.py: FATAL -- no files to scan", file=sys.stderr)
        return 2

    findings = scan_paths(targets)
    if not findings:
        print(
            f"check_function_size.py: {len(targets)} file(s) scanned, "
            f"no functions over {THRESHOLD_LINES} lines."
        )
        return 0
    _report(findings)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
