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

import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Vendored SOUP and generated tables. Matches the CLAUDE.md exemption list and
# the sibling gates' EXCLUDE_FRAGMENTS.
EXCLUDED_PREFIXES = (
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "libs/ra8_fonts/",
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
# tell ``tools/ra8_emulator/build/`` -- genuine CMake output -- from a first-party
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
# behaviour being removed. `scripts/`, `libs/` and friends are
# absent because nothing builds into them, so a `build` directory appearing
# there is source and must stay visible to the checkers.
BUILD_TREE_ROOTS = frozenset(
    {
        "docs",  # docs/build/ -- generated Doxygen HTML
        "examples",  # examples/**/<app>/build/ -- per-app CMake output
        "local-poc",  # local-poc/**/build/ -- git-excluded PoC tree
        "port",  # port/**/build/
        "tests",  # tests/build/, tests/build-cov/, tests/build-fuzz/
        "tools",  # tools/<tool>/build/ -- host tool output
        "apps",  # apps/<category>/<product>/build/ -- product build output
    }
)

# Directory names owned by a tool, which can never be a first-party source
# directory and are therefore matched at ANY depth. This is the ONLY
# depth-agnostic rule left, and every name in it is reserved by the tool that
# creates it: CMake writes CMakeFiles/ and _deps/, CPython writes __pycache__/,
# Zig writes .zig-cache/, and npm writes node_modules/. Nobody can legitimately
# author a source directory with one of these names, so matching them anywhere
# cannot swallow source.
TOOL_OUTPUT_DIR_NAMES = frozenset(
    {".zig-cache", "CMakeFiles", "_deps", "__pycache__", "node_modules"}
)


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
    ".just": "just",
    ".ld": "ld",
    ".zig": "zig",
}

# Exact basenames that carry no suffix but are unambiguously one language.
BASENAME_LANG = {
    "CMakeLists.txt": "cmake",
    "justfile": "just",
    "Justfile": "just",
}

# Directories whose extensionless executables are shell by construction. The
# git hooks are the case that matters: scripts/git/commit-msg is 670 lines of
# shell that no suffix-driven scope has ever seen.
SHEBANG_LANG = {
    "sh": "shell",
    "bash": "shell",
    "zsh": "shell",
    "dash": "shell",
    "python": "python",
    "python3": "python",
}

LANGUAGES = ("c", "python", "shell", "cmake", "yaml", "just", "ld", "zig")


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


def repo_files(
    pathspec: tuple[str, ...] = (), *, root: Path = REPO_ROOT, caller: str = "lint_targets.py"
) -> list[str]:
    """Every present repository file in scope, tracked OR untracked-not-ignored.

    THE enumeration primitive for gates (#713). A checker that shells out to a
    bare ``git ls-files`` sees the INDEX, not the working tree, so a source
    file nobody has ``git add``ed yet is invisible to it -- and the checker
    reports PASS over code it never read.  That is not hypothetical: it turned
    ``dev`` red once, after a local run of the very gates that should have
    caught it came back clean.  ``--others --exclude-standard`` closes the hole
    while keeping ``.gitignore``d build output out of scope, so the cost of the
    fix is nothing.

    Deleted-but-still-tracked paths are dropped: ``--cached`` prints a path the
    index still carries after ``rm``, and handing it to a formatter fails with
    ``ENOENT`` during an ordinary deletion.  Filtering on existence makes the
    result describe the tree that is actually on disk, which is what every
    working-tree checker means by "the files".  A committed CI snapshot has no
    deletions pending, so the distinction never changes a CI verdict.

    Args:
        pathspec: Optional git pathspec words (``"*.c"``, ``"tests"``) narrowing
            the enumeration.  Empty means the whole repository.
        root: Repository to enumerate.  Defaults to this checkout; the selftest
            passes a throwaway fixture.
        caller: Script name for the FATAL diagnostic, so a failure names the
            gate that hit it rather than this module.

    Returns:
        Repo-relative paths of existing files, sorted.

    Raises:
        SystemExit: When git fails.  An enumeration that cannot run must never
            read as an empty -- that is, clean -- tree.
    """
    proc = subprocess.run(  # noqa: S603 -- trusted: fixed argv, no shell, no user input
        [  # noqa: S607 -- trusted: fixed git argv
            "git",
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
            "--",
            *pathspec,
        ],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"{caller}: FATAL -- `git ls-files` failed\n")
        sys.exit(2)
    return sorted({rel for rel in proc.stdout.split("\0") if rel and (root / rel).is_file()})


def untracked_in_scope(
    pathspec: tuple[str, ...] = (), *, root: Path = REPO_ROOT, caller: str = "lint_targets.py"
) -> list[str]:
    """Present files in scope that git does not track and does not ignore.

    The set a deliberately index-scoped gate cannot see.  Such a gate is not
    wrong to be index-scoped -- a pre-commit hook judges the prospective
    commit, not the tree -- but it must not report clean without saying what
    it declined to read (#713).

    Args:
        pathspec: Optional git pathspec words narrowing the enumeration.
        root: Repository to enumerate.
        caller: Script name for the FATAL diagnostic.

    Returns:
        Repo-relative paths of existing untracked, non-ignored files, sorted.
    """
    proc = subprocess.run(  # noqa: S603 -- trusted: fixed argv, no shell, no user input
        [  # noqa: S607 -- trusted: fixed git argv
            "git",
            "ls-files",
            "-z",
            "--others",
            "--exclude-standard",
            "--",
            *pathspec,
        ],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"{caller}: FATAL -- `git ls-files --others` failed\n")
        sys.exit(2)
    return sorted({rel for rel in proc.stdout.split("\0") if rel and (root / rel).is_file()})


def announce_unscanned(paths: list[str], *, caller: str, why: str, limit: int = 10) -> None:
    """Say on stderr which in-scope files this run did not read.

    Silence over unread code is the defect #713 exists to kill.  A gate that
    must stay index-scoped keeps its scope and pays for it with this line,
    every run, so a clean verdict is never mistaken for a complete one.

    Args:
        paths: Repo-relative paths the run skipped.  Empty prints nothing.
        caller: Script name to lead the notice with.
        why: Short phrase naming why they are out of scope.
        limit: Paths to name before summarising the remainder.
    """
    if not paths:
        return
    shown = ", ".join(paths[:limit])
    rest = len(paths) - limit
    if rest > 0:
        shown += f", and {rest} more"
    sys.stderr.write(f"{caller}: {len(paths)} untracked file(s) not scanned ({why}): {shown}\n")


def _tracked() -> list[str]:
    """Every present first-party candidate path in this checkout."""
    return repo_files()


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


# A tree this size cannot legitimately collapse to a handful of files. A checker
# that enumerates almost nothing reports a clean tree because it looked at
# almost nothing -- the exact failure this module exists to prevent. Same
# trip-wire as check_ruff.py and check_lint_coverage.py.
TRACKED_FLOOR = 1000


def first_party_paths(
    suffixes: tuple[str, ...], *, respect_language_excludes: bool = True
) -> list[str]:
    """Every tracked first-party path ending in one of ``suffixes``.

    The derived-scope primitive the policy checkers share (#358). Enumeration
    is ``git ls-files`` -- never a hardcoded directory list -- so a newly added
    top-level directory (``tools/`` was the one that had been silently omitted
    for the life of six checkers) is in scope the day it lands, with no
    allowlist to forget. The only subtractions are the named SOUP / generated /
    build-output exemptions this module already defines; with
    ``respect_language_excludes`` also the per-language vendored trees
    (``port/threadx/`` is C SOUP), which are not ours to police.

    Args:
        suffixes: Extensions to keep, e.g. ``(".c", ".h")``. Matched with
            ``str.endswith``, so pass lower-case dotted forms.
        respect_language_excludes: When true, also drop a path that is a
            vendored tree for the language its own suffix implies. Callers
            scanning text (docs, config) pass false, where it is a no-op.

    Returns:
        The matching repo-relative paths, sorted.

    Raises:
        SystemExit: When ``git ls-files`` returns fewer than ``TRACKED_FLOOR``
            paths -- a collapsed enumeration must fail, never read as clean.
    """
    rels = _tracked()
    if len(rels) < TRACKED_FLOOR:
        sys.stderr.write(
            f"lint_targets.py: FATAL -- only {len(rels)} tracked path(s), floor "
            f"is {TRACKED_FLOOR}. A collapsed enumeration reports a clean tree "
            "because it enumerated nothing.\n"
        )
        sys.exit(2)
    out: list[str] = []
    for rel in rels:
        if not rel.endswith(suffixes):
            continue
        if _excluded(rel):
            continue
        if respect_language_excludes:
            lang = _raw_language(rel, REPO_ROOT)
            if lang is not None and _excluded(rel, lang):
                continue
        out.append(rel)
    return sorted(out)


# ---------------------------------------------------------------------------
# FIRMWARE PRODUCTS -- the single definition, shared by every checker that has
# to tell a cross-compiled image from a host program.
#
# Top-level roots used to classify build domain on their own: examples/ and
# port/ were firmware, tests/ and tools/ were hosted. apps/ -- the products
# tier -- breaks that, because it carries BOTH kinds. The mdl CLI is a
# host program the C runtime starts and whose exit status something reads; the
# e-reader is a two-image TrustZone composition reached from Reset_Handler,
# with no process and no exit status. "It lives under apps/" answers nothing.
#
# The discriminator is what the build actually does with the directory: an app
# directory holding BOTH a linker script and a vector table is LINKED INTO AN
# IMAGE. Neither half alone is enough -- a host program could carry a stray
# .ld for some other purpose, and a vector_table.c with nothing placing it is
# not an image -- and no host program has ever needed both.
#
# Derived from ``git ls-files`` rather than listed, so a firmware product that
# lands tomorrow is classified the day it lands, with no allowlist to forget.
# ---------------------------------------------------------------------------

#: Products tier root. Only this root is ambiguous; the others classify by name.
PRODUCTS_ROOT = "apps/"

#: Proof that a directory is linked into an image rather than started by a C
#: runtime. Any ``.ld`` counts.
_IMAGE_MARKER_SUFFIX = ".ld"

#: Proof that a directory owns a reset path.
_IMAGE_MARKER_NAME = "vector_table.c"

#: The second, now primary, proof. RA8FW-309 and #759 moved the linker scripts and
#: the reset path OUT of the app directories and into the board libraries: one
#: generated NS template per target, one shared vector table. That left every
#: app directory without the marker pair, so the pair rule alone derives the
#: EMPTY set over the live tree -- a discriminator that classifies nothing and
#: reports agreement forever. What still distinguishes an image is the macro
#: the app's CMakeLists invokes: these two cross-compile and link, while a host
#: program reaches for plain ``add_executable``. Both rules are kept, ORed: the
#: pair is still sufficient on its own, so an app that carries its own script
#: and vector table is classified the day it lands, with no allowlist.
_IMAGE_MACROS = ("ra8_add_app", "ra8_add_ns_image")

#: Where a directory declares how it is built.
_BUILD_FILE = "CMakeLists.txt"

_IMAGE_MACRO_RE = re.compile(r"^[ \t]*(?:" + "|".join(_IMAGE_MACROS) + r")[ \t]*\(", re.MULTILINE)


def firmware_app_dirs(paths: list[str] | None = None, root: Path | None = None) -> tuple[str, ...]:
    """Every directory under ``apps/`` that builds a cross-compiled image.

    Args:
        paths: Repo-relative paths to classify. Defaults to the tracked tree,
            which is what every caller wants; the parameter exists so a
            selftest can drive the rule with a fixture instead of the live
            tree.
        root: Directory the paths are relative to, for reading the build file
            of a candidate. Defaults to the repository root. A fixture that
            does not list a ``CMakeLists.txt`` never reaches it.

    Returns:
        The matching repo-relative directories, sorted, with no trailing slash.
    """
    if paths is None:
        paths = [rel for rel in _tracked() if not is_build_output(rel)]
    if root is None:
        root = REPO_ROOT
    scripts: set[str] = set()
    vectors: set[str] = set()
    builders: set[str] = set()
    for rel in paths:
        if not rel.startswith(PRODUCTS_ROOT):
            continue
        head, _, name = rel.rpartition("/")
        if not head:
            continue
        if name.endswith(_IMAGE_MARKER_SUFFIX):
            scripts.add(head)
        elif name == _IMAGE_MARKER_NAME:
            app_dir, separator, leaf = head.rpartition("/")
            if separator and leaf == "src":
                vectors.add(app_dir)
        elif name == _BUILD_FILE and _links_an_image(root / rel):
            builders.add(head)
    return tuple(sorted((scripts & vectors) | builders))


def _links_an_image(build_file: Path) -> bool:
    """Does this ``CMakeLists.txt`` invoke a macro that links a firmware image?

    Unreadable is False rather than an error: ``paths`` may name a fixture file
    that was never written, and the pair rule still classifies such a tree.
    """
    try:
        text = build_file.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return bool(_IMAGE_MACRO_RE.search(text))


def _seed_enumeration_fixture(root: Path) -> None:
    """Build a throwaway git repo carrying one file of every enumeration class."""
    (root / "libs/ra8_new/src").mkdir(parents=True)
    (root / "build").mkdir()
    (root / ".gitignore").write_text("build/\n", encoding="ascii")
    (root / "libs/ra8_new/src/tracked.c").write_text("int tracked(void);\n", encoding="ascii")
    (root / "libs/ra8_new/src/deleted.c").write_text("int gone(void);\n", encoding="ascii")
    (root / "libs/ra8_new/src/untracked.c").write_text("int fresh(void);\n", encoding="ascii")
    (root / "build/generated.c").write_text("int built(void);\n", encoding="ascii")
    for argv in (
        ("init", "-q"),
        ("add", ".gitignore", "libs/ra8_new/src/tracked.c", "libs/ra8_new/src/deleted.c"),
        ("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "seed"),
    ):
        subprocess.run(  # noqa: S603 -- fixed git argv against a temporary fixture
            ["git", *argv],  # noqa: S607 -- trusted: git off PATH, as every gate runs it
            cwd=root,
            check=True,
            capture_output=True,
        )
    (root / "libs/ra8_new/src/deleted.c").unlink()


def _enumeration_cases(root: Path) -> tuple[tuple[bool, str], ...]:
    """Both directions of the #713 contract, against a fixture repo.

    The must-fire case is the one that matters: a source file written but never
    ``git add``ed has to appear, because a gate that cannot see it reports
    clean over code it never read.  The must-stay-quiet cases keep the fix from
    being bought with a worse bug -- sweeping ignored build output into every
    gate's scope, or handing a formatter a path the working tree no longer has.
    """
    _seed_enumeration_fixture(root)
    seen = repo_files(root=root)
    scoped = repo_files(("libs",), root=root)
    return (
        ("libs/ra8_new/src/untracked.c" in seen, "untracked, non-ignored source IS enumerated"),
        ("libs/ra8_new/src/tracked.c" in seen, "tracked source is still enumerated"),
        ("build/generated.c" not in seen, "gitignored build output stays out of scope"),
        ("libs/ra8_new/src/deleted.c" not in seen, "tracked-but-deleted path is not a target"),
        ("libs/ra8_new/src/untracked.c" in scoped, "pathspec narrowing keeps the untracked file"),
        (".gitignore" not in scoped, "pathspec narrowing still narrows"),
    )


def selftest() -> int:
    """Prove source classification includes tricky code and excludes real outputs/SOUP."""
    with tempfile.TemporaryDirectory(prefix="lint-targets-selftest-") as raw:
        root = Path(raw)
        (root / "enumeration").mkdir()
        hook = root / "scripts/git/commit-msg"
        hook.parent.mkdir(parents=True)
        hook.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="ascii")
        cases = (
            (language_of("scripts/git/commit-msg", root) == "shell", "shebang-only hook is shell"),
            (
                language_of("internal/build/helper.sh", root) == "shell",
                "source build dir is visible",
            ),
            (is_build_output("tools/demo/build/object.o"), "tool build output is excluded"),
            (is_build_output("apps/host/reg_gen/.zig-cache/cache.zig"), "Zig cache is excluded"),
            (
                not is_build_output("internal/build/helper.sh"),
                "non-product build directory is not output",
            ),
            (
                language_of("port/threadx/src/vendor.c", root) is None,
                "language-specific vendored C is excluded",
            ),
            (
                firmware_app_dirs(
                    [
                        "apps/board/reader/linker.ld",
                        "apps/board/reader/src/vector_table.c",
                        "apps/host/tool/linker.ld",
                    ]
                )
                == ("apps/board/reader",),
                "firmware product needs linker and vector markers",
            ),
            *_enumeration_cases(root / "enumeration"),
        )
    failed = [label for passed, label in cases if not passed]
    for passed, label in cases:
        print(f"  [{'ok' if passed else 'FAIL'}] {label}")
    if failed:
        print(f"lint_targets.py --selftest: {len(failed)} failure(s)", file=sys.stderr)
        return 1
    print("lint_targets.py --selftest: all cases pass (both directions).")
    return 0


def main(argv: list[str]) -> int:
    """Print the first-party file list, optionally filtered by language.

    Exits non-zero printing NOTHING when a requested language resolves to zero
    files. That is the contract the size gates depend on: an empty list must
    be distinguishable from a clean tree, or a broken enumeration reads as
    success.

    Returns 0 with the paths on stdout, 1 on an unknown or empty language.
    """
    args = argv[1:]
    if args == ["--selftest"]:
        return selftest()
    if args == ["--list"]:
        print("\n".join(LANGUAGES))
        return 0
    if any(arg.startswith("-") for arg in args):
        sys.stderr.write("usage: lint_targets.py [--list|--selftest|LANGUAGE ...]\n")
        return 2
    requested = tuple(args) or LANGUAGES
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
