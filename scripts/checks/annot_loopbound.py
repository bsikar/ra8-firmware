# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Per-loop bound markers, paired to the loop they describe.

`RA8_LOOP_BOUND` / `RA8_LOOP_BOUND_RUNTIME` (in ``ra8_attributes.h``) bind a
NASA Power-of-10 Rule 2 bound to ONE specific loop. Unlike the ``RA8_*``
annotation macros they are not ``[[clang::annotate]]`` attributes -- they lower
to a ``static_assert`` and a symbol reference, real C valid in statement
position under every toolchain. The compiler already enforces that the ceiling
is a positive compile-time constant (or that the runtime ceiling symbol
exists); this module enforces the other half: that a marker is actually
attached to a loop, and that nobody reintroduces the broken predecessor.

Why textual, not libclang
-------------------------
The rest of ``check_annotations.py`` reasons over the AST, but these markers
are gone by the time libclang sees the code: ``RA8_LOOP_BOUND(k_foo)`` has
already expanded to ``static_assert(...)``, and the ``RA8_LOOP_BOUND`` token is
nowhere in the tree. The property under check -- "this marker sits on the line
above a ``for`` / ``while`` / ``do``" -- is a source-text adjacency, so it is
checked on source text. Comments, strings and preprocessor ``#define`` lines
are stripped first so a marker named inside a Doxygen example or the macro's
own definition is not mistaken for a use.

The two failure directions
---------------------------
This is the exact defect class the marker exists to end -- a bound annotation
that binds to nothing -- so both directions are fatal, and the selftest
(:func:`run_loopbound_selftest`) asserts each:

* **mis-attached** -- an ``RA8_LOOP_BOUND`` / ``RA8_LOOP_BOUND_RUNTIME`` marker
  whose next code line is not a loop. The bound describes no loop.
* **stale statement form** -- a legacy ``RA8_BOUNDED_LOOP(x);`` in statement
  position (immediately above a loop). That annotation was a hard clang error
  and a silent GCC no-op that bound to no loop at all; it is the very thing
  #382 replaced, and it must never come back. (The function-level
  ``RA8_BOUNDED_LOOP`` annotation -- immediately above a *declaration* -- is
  legitimate and is left alone: its next code line is a function signature, not
  a loop.)
"""

from __future__ import annotations

import pathlib
import re
from collections.abc import Callable

from annot_model import Violation
from annot_scope import SCAN_DIRS, is_excluded, repo_root

#: Suffixes scanned for loop-bound markers. Headers are included because an
#: inline function in a header can carry a bounded loop too.
_LOOPBOUND_SUFFIXES = frozenset({".c", ".cpp", ".h", ".hpp"})

#: The per-loop marker that asserts a compile-time-constant ceiling.
_RE_STATIC = re.compile(r"\bRA8_LOOP_BOUND\s*\(")

#: The per-loop marker for a runtime / linker-symbol ceiling.
_RE_RUNTIME = re.compile(r"\bRA8_LOOP_BOUND_RUNTIME\s*\(")

#: The legacy function-level annotation. Banned in statement position.
_RE_LEGACY = re.compile(r"\bRA8_BOUNDED_LOOP\s*\(")

#: A source line that begins a loop, once leading whitespace is removed.
_RE_LOOP_START = re.compile(r"^(for|while|do)\b")

#: The one rule key this module reports under.
_RULE = "ra8_loop_bound"

#: One C token at a time, longest-match-first: a block comment, a line
#: comment, a string literal, a char literal, or any single other character.
#: Every alternative but the last is multi-character, so a one-character match
#: is by construction ordinary code and everything longer is a comment or
#: literal to blank out. ``re.DOTALL`` lets the block-comment and the final
#: ``.`` span newlines, so the reconstruction stays line-for-line with the input.
_TOKEN_RE = re.compile(
    r"/\*.*?\*/"  # block comment
    r"|//[^\n]*"  # line comment
    r'|"(?:\\.|[^"\\\n])*"'  # string literal
    r"|'(?:\\.|[^'\\\n])*'"  # char literal
    r"|.",  # any other single character
    re.DOTALL,
)


def _blanked(token: str) -> str:
    """Replace ``token`` with spaces, but keep its newlines (line alignment)."""
    return "".join("\n" if ch == "\n" else " " for ch in token)


def strip_code(text: str) -> list[str]:
    """Return ``text``'s lines with comments and string/char literals blanked.

    A marker or loop keyword that appears only inside a comment or a string is
    thereby not seen as code -- which is what keeps the macro's own definition,
    Doxygen examples and message strings from reading as uses. Newlines are
    preserved, so the returned list is 1:1 with the source lines.
    """
    rebuilt = "".join(
        m.group(0) if len(m.group(0)) == 1 else _blanked(m.group(0))
        for m in _TOKEN_RE.finditer(text)
    )
    return rebuilt.split("\n")


def _next_code_index(code: list[str], start: int) -> int | None:
    """Return the index of the first non-blank code line after ``start``."""
    for j in range(start + 1, len(code)):
        if code[j].strip():
            return j
    return None


def _is_loop(code_line: str) -> bool:
    """True when ``code_line`` (comments stripped) begins a loop statement."""
    return bool(_RE_LOOP_START.match(code_line.strip()))


def scan_source(path: str, text: str) -> list[Violation]:
    """Return every loop-bound violation in one file's source ``text``.

    Pure: no filesystem, no libclang, so the selftest drives it directly.
    """
    code = strip_code(text)
    out: list[Violation] = []
    for idx, code_line in enumerate(code):
        stripped = code_line.strip()
        # Preprocessor lines (the macro definitions themselves) are not uses.
        if stripped.startswith("#"):
            continue
        line_no = idx + 1
        nxt = _next_code_index(code, idx)
        next_is_loop = nxt is not None and _is_loop(code[nxt])

        if _RE_RUNTIME.search(code_line) or _RE_STATIC.search(code_line):
            if not next_is_loop:
                out.append(
                    Violation(
                        _RULE,
                        path,
                        line_no,
                        "RA8_LOOP_BOUND marker is not immediately followed by a "
                        "for/while/do loop -- the bound binds to nothing; place it "
                        "directly above the loop it describes",
                    )
                )
        elif _RE_LEGACY.search(code_line) and next_is_loop:
            out.append(
                Violation(
                    _RULE,
                    path,
                    line_no,
                    "RA8_BOUNDED_LOOP used in statement position (immediately above a "
                    "loop) -- that annotation binds to no loop (a clang error and a "
                    "silent GCC no-op). Use RA8_LOOP_BOUND / RA8_LOOP_BOUND_RUNTIME",
                )
            )
    return out


def discover_loopbound_files() -> list[pathlib.Path]:
    """Return every first-party .c/.cpp/.h/.hpp under SCAN_DIRS, minus SOUP/build."""
    out: list[pathlib.Path] = []
    for top in SCAN_DIRS:
        root = repo_root() / top
        if not root.is_dir():
            continue
        out.extend(
            p for p in root.rglob("*") if p.suffix in _LOOPBOUND_SUFFIXES and not is_excluded(p)
        )
    return sorted(out)


def enforce_loop_bounds(files: list[pathlib.Path], *, require_nonempty: bool) -> list[Violation]:
    """Scan ``files`` for loop-bound marker discipline.

    ``require_nonempty`` guards the whole-tree gate against a scan that silently
    reads nothing: an empty file list there means the discovery glob came apart,
    which would report a clean tree having looked at zero files -- the exact
    do-nothing-gate failure this checker exists to prevent.
    """
    scanned = 0
    out: list[Violation] = []
    for path in files:
        if path.suffix not in _LOOPBOUND_SUFFIXES:
            continue
        try:
            text = path.read_text(errors="ignore")
        except OSError:
            continue
        scanned += 1
        out.extend(scan_source(str(path), text))
    if require_nonempty and scanned == 0:
        out.append(
            Violation(
                _RULE,
                str(repo_root()),
                0,
                "loop-bound scan found no source files -- the discovery glob is "
                "broken; the check would report a clean tree having read nothing",
            )
        )
    return out


def _sf_names(violations: list[Violation]) -> set[int]:
    """Return the set of violation line numbers, for selftest assertions."""
    return {v.line for v in violations if v.rule == _RULE}


def _sf_new_marker(fires: Callable[[str, str], bool]) -> list[str]:
    """Direction 1: a NEW marker attached to a loop is clean; detached, it fires."""
    failures: list[str] = []
    good_static = (
        "void f(void) {\n  RA8_LOOP_BOUND(k_cap);\n  for (int i = 0; i < 4; i++) { g(); }\n}\n"
    )
    if fires("good_static.c", good_static):
        failures.append(
            "loop-bound false positive: RA8_LOOP_BOUND directly above a for-loop "
            "was reported as mis-attached"
        )
    good_runtime = (
        "void f(void) {\n"
        "  RA8_LOOP_BOUND_RUNTIME(g_end);\n"
        "  while (p < &g_end) { *p = 0; p++; }\n"
        "}\n"
    )
    if fires("good_runtime.c", good_runtime):
        failures.append(
            "loop-bound false positive: RA8_LOOP_BOUND_RUNTIME directly above a "
            "while-loop was reported as mis-attached"
        )
    bad_detached = (
        "void f(void) {\n"
        "  RA8_LOOP_BOUND(k_cap);\n"
        "  x = 5;\n"
        "  for (int i = 0; i < 4; i++) { g(); }\n"
        "}\n"
    )
    if not fires("bad_detached.c", bad_detached):
        failures.append(
            "loop-bound went toothless: RA8_LOOP_BOUND with a statement between it "
            "and the loop (mis-attached) was NOT reported"
        )
    bad_no_loop = "void f(void) {\n  RA8_LOOP_BOUND(k_cap);\n  return;\n}\n"
    if not fires("bad_no_loop.c", bad_no_loop):
        failures.append(
            "loop-bound went toothless: RA8_LOOP_BOUND with no following loop "
            "(a marker present with no loop) was NOT reported"
        )
    return failures


def _sf_legacy(fires: Callable[[str, str], bool]) -> list[str]:
    """Direction 2: legacy RA8_BOUNDED_LOOP is clean on a decl, fires on a loop."""
    failures: list[str] = []
    legacy_decl = (
        "RA8_BOUNDED_LOOP(k_polls)\n"
        "static int worker(int n)\n"
        "{\n"
        "  for (int i = 0; i < n; i++) { g(); }\n"
        "  return 0;\n"
        "}\n"
    )
    if fires("legacy_decl.c", legacy_decl):
        failures.append(
            "loop-bound false positive: function-level RA8_BOUNDED_LOOP above a "
            "declaration is the legitimate use and must not be reported"
        )
    legacy_stmt = (
        "void f(void) {\n  RA8_BOUNDED_LOOP(k_cap);\n  for (int i = 0; i < 4; i++) { g(); }\n}\n"
    )
    if not fires("legacy_stmt.c", legacy_stmt):
        failures.append(
            "loop-bound went toothless: legacy RA8_BOUNDED_LOOP in statement "
            "position above a loop (a loop lacking a real bound) was NOT reported"
        )
    return failures


def _sf_non_uses(fires: Callable[[str, str], bool]) -> list[str]:
    """A marker named only in a comment, string, or #define is not a use."""
    failures: list[str] = []
    non_uses = (
        '#define RA8_LOOP_BOUND(c) static_assert((c) > 0, "x")\n'
        "/* RA8_LOOP_BOUND(k_cap); shown in a comment */\n"
        'const char* s = "RA8_LOOP_BOUND(k_cap);";\n'
        "void f(void) { return; }\n"
    )
    if fires("non_uses.c", non_uses):
        failures.append(
            "loop-bound false positive: a marker inside a #define / comment / "
            "string literal was treated as a real use"
        )
    return failures


def run_loopbound_selftest() -> list[str]:
    """Assert both failure directions and both clean shapes. Returns failures.

    Called from ``annot_selftest.run_selftest`` so the one ``--selftest`` proves
    this check the same way it proves the AST rules: the rule fires on the
    broken fixture AND stays quiet on the correct one, because a rule can be
    "fixed" by defanging it and no single-direction test can tell the difference.

    Split by fixture family (:func:`_sf_new_marker`, :func:`_sf_legacy`,
    :func:`_sf_non_uses`) so each stays within the 60-line NASA P10 Rule 4 cap.
    """

    def fires(name: str, src: str) -> bool:
        return bool(_sf_names(scan_source(name, src)))

    return _sf_new_marker(fires) + _sf_legacy(fires) + _sf_non_uses(fires)
