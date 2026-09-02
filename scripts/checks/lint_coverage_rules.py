#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
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
  * canonical-form checkers -- yamllint's style rules,
    check_linker_scripts.py. Nothing rewrites a GNU ld script or a justfile in
    this ecosystem, so these enforce the layout rules by rejecting deviations
    instead of by producing the fixed text.

The distinction that matters for coverage is "does something reject this file
for being laid out wrongly", not "can something rewrite it for me". A class
served only by a canonical-form checker is covered; a class served by nothing
is a gap, and gaps are enumerated in KNOWN_GAPS rather than quietly dropped.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

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
    "just": _spec("just", CODE, "just task runner recipes"),
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
    "validated-input": _spec(
        "validated-input",
        CONF,
        "exact machine-readable inputs parsed by their pinned build or generator",
    ),
    "generated-source": _spec(
        "generated-source",
        DATA,
        "exact reproducible source outputs whose generator owns the canonical bytes",
    ),
    "ansible-systemd-template": _spec(
        "ansible-systemd-template",
        CODE,
        "privileged Jinja/systemd input checked against the fleet role contract",
    ),
}

# ---------------------------------------------------------------------------
# Exact path -> class, checked BEFORE basename and extension tables.
#
# These exact files deliberately do not create a blanket exemption for every
# future .patch, `series`, .proto, or *pb-c.c file. The coprocessor patch is
# checked by its pinned build; the SOUP patch series are replayed byte-exact by
# check_third_party_patches.py; the schema is parsed by the pinned protobuf-c
# generator; and the two codec files are that generator's reproducible outputs.
# A second file of any of these types remains unclassified/C-family and makes
# lint-coverage fail until it receives an equally specific validation story.
# ---------------------------------------------------------------------------
PATH_CLASS: dict[str, str] = {
    "infra/ansible/roles/dev_box/templates/ra8-hil-runner.service.j2": "ansible-systemd-template",
    "infra/ansible/roles/dev_box/templates/ra8-hil-privileged-policy.json.j2": "validated-input",
    "scripts/hil/lib/ra8-hil-privileged.sha256": "validated-input",
    "scripts/checks/patches/cppcheck-2.13/misra_9-c23-empty-initializer.patch": "validated-input",
    "coprocessor/esp32c6/patches/0001-custom-rpc-sync-response-hook.patch": "validated-input",
    "coprocessor/esp32c6/patches/series": "validated-input",
    "docs/sbom/patches/levelx/0001-remove-nested-attribute-macros.patch": "validated-input",
    "docs/sbom/patches/levelx/series": "validated-input",
    "docs/sbom/patches/libwebp/0001-use-ra8-arena-allocator.patch": "validated-input",
    "docs/sbom/patches/libwebp/series": "validated-input",
    "docs/sbom/patches/mbedtls/0001-track-generated-config-headers.patch": "validated-input",
    "docs/sbom/patches/mbedtls/series": "validated-input",
    "docs/sbom/patches/netxduo/0001-remove-nested-attribute-macros.patch": "validated-input",
    "docs/sbom/patches/netxduo/series": "validated-input",
    "docs/sbom/patches/stb/0001-harden-font-parser-bounds.patch": "validated-input",
    "docs/sbom/patches/stb/series": "validated-input",
    "docs/sbom/patches/threadx/0001-remove-nested-attribute-macros.patch": "validated-input",
    "docs/sbom/patches/threadx/series": "validated-input",
    "docs/sbom/patches/usbx/0001-remove-nested-attribute-macros.patch": "validated-input",
    "docs/sbom/patches/usbx/series": "validated-input",
    "libs/ra8_c6link/proto/ra8_media_download.proto": "validated-input",
    "libs/ra8_c6link/inc/ra8_media_download.pb-c.h": "generated-source",
    "libs/ra8_c6link/src/ra8_media_download.pb-c.c": "generated-source",
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
    ".just": "just",
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
    ".tsv": "csv",
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
    ".lock": "text",
    ".conf": "text",
    ".dox": "markdown",
    ".in": "text",
    ".example": "text",
    ".args": "text",
    ".defs": "text",
    ".env": "text",
    ".defaults": "text",
    # docs/sbom/upstream/*.manifest -- generated evidence, one line per vendored
    # file. Not code and not hand-authored, but not unvalidated either:
    # check_soup_upstream.py parses every record strictly and fails on a
    # malformed one, so this class is claimed by that gate rather than a linter.
    ".manifest": "text",
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
    "justfile": "just",
    "Justfile": "just",
    "Doxyfile": "tool-config",
    "VERSION": "text",
    "LICENSE": "text",
    "NOTICE": "text",
    "mimetype": "fixture",
    ".clang-format": "tool-config",
    ".clang-tidy": "tool-config",
    ".clangd": "tool-config",
    ".editorconfig": "tool-config",
    ".dockerignore": "tool-config",
    ".shellcheckrc": "tool-config",
    ".pylintrc": "tool-config",
    ".globalrc": "tool-config",
    ".cppcheck-suppressions": "tool-config",
    # cppcheck-only C23 nullptr shim, force-included by the cppcheck gate; it is
    # never compiled into any TU, so clang-tidy cannot claim it as c-family and
    # it is classified for what it is -- configuration for a named tool.
    "cppcheck_c23_compat.h": "tool-config",
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
    ("libs/third_party/", "vendored platform SOUP; CLAUDE.md exempts it"),
    ("apps/shared_libs/third_party/", "vendored app SOUP; CLAUDE.md exempts it"),
    ("libs/ra8_fonts/", "generated glyph tables, not hand-authored"),
    ("tools/vela/generated/", "emitted by the Vela NPU compiler on every regen"),
    ("docs/reference/", "committed Renesas datasheet and HUM PDFs"),
    ("docs/doxygen_theme/", "vendored doxygen-awesome theme"),
    ("docs/build/", "generated Doxygen HTML output"),
    ("content/", "EPUB/CBZ book fixtures used as reader test content"),
    (
        "apps/board/stand_alone/ereader/content/",
        "EPUB/CBZ book fixtures used as reader test content",
    ),
    # NOT tests/fixtures/ as a whole. A blanket exemption there hid
    # apps/shared_libs/epub/tests/src/epub_probe.c and tests/fixtures/epub/run_probe.sh -- real
    # first-party
    # code that clang-format and shellcheck were in fact already covering. An
    # exemption that conceals covered code makes the matrix understate reality,
    # which is the same class of wrongness as one that conceals uncovered code.
    # Everything else under fixtures/ classifies as data on its own merits.
    ("tests/fuzz/corpus/", "libFuzzer corpora, machine-generated random inputs"),
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
        "objc-needs-macos-runner",
        2,
        "#436",
        "the two Objective-C host views (ra8_emulator, ra8_viewer) are AppKit / "
        "CoreGraphics code. clang-tidy can only parse them against the macOS "
        "SDK, so clang_tidy.sh claims them on Darwin and not on Linux -- where "
        "CI runs. The C++ half of #370 is fully closed: every .cpp/.cc finding "
        "is fixed and its tidy-baseline rows are gone. Closing this one needs a "
        "macOS lint job, which is a runner decision, not a code change",
        lambda c: c.cls == "c-family" and c.rel.endswith(".m"),
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

    A class named by PATH_CLASS/EXT_CLASS/NAME_CLASS but absent from CLASSES would make
    the gate crash on a file it was supposed to classify, and an exemption with
    an empty reason is the rot this list exists to prevent.
    """
    problems: list[str] = []
    for table_name, table in (
        ("PATH_CLASS", PATH_CLASS),
        ("EXT_CLASS", EXT_CLASS),
        ("NAME_CLASS", NAME_CLASS),
    ):
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
