#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""One definition of "which files are first-party code", for the size gates.

Four checkers in this tree have now had the same defect: a hand-written
``SCAN_ROOTS`` / ``SOURCE_SUFFIXES`` tuple that quietly stopped describing the
repository.  ``check_file_size.py`` and ``check_function_size.py`` were the
worst of them -- their roots omitted ``scripts/`` and their suffixes covered
only C/C++, so the documented 1000-line file cap and the 60-line NASA Rule 4
function cap had never once applied to a Python or shell file (#359).

The failure mode is specific and worth naming: a hardcoded list does not fail
when it goes stale.  It reports success over a shrinking slice of the tree, and
the gate looks green precisely because it stopped looking.  So the enumeration
is derived instead:

* the file set comes from ``git ls-files`` -- whatever is in the repository is
  in scope, and a new top-level directory is covered the day it is added;
* language is decided per file by suffix, by well-known basename, or by
  shebang, so an extensionless executable cannot escape by having no suffix;
* the only subtractions are vendored SOUP and generated tables, which
  CLAUDE.md already exempts by name.

``check_lint_coverage.py`` asks a parallel question ("is every code file
claimed by some linter?") and this module answers the size gates' half of it
with the same enumeration, so the two cannot disagree about what code is.

Run::

    lint_targets.py                # every first-party code file
    lint_targets.py c python       # only the named languages
    lint_targets.py --list         # the language names this module knows

Prints one repo-relative path per line, sorted.  Exits non-zero, printing
nothing, when a requested language resolves to zero files: a gate must never
mistake a broken enumeration for a clean tree.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Vendored SOUP and generated tables. Matches the CLAUDE.md exemption list and
# the sibling gates' EXCLUDE_FRAGMENTS.
EXCLUDED_PREFIXES = (
    "libs/third_party/",
    "libs/fonts/",
    "tools/vela/generated/",
)

# Prefixes excluded for SOME languages only. A vendored tree is SOUP for the
# language whose sources it carries, but the build glue that compiles it is
# ours and is linted like any other first-party listfile: port/threadx/ holds
# vendored ThreadX C, and a CMakeLists.txt we wrote and hold to the cmake gate.
# Excluding the directory wholesale -- which this module originally did -- would
# have silently dropped that listfile out of the cmake scope.
LANGUAGE_EXCLUDED_PREFIXES = {
    "c": ("port/threadx/",),
}

# Path fragments that mark build output rather than source.
EXCLUDED_FRAGMENTS = ("/build/", "/build-cov/", "/_deps/", "node_modules/")

# suffix -> language
SUFFIX_LANG = {
    ".c": "c",
    ".h": "c",
    ".cpp": "c",
    ".hpp": "c",
    ".cc": "c",
    ".cxx": "c",
    ".hh": "c",
    ".hxx": "c",
    ".py": "python",
    ".sh": "shell",
    ".bash": "shell",
    ".cmake": "cmake",
    ".yml": "yaml",
    ".yaml": "yaml",
    ".mk": "make",
    ".ld": "ld",
}

# Exact basenames that carry no suffix but are unambiguously one language.
BASENAME_LANG = {
    "CMakeLists.txt": "cmake",
    "Makefile": "make",
    "GNUmakefile": "make",
}

# Directories whose extensionless executables are shell by construction. The
# git hooks are the case that matters: scripts/git/pre-commit is 670 lines of
# shell that no suffix-driven scope has ever seen.
SHEBANG_LANG = {
    "sh": "shell",
    "bash": "shell",
    "zsh": "shell",
    "dash": "shell",
    "python": "python",
    "python3": "python",
}

LANGUAGES = ("c", "python", "shell", "cmake", "yaml", "make", "ld")


def _tracked() -> list[str]:
    """Tracked plus untracked-but-not-ignored paths, from git itself."""
    proc = subprocess.run(
        [  # noqa: S607 -- trusted: fixed git argv
            "git",
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write("lint_targets.py: FATAL -- `git ls-files` failed\n")
        sys.exit(2)
    return [rel for rel in proc.stdout.split("\0") if rel]


def _excluded(rel: str, lang: str | None = None) -> bool:
    if rel.startswith(EXCLUDED_PREFIXES) or any(frag in f"/{rel}" for frag in EXCLUDED_FRAGMENTS):
        return True
    extra = LANGUAGE_EXCLUDED_PREFIXES.get(lang or "", ())
    return bool(extra) and rel.startswith(extra)


def _shebang_lang(path: Path) -> str | None:
    """Language named by a ``#!`` first line, or None.

    This is the half of the enumeration a suffix list cannot do. An executable
    with no extension is still code, and the git hooks are exactly that.
    """
    try:
        with path.open("rb") as handle:
            first = handle.readline(200).decode("utf-8", errors="replace")
    except OSError:
        return None
    if not first.startswith("#!"):
        return None
    words = first[2:].replace("/usr/bin/env", " ").replace("/", " ").split()
    for word in words:
        base = word.split("-")[0]
        if base in SHEBANG_LANG:
            return SHEBANG_LANG[base]
    return None


def _raw_language(rel: str, root: Path) -> str | None:
    """The language a path's name implies, before any exclusion is applied."""
    path = Path(rel)
    if path.name in BASENAME_LANG:
        return BASENAME_LANG[path.name]
    lang = SUFFIX_LANG.get(path.suffix)
    if lang is not None:
        return lang
    if path.suffix:
        return None  # a suffix we know is not code (.md, .json, .pdf, ...)
    return _shebang_lang(root / rel)


def language_of(rel: str, root: Path = REPO_ROOT) -> str | None:
    """The language of one repo-relative path, or None if it is not code.

    The language is resolved BEFORE exclusion, because exclusion is now
    per-language: a vendored tree can be SOUP for its sources and still hold
    first-party build glue.
    """
    if _excluded(rel):
        return None
    lang = _raw_language(rel, root)
    if lang is None or _excluded(rel, lang):
        return None
    return lang


def files_for(languages: tuple[str, ...] = LANGUAGES) -> dict[str, list[str]]:
    """Map each requested language to its sorted first-party file list."""
    out: dict[str, list[str]] = {lang: [] for lang in languages}
    for rel in _tracked():
        lang = language_of(rel)
        if lang in out:
            out[lang].append(rel)
    return {lang: sorted(paths) for lang, paths in out.items()}


def main(argv: list[str]) -> int:
    args = argv[1:]
    if "--list" in args:
        print("\n".join(LANGUAGES))
        return 0
    requested = tuple(a for a in args if not a.startswith("-")) or LANGUAGES
    unknown = [lang for lang in requested if lang not in LANGUAGES]
    if unknown:
        sys.stderr.write(f"lint_targets.py: unknown language(s): {unknown}\n")
        return 2
    grouped = files_for(requested)
    empty = [lang for lang, paths in grouped.items() if not paths]
    if empty:
        sys.stderr.write(
            f"lint_targets.py: FATAL -- language(s) {empty} resolved to zero "
            f"files. The enumeration is broken; refusing to report a clean scope.\n"
        )
        return 2
    for lang in requested:
        for rel in grouped[lang]:
            print(rel)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
