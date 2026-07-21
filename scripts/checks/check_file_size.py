#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: source files shall not exceed a maintainability line cap (1000).

The per-function NASA Power-of-10 Rule 4 cap (``check_function_size.py``,
60 lines/function) keeps individual routines small but says nothing about the
size of the *file* they live in: a translation unit can stay Rule-4-clean while
growing into a multi-thousand-line god-file, or the same body can be
copy-pasted across siblings until each is enormous.

Scope is derived, not listed (#359)
-----------------------------------
This gate used to carry a hand-written ``SCAN_ROOTS`` tuple that omitted
``scripts/`` and a ``SOURCE_SUFFIXES`` tuple covering only C and C++.  The
documented 1000-line cap had therefore never applied to a single Python or
shell file in the repository's history -- and it did not *fail* to apply, it
reported success over a shrinking slice of the tree.  A stale hardcoded list
looks exactly like a clean one.

The scope now comes from :mod:`lint_targets`, which derives it from
``git ls-files`` plus per-file language detection (suffix, well-known basename,
or ``#!`` shebang).  A new top-level directory is covered the day it is added,
and an extensionless executable -- the 670-line ``scripts/git/pre-commit``, for
instance -- cannot escape by having no suffix.

Every language ``lint_targets`` calls code is in scope: C/C++, Python, shell,
CMake, YAML, Make and linker scripts.  Markdown is deliberately NOT: prose is
not code, a long reference document is not a maintainability defect, and the
three docs over the cap (ROADMAP, HARDWARE_BRINGUP, JOF) are reference material
that would be worse split up.

Waivers
-------
A file may waive the cap with an in-file marker in its head:
  @generated    -- machine-emitted; the generator owns its length, not us.
  FILE-SIZE-OK  -- explicit, justified waiver for a hand-authored file.

Run::

    check_file_size.py                      # scan the whole tree
    check_file_size.py path/to/file.c ...   # scan listed files
    check_file_size.py --selftest           # prove it fires and stays quiet

Exit 0 if every file is at or below the cap, exit 1 (with a table) otherwise.
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lint_targets import REPO_ROOT, files_for, language_of

# Maintainability cap: a single source file should stay reviewable.
THRESHOLD_LINES = 1000

# In-file waiver markers; only the head is scanned so a stray occurrence deep
# in data cannot silently waive a real offender.
GENERATED_MARKERS = ("@generated", "FILE-SIZE-OK")
HEAD_SCAN_LINES = 40


def _is_waived(path: Path) -> bool:
    """Return True if `path`'s head carries a generated / size-OK marker."""
    try:
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            for _ in range(HEAD_SCAN_LINES):
                line = handle.readline()
                if not line:
                    break
                if any(marker in line for marker in GENERATED_MARKERS):
                    return True
    except OSError:
        return False
    return False


def _count_lines(path: Path) -> int:
    """Return the physical line count of `path`, or -1 if unreadable."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return -1
    return len(text.splitlines())


def _targets_from_args(args: list[str]) -> list[Path]:
    """Resolve explicitly named files / directories to a scan list."""
    out: list[Path] = []
    for raw in args:
        path = Path(raw)
        if not path.is_absolute():
            path = REPO_ROOT / path
        if path.is_dir():
            out.extend(
                p for p in path.rglob("*") if p.is_file() and language_of(_rel(p)) is not None
            )
        elif language_of(_rel(path)) is not None:
            out.append(path)
    return out


def _rel(path: Path) -> str:
    if path.is_absolute() and path.is_relative_to(REPO_ROOT):
        return str(path.relative_to(REPO_ROOT))
    return str(path)


def _all_targets() -> list[Path]:
    """Every first-party code file, derived from git + language detection."""
    grouped = files_for()
    return [REPO_ROOT / rel for paths in grouped.values() for rel in paths]


def over_cap(targets: list[Path]) -> list[tuple[int, str]]:
    """The (line count, path) of every unwaived file above the cap."""
    over = []
    for path in targets:
        # Language is re-checked here rather than trusted from the caller, so
        # the rule is the same however the file arrived -- derived scan,
        # explicit argument, or selftest fixture.
        if language_of(_rel(path)) is None or _is_waived(path):
            continue
        count = _count_lines(path)
        if count > THRESHOLD_LINES:
            over.append((count, _rel(path)))
    over.sort(reverse=True)
    return over


# ---------------------------------------------------------------------------
# Selftest -- asserts BOTH directions before the real scan is trusted.
# ---------------------------------------------------------------------------


# (filename, body, expected finding count, what the case proves)
_OVER = THRESHOLD_LINES + 1

_SELFTEST_CASES = (
    ("over.c", "x\n" * _OVER, 1, "a C file over the cap fires"),
    ("under.c", "x\n" * THRESHOLD_LINES, 0, "a C file exactly at the cap is clean"),
    ("over.py", "x\n" * _OVER, 1, "a Python file over the cap fires"),
    ("over.sh", "x\n" * _OVER, 1, "a shell file over the cap fires"),
    ("over.cmake", "x\n" * _OVER, 1, "a CMake file over the cap fires"),
    ("over.md", "x\n" * _OVER, 0, "Markdown is prose and out of scope"),
    ("over.json", "x\n" * _OVER, 0, "a data file is out of scope"),
    (
        "gen.py",
        "# @generated by a tool\n" + "x\n" * _OVER,
        0,
        "the @generated marker waives",
    ),
    (
        "waived.py",
        "# FILE-SIZE-OK: justified\n" + "x\n" * _OVER,
        0,
        "the FILE-SIZE-OK marker waives",
    ),
    (
        "deep.py",
        "# marker far below the head\n" * HEAD_SCAN_LINES + "@generated\n" + "x\n" * _OVER,
        1,
        "a marker below the head does not waive",
    ),
)


def _selftest_cap(cases: tuple) -> list[str]:
    """Run the threshold / waiver / scope fixtures and return any failures."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for name, body, expected, description in cases:
            path = root / name
            path.write_text(body)
            got = len(over_cap([path]))
            path.unlink()
            if got != expected:
                failures.append(f"  FAIL {description}: expected {expected}, got {got}")

        # A shebang script with no suffix is code. This is the case a
        # suffix-only scope silently drops -- and scripts/git/pre-commit is
        # 677 lines of exactly it.
        hook = root / "pre-commit-like"
        hook.write_text("#!/usr/bin/env bash\n" + "x\n" * (THRESHOLD_LINES + 1))
        if len(over_cap([hook])) != 1:
            failures.append("  FAIL an extensionless shebang script is in scope")
    return failures


def _selftest_scope() -> tuple[list[str], dict[str, list[str]]]:
    """Assert the live scope still resolves files for every known language.

    Zero files for a language is always a broken enumeration, never a clean
    tree -- the failure mode this checker was rewritten to end.
    """
    grouped = files_for()
    failures = [
        f"  FAIL live scope: language {lang!r} resolved to zero files"
        for lang, paths in grouped.items()
        if not paths
    ]
    return failures, grouped


def _selftest() -> int:
    """Prove the cap fires, the waivers work, and the scope reaches every language."""
    failures = _selftest_cap(_SELFTEST_CASES)
    scope_failures, grouped = _selftest_scope()
    failures.extend(scope_failures)

    if failures:
        print("check_file_size.py: --selftest FAILED", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(
        f"check_file_size.py: --selftest OK ({len(_SELFTEST_CASES) + 1} cases both "
        f"directions; live scope covers {len(grouped)} language(s), "
        f"{sum(len(p) for p in grouped.values())} file(s))"
    )
    return 0


def main(argv: list[str]) -> int:
    args = argv[1:]
    if "--selftest" in args:
        return _selftest()

    targets = _targets_from_args(args) if args else _all_targets()
    if not targets:
        print("check_file_size.py: FATAL -- no files to scan", file=sys.stderr)
        return 2

    over = over_cap(targets)
    if not over:
        print(
            f"check_file_size.py: {len(targets)} file(s) scanned, "
            f"none over {THRESHOLD_LINES} lines."
        )
        return 0

    print(
        f"check_file_size.py: {len(over)} file(s) exceed the {THRESHOLD_LINES}-line cap:\n",
        file=sys.stderr,
    )
    print("  lines  file", file=sys.stderr)
    for count, path in over:
        print(f"  {count:5d}  {path}", file=sys.stderr)
    print(
        "\nSplit the file along its responsibilities, or extract a shared "
        "helper if the bulk is duplicated.  Generated files may carry a "
        "@generated marker; a justified hand-authored file may use FILE-SIZE-OK.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
