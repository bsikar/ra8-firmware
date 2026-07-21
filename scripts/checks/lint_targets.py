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

# ---------------------------------------------------------------------------
# BUILD OUTPUT -- the single definition, shared by every checker in this tree.
#
# This used to be thirteen copies of the substring ``"/build/"``, one per
# checker, and the substring is the defect (#377). ``"/build/" in path`` cannot
# tell ``tools/board_sim/build/`` -- genuine CMake output -- from a first-party
# source directory that happens to be called ``build``. When #359's
# reorganisation created ``scripts/build/``, every file in it  # PATHREF-OK: #359
# became invisible to shellcheck, shfmt and the rest, while every gate still
# reported
# a clean tree. The bare ``build/`` line in .gitignore did the same thing to
# git, so a NEW file there would never have been added at all; the six that
# survived did so only because ``git mv`` moves already-tracked files.
#
# The replacement is a repo-relative PATH check rather than a substring match.
# A build directory counts as build output only where a build tree is actually
# produced: at the repo root, or under one of the roots below. Anywhere else,
# a directory named ``build`` is ordinary source and is linted like any other.
#
# .gitignore carries the matching anchored patterns, and
# ``check_gitignore_scope.py`` fails on any new unanchored directory pattern,
# so the two halves cannot drift back apart.
# ---------------------------------------------------------------------------

# Top-level directories beneath which a per-target build tree legitimately
# appears, at any depth. Deliberately NOT "any directory anywhere": that is the
# behaviour being removed. `scripts/`, `libs/`, `src/inc/` and friends are
# absent because nothing builds into them, so a `build` directory appearing
# there is source and must stay visible to the checkers.
BUILD_TREE_ROOTS = frozenset(
    {
        "docs",  # docs/build/ -- generated Doxygen HTML
        "esp32",  # esp32/build/ -- esp32/Makefile writes here
        "examples",  # examples/**/<app>/build/ -- per-app CMake output
        "local-poc",  # local-poc/**/build/ -- git-excluded PoC tree
        "port",  # port/**/build/
        "src",  # src/app/build/
        "tests",  # tests/build/, tests/build-cov/, tests/build-fuzz/
        "tools",  # tools/<tool>/build/ -- host tool output
    }
)

# Directory names owned by a tool, which can never be a first-party source
# directory and are therefore matched at ANY depth. This is the ONLY
# depth-agnostic rule left, and every name in it is reserved by the tool that
# creates it: CMake writes CMakeFiles/ and _deps/, CPython writes __pycache__/,
# npm writes node_modules/. Nobody can legitimately author a source directory
# with one of these names, so matching them anywhere cannot swallow source.
TOOL_OUTPUT_DIR_NAMES = frozenset({"CMakeFiles", "_deps", "__pycache__", "node_modules"})


def is_build_dir_name(name: str) -> bool:
    """True when one path COMPONENT names a build tree.

    Exact ``build``, or a ``build-`` / ``build_`` / ``cmake-build-`` prefix.
    The separator is required: ``builders`` starts with ``build`` and is NOT a
    build directory, which is precisely the collision a ``build*`` glob would
    reintroduce.
    """
    return name == "build" or name.startswith(("build-", "build_", "cmake-build-"))


def is_build_output(rel: str) -> bool:
    """True when repo-relative `rel` lives inside a build tree.

    Directory components only -- a FILE called ``build`` is not a build tree.
    """
    parts = rel.split("/")
    for index, part in enumerate(parts[:-1]):
        if part in TOOL_OUTPUT_DIR_NAMES:
            return True
        if is_build_dir_name(part) and (index == 0 or parts[0] in BUILD_TREE_ROOTS):
            return True
    return False


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


def is_build_output_path(path: object) -> bool:
    """``is_build_output`` for a str or Path that may be absolute.

    The checkers hold a mix of absolute paths, repo-relative paths and
    slash-wrapped forms. Normalising here keeps every call site a single
    predicate instead of thirteen hand-rolled substring tuples (#377).
    """
    text = str(path).replace("\\", "/").strip("/")
    root = str(REPO_ROOT).replace("\\", "/").strip("/")
    if text.startswith(root + "/"):
        text = text[len(root) + 1 :]
    elif text.startswith("./"):
        text = text[2:]
    return is_build_output(text)


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
    if rel.startswith(EXCLUDED_PREFIXES) or is_build_output(rel):
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
    """Print the first-party file list, optionally filtered by language.

    Exits non-zero printing NOTHING when a requested language resolves to zero
    files. That is the contract the size gates depend on: an empty list must
    be distinguishable from a clean tree, or a broken enumeration reads as
    success.

    Returns 0 with the paths on stdout, 1 on an unknown or empty language.
    """
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
