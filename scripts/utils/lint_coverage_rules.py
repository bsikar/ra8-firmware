#!/usr/bin/env python3
"""Classification tables and provider descriptors for ``check_lint_coverage.py``.

This module is DATA plus pure helpers: it says what kinds of file exist in this
repository, which of them are code, who is supposed to lint and format each
kind, and which paths are deliberately outside every checker's reach. The
enforcement logic that consumes it lives in ``check_lint_coverage.py``.

The split is deliberate. The tables below are the part a human edits when a new
file type lands; keeping them away from the subprocess plumbing means that edit
is reviewable on its own, and means neither file drifts toward being the
2000-line checker nobody reads.

A NOTE ON WHAT "FORMATTER" MEANS HERE
-------------------------------------
Two different things enforce layout in this tree, and both count:

  * rewriters -- clang-format, shfmt, cmake-format, ruff format. Given a file
    they emit the canonical form, and the gate runs them in --check mode.
  * canonical-form checkers -- yamllint's style rules, check_makefiles.py,
    check_linker_scripts.py. Nothing rewrites a GNU ld script or a Makefile in
    this ecosystem, so these enforce the layout rules by rejecting deviations
    instead of by producing the fixed text.

The distinction that matters for coverage is "does something reject this file
for being laid out wrongly", not "can something rewrite it for me". A class
served only by a canonical-form checker is covered; a class served by nothing
is a gap, and gaps are enumerated in KNOWN_GAPS rather than quietly dropped.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable

# ---------------------------------------------------------------------------
# Roles
# ---------------------------------------------------------------------------
LINT = "lint"
FORMAT = "format"

# ---------------------------------------------------------------------------
# File kinds
#
#   CODE  -- hand-authored instructions to a machine. Must be linted AND
#            formatted. This is the class the gate exists to protect.
#   DATA  -- committed bytes that are read, not executed or compiled: fixtures,
#            baselines, assets, generated tables. No linter applies.
#   DOC   -- prose for humans. The ascii/attribution/terminology gates already
#            sweep every tracked file including these; there is no prose
#            formatter in this tree and inventing one is not this gate's job.
#   CONF  -- tool configuration consumed by a specific tool, which validates it
#            on use. A malformed .clang-format fails the format gate itself.
# ---------------------------------------------------------------------------
CODE = "code"
DATA = "data"
DOC = "doc"
CONF = "conf"


@dataclass(frozen=True)
class ClassSpec:
    """One file class: what kind it is and what coverage it therefore needs."""

    name: str
    kind: str
    why: str

    @property
    def needs_checking(self) -> bool:
        """True when files of this class must have both a linter and formatter."""
        return self.kind == CODE


def _spec(name: str, kind: str, why: str) -> ClassSpec:
    return ClassSpec(name, kind, why)


# Every class the repository contains. Adding a file type means adding a row
# here AND (if it is CODE) wiring a provider below -- the gate fails on an
# unclassified extension precisely so that decision cannot be skipped.
CLASSES: dict[str, ClassSpec] = {
    "c-family": _spec("c-family", CODE, "firmware, host tools and tests"),
    "asm": _spec("asm", CODE, "hand-written startup and low-level entry code"),
    "dockerfile": _spec("dockerfile", CODE, "the devcontainer that pins every CI tool version"),
    "zsh": _spec("zsh", CODE, "zsh dialect; shellcheck refuses zsh, so not `shell`"),
    "python": _spec("python", CODE, "the gate suite and host tooling"),
    "shell": _spec("shell", CODE, "gate drivers, HIL scripts, git hooks"),
    "cmake": _spec("cmake", CODE, "decides what compiles with which flags"),
    "make": _spec("make", CODE, "per-app and top-level build entry points"),
    "linker-script": _spec("linker-script", CODE, "the memory map is code"),
    "yaml": _spec("yaml", CODE, "workflows decide which gates run at all"),
    "markdown": _spec("markdown", DOC, "prose; swept by ascii/terminology gates"),
    "restructuredtext": _spec("restructuredtext", DOC, "prose, vendored doc trees"),
    "html": _spec("html", DATA, "rendered fixtures and doxygen fragments"),
    "css": _spec("css", DATA, "e-reader stylesheet assets shipped as content"),
    "javascript": _spec("javascript", DATA, "doxygen theme assets, not authored logic"),
    "json": _spec("json", CONF, "manifests and SBOM; consumers validate on load"),
    "toml": _spec("toml", CONF, "pyproject and friends; ruff validates its own"),
    "ini": _spec("ini", CONF, "tool configuration"),
    "xml": _spec("xml", DATA, "EPUB/OPF package descriptors and IDE leftovers"),
    "csv": _spec("csv", DATA, "measurement tables"),
    "text": _spec("text", DATA, "baselines, pin lists, licence text"),
    "binary": _spec("binary", DATA, "images, fonts, archives, PDFs, book fixtures"),
    "tool-config": _spec("tool-config", CONF, "dotfile config for a named tool"),
    "vcs-metadata": _spec("vcs-metadata", CONF, "gitignore/gitattributes and kin"),
    "fixture": _spec("fixture", DATA, "test corpora and golden inputs"),
}

# ---------------------------------------------------------------------------
# Extension -> class. Lower-cased suffix, including the dot.
# ---------------------------------------------------------------------------
EXT_CLASS: dict[str, str] = {
    # C family
    ".c": "c-family",
    ".h": "c-family",
    ".cpp": "c-family",
    ".hpp": "c-family",
    ".cc": "c-family",
    ".cxx": "c-family",
    ".hh": "c-family",
    ".hxx": "c-family",
    ".inc": "c-family",
    ".m": "c-family",
    # Assembly. GNU as accepts both; .S is preprocessed, .s is not.
    ".s": "asm",
    # Scripting
    ".py": "python",
    ".sh": "shell",
    ".bash": "shell",
    # NOT "shell": shellcheck explicitly refuses zsh input, so calling a .zsh
    # file shell would claim coverage that does not exist.
    ".zsh": "zsh",
    # Build systems
    ".cmake": "cmake",
    ".mk": "make",
    ".make": "make",
    ".ld": "linker-script",
    # Structured config
    ".yml": "yaml",
    ".yaml": "yaml",
    ".json": "json",
    ".toml": "toml",
    ".ini": "ini",
    ".cfg": "ini",
    ".properties": "ini",
    ".xml": "xml",
    ".csv": "csv",
    ".opf": "xml",
    ".user": "tool-config",
    # Documents and assets
    ".md": "markdown",
    ".rst": "restructuredtext",
    ".html": "html",
    ".xhtml": "html",
    ".css": "css",
    ".js": "javascript",
    ".txt": "text",
    ".conf": "text",
    ".dox": "markdown",
    ".in": "text",
    ".example": "text",
    ".args": "text",
    ".defs": "text",
    # Binary payloads
    ".png": "binary",
    ".jpg": "binary",
    ".jpeg": "binary",
    ".webp": "binary",
    ".svg": "binary",
    ".gif": "binary",
    ".pdf": "binary",
    ".ttf": "binary",
    ".otf": "binary",
    ".woff": "binary",
    ".woff2": "binary",
    ".epub": "binary",
    ".cbz": "binary",
    ".gz": "binary",
    ".xz": "binary",
    ".zip": "binary",
    ".a": "binary",
    ".bin": "binary",
    ".elf": "binary",
    ".hex": "binary",
    ".iex": "binary",
    ".yaff": "binary",
    ".mesh": "binary",
    ".tflite": "binary",
}

# ---------------------------------------------------------------------------
# Exact filename -> class, checked BEFORE the extension table. This is how
# extensionless build files and dotfile configs get classified.
# ---------------------------------------------------------------------------
NAME_CLASS: dict[str, str] = {
    "CMakeLists.txt": "cmake",
    "Dockerfile": "dockerfile",
    "zshrc": "zsh",
    "Makefile": "make",
    "makefile": "make",
    "GNUmakefile": "make",
    "Doxyfile": "tool-config",
    "VERSION": "text",
    "LICENSE": "text",
    "NOTICE": "text",
    "mimetype": "fixture",
    ".clang-format": "tool-config",
    ".clang-tidy": "tool-config",
    ".clangd": "tool-config",
    ".editorconfig": "tool-config",
    ".shellcheckrc": "tool-config",
    ".pylintrc": "tool-config",
    ".globalrc": "tool-config",
    ".cppcheck-suppressions": "tool-config",
    ".style_ignored_dirs": "tool-config",
    ".rat-excludes": "tool-config",
    ".gitignore": "vcs-metadata",
    ".gitattributes": "vcs-metadata",
    ".gitmodules": "vcs-metadata",
    ".gitkeep": "vcs-metadata",
    ".mailmap": "vcs-metadata",
}

# ---------------------------------------------------------------------------
# Shebang interpreter -> class. Consulted for files the tables above miss,
# which is how `scripts/git/pre-commit` (extensionless, #!/usr/bin/env bash)
# is recognised as shell rather than falling through as unclassified.
# ---------------------------------------------------------------------------
SHEBANG_CLASS: tuple[tuple[str, str], ...] = (
    ("python", "python"),
    ("bash", "shell"),
    ("zsh", "shell"),
    ("/sh", "shell"),
    ("env sh", "shell"),
)

# ---------------------------------------------------------------------------
# EXEMPTIONS -- paths no first-party checker is expected to reach.
#
# Every entry is a path prefix with a one-line reason. A prefix without a
# reason is not accepted by the loader below: an exemption list that grows
# without justification is exactly how the coverage question became
# unanswerable in the first place.
# ---------------------------------------------------------------------------
EXEMPT_PREFIXES: tuple[tuple[str, str], ...] = (
    ("libs/third_party/", "vendored SOUP; CLAUDE.md exempts it from first-party standards"),
    ("libs/fonts/", "generated glyph tables, not hand-authored"),
    ("tools/vela/generated/", "emitted by the Vela NPU compiler on every regen"),
    ("docs/reference/", "committed Renesas datasheet and HUM PDFs"),
    ("docs/doxygen_theme/", "vendored doxygen-awesome theme"),
    ("docs/build/", "generated Doxygen HTML output"),
    ("content/", "EPUB/CBZ book fixtures used as reader test content"),
    # NOT tests/fixtures/ as a whole. A blanket exemption there hid
    # tests/fixtures/epub/epub_probe.c and run_probe.sh -- real first-party
    # code that clang-format and shellcheck were in fact already covering. An
    # exemption that conceals covered code makes the matrix understate reality,
    # which is the same class of wrongness as one that conceals uncovered code.
    # Everything else under fixtures/ classifies as data on its own merits.
    ("tests/fuzz/corpus/", "libFuzzer corpora, machine-generated random inputs"),
    ("esp32/third_party/", "vendored ESP-IDF and esp-hosted sources"),
    (".devcontainer/p10k.zsh", "vendored powerlevel10k theme config, not a project script"),
)

# ---------------------------------------------------------------------------
# KNOWN_GAPS -- code files that NOTHING lints or formats today.
#
# This is NOT an exemption list, and the difference matters. An exemption says
# "this file is not ours to check". A gap says "this file IS ours, nothing
# checks it, here is the issue and here is exactly how many there are". Every
# entry carries a recorded count and the gate fails the moment a gap grows past
# it -- so a hole can be carried deliberately while it is being closed, but it
# can never widen unnoticed, and it can never quietly become permanent.
#
# Closing a gap means deleting its row, not raising its count.
# ---------------------------------------------------------------------------
RTOS_HEADERS = ("tx_api.h", "nx_api.h", "ux_api.h")


def includes_rtos_header(text: str) -> bool:
    """True when a TU includes a ThreadX / NetX / USBX vendor header."""
    return any(f"include <{h}>" in text or f'include "{h}"' in text for h in RTOS_HEADERS)


@dataclass(frozen=True)
class GapCtx:
    """What a gap predicate gets to look at when deciding if a file is its own."""

    rel: str
    cls: str
    text: str
    """File contents, read lazily and only for files already known uncovered."""


@dataclass(frozen=True)
class Gap:
    """One recorded hole in lint/format coverage."""

    name: str
    count: int
    issue: str
    reason: str
    match: Callable[[GapCtx], bool]


KNOWN_GAPS: tuple[Gap, ...] = (
    Gap(
        "asm",
        2,
        "#367",
        "no assembler linter or formatter exists in this tree; 2 hand-written "
        "startup TUs (esp32 boot, ThreadX low-level init)",
        lambda c: c.cls == "asm",
    ),
    Gap(
        "firmware-tu-no-cross-compile-db",
        435,
        "#368",
        "examples/, port/ and esp32/ are cross-compiled firmware. clang-tidy "
        "parses against the HOST compile_commands.json, which carries no ARM "
        "flags, per-app includes or vendor RTOS paths -- pointing it at these "
        "TUs yielded 135 findings across 96 files, every one a parse error and "
        "none an actionable style finding. Needs a cross-compile compile "
        "database, not a scope widening",
        lambda c: c.cls == "c-family" and c.rel.startswith(("examples/", "port/", "esp32/")),
    ),
    Gap(
        "rtos-tu-no-compile-db",
        3,
        "#368",
        "host-buildable trees still contain TUs that include ThreadX/NetX/USBX "
        "vendor headers the host compile database does not carry",
        lambda c: c.cls == "c-family" and includes_rtos_header(c.text),
    ),
    Gap(
        "cxx-objc-not-in-tidy-scope",
        12,
        "#369",
        "clang_tidy.sh collects only *.c and *.h, so first-party C++ shims and "
        "the two Objective-C host views are unlinted. The .m files additionally "
        "need a macOS runner",
        lambda c: (
            c.cls == "c-family"
            and c.rel.rsplit(".", 1)[-1] in ("cpp", "cc", "cxx", "hpp", "hh", "hxx", "m")
        ),
    ),
    Gap(
        "dockerfile",
        1,
        "#370",
        "hadolint is not provisioned on the runner; the devcontainer that pins "
        "every CI tool version is itself unlinted and unformatted",
        lambda c: c.cls == "dockerfile",
    ),
    Gap(
        "zsh",
        1,
        "#370",
        "shellcheck refuses zsh input and there is no zsh formatter in the tree; "
        "the devcontainer zshrc is unchecked",
        lambda c: c.cls == "zsh",
    ),
)


def exemption_reason(rel: str) -> str | None:
    """Return the justification for `rel` being exempt, or None if it is not."""
    for prefix, reason in EXEMPT_PREFIXES:
        if rel == prefix or rel.startswith(prefix):
            return reason
    return None


def validate_tables() -> list[str]:
    """Assert the tables are internally consistent. Returns a list of problems.

    A class named by EXT_CLASS/NAME_CLASS but absent from CLASSES would make
    the gate crash on a file it was supposed to classify, and an exemption with
    an empty reason is the rot this list exists to prevent.
    """
    problems: list[str] = []
    for table_name, table in (("EXT_CLASS", EXT_CLASS), ("NAME_CLASS", NAME_CLASS)):
        for key, cls in table.items():
            if cls not in CLASSES:
                problems.append(f"{table_name}[{key!r}] names unknown class {cls!r}")
    for _, cls in SHEBANG_CLASS:
        if cls not in CLASSES:
            problems.append(f"SHEBANG_CLASS names unknown class {cls!r}")
    for prefix, reason in EXEMPT_PREFIXES:
        if not reason.strip():
            problems.append(f"exemption {prefix!r} carries no reason")
    return problems
