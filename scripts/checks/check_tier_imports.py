#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: the tier dependency arrow points one way, and nothing may reverse it.

The tree has three tiers (#718), and the products tier has an internal layer of
its own:

* PLATFORM -- ``libs/``, ``port/``, ``src/``, ``tools/``. General-purpose,
  reusable, knowing nothing about any one product.
* PRODUCTS -- ``apps/``, split by FORM. ``apps/stand_alone/`` holds host-form
  products, ``apps/threadx_modules/`` holds module-form products, and
  ``apps/shared/`` holds the portable product-tier code both forms consume.
* CONSUMERS -- ``examples/`` and ``tests/``, which exist to demonstrate and to
  compile the other two and therefore legitimately name any path in the tree.

Two rules follow from that shape, and this gate is both of them:

**Rule 1 -- the platform never imports a product.** Nothing under a platform
root may ``#include`` a header from ``apps/`` or name an ``apps/`` path in a
CMake source list or include directory. Any category: a platform file reaching
into ``apps/shared/`` is the same defect as one reaching into
``apps/stand_alone/``. The moment it does, the platform has stopped being
general-purpose and the tier boundary is a comment rather than a fact.

**Rule 2 -- shared product code never imports a product FORM.**
``apps/shared/`` sits below ``apps/stand_alone/`` and ``apps/threadx_modules/``:
the forms consume shared freely, and shared must not reach back up. Shared code
that knows which form is compiling it is not shared, it is a copy of one form
with a switch in it.

Both rules are the same shape -- a layer, and the region it may not reach into
-- so both are expressed as one ``Layer`` row and scanned by one scanner. That
model lives in ``tier_layers.py``: the layers, their roots, the populations the
non-vacuity floors are asserted on, and the exclusive-basename census. This
file is the scanner and the gate entry point. The split is the reason the rule
set is keyed on CATEGORY DIRECTORY NAMES rather than on a file list -- a
product moving between categories is a change to the model and to nothing here,
so the gate keeps working across a layout change.

``tier_layers.py`` carries no selftest of its own by design: it is a model, not
a detector, and every predicate and floor in it is proved in both directions
below, through the same entry point CI drives.

TWO HALVES, BECAUSE THERE ARE TWO WAYS IN
-----------------------------------------

* **Includes.** A C/C++ file must not ``#include`` a header from a forbidden
  region.
* **CMake.** A listfile must not name a forbidden region's path in a source
  list, an include directory, or anywhere else -- compiling another layer's
  translation unit into your target is the same coupling in another language.

PRECISION VERSUS RECALL
-----------------------

Calibrated for ZERO false positives on the current tree, because a boundary
gate that cries wolf gets bypassed and then the boundary is gone again. Three
deliberate choices buy that:

1. **Comments are stripped first, in both languages.** A naive ``grep`` for
   ``apps/`` over the platform listfiles reports three hits today
   (``tools/rabook_imagepack/CMakeLists.txt``, ``cmake/ra8_app/sources.cmake``,
   ``cmake/ra8_webp_vendor.cmake``) and all three are PROSE -- comments
   explaining which producers exist and which two files once faked a WebP
   symbol. Those are exactly the false positives that would sink the gate, so
   ``#`` and ``#[[ ]]`` comments in CMake, and ``//`` and block comments in C,
   are removed before matching.
2. **The include directive is anchored to the start of its line.** A C string
   literal containing the text of an include (a code generator emitting C)
   cannot start a line with ``#``, so the anchor costs no real recall and
   removes a whole false-positive class without needing a full C lexer.
3. **The bare-name rule is keyed on an EXCLUSIVE basename census.** A file that
   writes ``#include "mdl_cache.h"`` names no directory, yet it can only ever
   resolve to a product header. The gate therefore builds a repo-wide
   header-basename census first and flags a bare include only when its basename
   exists inside the forbidden region and NOWHERE else in the tree. Measured
   2026-08-17 for Rule 1: 52 headers under ``apps/``, and the intersection with
   the basenames of every other header in the repository -- first-party,
   vendored SOUP and generated alike -- is EMPTY, so every one of the 52 is
   unambiguous.

The recall this gives up is stated rather than hidden:

* A bare include whose basename ALSO exists outside the forbidden region is not
  flagged. Under every include-directory ordering this tree's builds use, such
  an include resolves to the legal copy, so flagging it would be a guess about
  the build rather than a fact about the source.
* Include search paths are not replayed per translation unit. Doing that would
  make the gate depend on a configured compile database, and therefore on a
  build succeeding, which is precisely how a checker ends up silently scoped to
  whatever last configured.
* Rule 2's bare-name half is only as strong as the forms' exclusive census, and
  that census is legitimately allowed to be EMPTY -- ``apps/shared/`` may hold
  every header while a form is a single ``main.c``. Rule 2's literal-path and
  CMake halves do not depend on the census and are always live, which is why
  the non-vacuity floors below apply to the tier populations rather than to
  this one derived set.

What is caught with no ambiguity at all is the literal form: any include whose
path carries the forbidden region's DIRECTORY components -- ``apps/...``,
``../../apps/...``, ``apps/stand_alone/...`` -- fires regardless of the census.
No platform path in this tree contains a component named ``apps`` (measured:
zero), so that rule cannot misfire either.

THE CMAKE EXEMPTION IS ENUMERATED, NOT WILDCARDED
-------------------------------------------------

Orchestration has to be able to name a product: something must eventually
``add_subdirectory()`` one. That permission is granted to an explicit list of
exact paths (``ORCHESTRATION_EXEMPT``), never to a pattern, and even inside
those files it is narrow: only an ``add_subdirectory`` line may name a
forbidden region. A source list or an include directory in an exempt file still
fails, because "this file is allowed to know a product exists" is a different
claim from "this file is allowed to compile one".

Run::

    check_tier_imports.py --all       # the gate: sweep every ruled layer
    check_tier_imports.py FILE ...    # scan named files
    check_tier_imports.py --selftest  # prove it in both directions

Exit 0 when clean, 1 on a tier violation, 2 when the scan itself collapsed.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from selftest_assert import expect, report
from tier_layers import (
    APPS_C_FILE_FLOOR,
    APPS_HEADER_FLOOR,
    C_ROOT_FILE_FLOORS,
    CMAKE_REGION_RES,
    CMAKE_TOTAL_FILE_FLOOR,
    EXEMPT_CONSUMER_ROOTS,
    FORM_CATEGORIES,
    ORCHESTRATION_EXEMPT,
    PLATFORM_C_ROOTS,
    PLATFORM_LAYER_NAME,
    PRODUCTS_ROOT,
    REPO_ROOT,
    SHARED_CATEGORY,
    SHARED_LAYER_NAME,
    Census,
    Layer,
    build_exclusive,
    census_floor_errors,
    layer_for_c,
    layer_for_cmake,
    measure,
    normalize_rel,
    region_parts,
)

# A C preprocessor include, anchored to the start of its (comment-stripped)
# line. See "PRECISION VERSUS RECALL" for why the anchor is load-bearing.
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')

ADD_SUBDIRECTORY_RE = re.compile(r"\badd_subdirectory\s*\(")

KIND_PATH = "PATH"
KIND_HEADER = "HEADER"
KIND_CMAKE = "CMAKE"

EXIT_OK = 0
EXIT_VIOLATION = 1
EXIT_VACUOUS = 2

# (layer, kind, rel, line number, what was named, the offending source line)
Finding = tuple[str, str, str, int, str, str]

# (path on disk, repo-relative display path)
Target = tuple[Path, str]


def strip_c_comments(text: str) -> list[str]:
    """Return `text`'s lines with `//` and `/* */` comment content removed.

    String literals are left intact -- unlike the shared ``blank_noncode``
    lexer, which blanks them and would erase the very ``"header.h"`` operand
    this gate reads.

    Args:
        text: Whole C/C++ translation unit or header.

    Returns:
        One string per input line, comment bytes dropped, so line numbers still
        index the original file.
    """
    lines: list[str] = []
    in_block = False
    for raw in text.splitlines():
        kept: list[str] = []
        index = 0
        while index < len(raw):
            pair = raw[index : index + 2]
            if in_block:
                in_block = pair != "*/"
                index += 1 if in_block else 2
            elif pair == "/*":
                in_block = True
                index += 2
            elif pair == "//":
                break
            else:
                kept.append(raw[index])
                index += 1
        lines.append("".join(kept))
    return lines


def _first_hash_outside_quotes(line: str) -> int:
    """Return the index of the first `#` not inside a double-quoted string.

    Args:
        line: One CMake source line, bracket comments already removed.

    Returns:
        The index, or -1 when the line carries no line comment.
    """
    in_quote = False
    escaped = False
    for index, char in enumerate(line):
        if escaped:
            escaped = False
        elif char == "\\":
            escaped = True
        elif char == '"':
            in_quote = not in_quote
        elif char == "#" and not in_quote:
            return index
    return -1


def strip_cmake_comments(text: str) -> list[str]:
    """Return `text`'s lines with CMake line and bracket comments removed.

    Args:
        text: Whole ``CMakeLists.txt`` or ``*.cmake`` file.

    Returns:
        One string per input line, so line numbers still index the original.
    """
    lines: list[str] = []
    in_bracket = False
    for raw in text.splitlines():
        line = raw
        if in_bracket:
            end = line.find("]]")
            if end < 0:
                lines.append("")
                continue
            line = line[end + 2 :]
            in_bracket = False
        start = line.find("#[[")
        if start >= 0:
            end = line.find("]]", start + 3)
            if end < 0:
                in_bracket = True
                line = line[:start]
            else:
                line = line[:start] + line[end + 2 :]
        hash_index = _first_hash_outside_quotes(line)
        lines.append(line if hash_index < 0 else line[:hash_index])
    return lines


def classify_include(
    target: str, forbidden: tuple[str, ...], exclusive: frozenset[str]
) -> tuple[str, str] | None:
    """Classify one include operand against a layer's forbidden regions.

    Args:
        target: The text between the quotes or angle brackets.
        forbidden: Region prefixes the including layer may not reach into.
        exclusive: Header basenames that exist inside those regions and nowhere
            else in the repository.

    Returns:
        ``(kind, what was named)`` for a violation, or None when the include is
        legal. ``PATH`` means the operand spells the region's directory
        components; ``HEADER`` means its basename can only be a header from it.
    """
    parts = [part for part in target.replace("\\", "/").split("/") if part not in ("", ".")]
    while parts and parts[0] == "..":
        parts.pop(0)
    if not parts:
        return None
    for prefix in forbidden:
        region = region_parts(prefix)
        span = len(region)
        for start in range(len(parts) - span):
            if tuple(parts[start : start + span]) == region:
                return (KIND_PATH, prefix + "/".join(parts[start + span :]))
    if parts[-1] in exclusive:
        return (KIND_HEADER, parts[-1])
    return None


def scan_c_text(text: str, rel: str, layer: Layer, exclusive: frozenset[str]) -> list[Finding]:
    """Return every cross-layer include in one C-family file.

    Args:
        text: File contents.
        rel: Repo-relative display path.
        layer: The ruled layer that owns the file.
        exclusive: Exclusive header basenames for that layer's forbidden
            regions.

    Returns:
        One finding per offending ``#include`` line.
    """
    findings: list[Finding] = []
    for lineno, line in enumerate(strip_c_comments(text), 1):
        match = INCLUDE_RE.match(line)
        if match is None:
            continue
        verdict = classify_include(match.group(2).strip(), layer.forbidden, exclusive)
        if verdict is not None:
            kind, named = verdict
            findings.append((layer.name, kind, rel, lineno, named, line.strip()))
    return findings


def scan_cmake_text(text: str, rel: str, layer: Layer) -> list[Finding]:
    """Return every cross-layer path reference in one CMake listfile.

    Args:
        text: File contents.
        rel: Repo-relative display path; decides orchestration exemption.
        layer: The ruled layer that owns the listfile.

    Returns:
        One finding per offending line. In an ``ORCHESTRATION_EXEMPT`` listfile
        an ``add_subdirectory`` line is allowed and everything else still
        fires.
    """
    exempt = normalize_rel(rel) in ORCHESTRATION_EXEMPT
    findings: list[Finding] = []
    for lineno, line in enumerate(strip_cmake_comments(text), 1):
        for prefix in layer.forbidden:
            match = CMAKE_REGION_RES[prefix].search(line)
            if match is None:
                continue
            if exempt and ADD_SUBDIRECTORY_RE.search(line):
                continue
            named = line[match.start() :].split()[0].rstrip(")\"'")
            findings.append((layer.name, KIND_CMAKE, rel, lineno, named, line.strip()))
            break
    return findings


def _git_working_tree() -> list[str]:
    """Enumerate tracked and newly-added (non-ignored) paths.

    Returns:
        Repo-relative paths, so a brand-new file is judged the moment it is
        written rather than the moment it is committed.

    Raises:
        SystemExit: When git cannot enumerate the working tree; a gate that
            cannot see the tree must fail, never report clean.
    """
    proc = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],  # noqa: S607
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write("check_tier_imports.py: FATAL -- git working-tree enumeration failed\n")
        raise SystemExit(EXIT_VACUOUS)
    return [rel for rel in proc.stdout.split("\0") if rel]


def _working_scope() -> tuple[list[Target], list[Target], dict[str, frozenset[str]]]:
    """Enumerate every ruled layer and prove the enumeration is not vacuous.

    Returns:
        The C-family targets, the CMake targets, and each layer's exclusive
        header-basename census.

    Raises:
        SystemExit: When any non-vacuity floor is violated.
    """
    rels = _git_working_tree()
    errors = census_floor_errors(measure(rels))
    if errors:
        sys.stderr.write("check_tier_imports.py: FATAL -- " + "; ".join(errors) + "\n")
        raise SystemExit(EXIT_VACUOUS)
    ordered = sorted(rels)
    c_targets = [
        (REPO_ROOT / rel, normalize_rel(rel)) for rel in ordered if layer_for_c(rel) is not None
    ]
    cmake_targets = [
        (REPO_ROOT / rel, normalize_rel(rel)) for rel in ordered if layer_for_cmake(rel) is not None
    ]
    return (
        [target for target in c_targets if target[0].is_file()],
        [target for target in cmake_targets if target[0].is_file()],
        build_exclusive(rels),
    )


def _explicit_scope(raw_paths: Iterable[str]) -> tuple[list[Target], list[Target]]:
    """Filter caller-named files through the same layer-ownership policy.

    Args:
        raw_paths: Paths from argv.

    Returns:
        The in-scope C-family targets and CMake targets.
    """
    c_targets: list[Target] = []
    cmake_targets: list[Target] = []
    for raw in raw_paths:
        path = Path(raw)
        absolute = path if path.is_absolute() else REPO_ROOT / path
        try:
            rel = absolute.resolve().relative_to(REPO_ROOT).as_posix()
        except ValueError:
            continue
        if not absolute.is_file():
            continue
        if layer_for_c(rel) is not None:
            c_targets.append((absolute, rel))
        elif layer_for_cmake(rel) is not None:
            cmake_targets.append((absolute, rel))
    return sorted(set(c_targets)), sorted(set(cmake_targets))


def scan_targets(
    c_targets: Iterable[Target],
    cmake_targets: Iterable[Target],
    exclusive: dict[str, frozenset[str]],
) -> tuple[int, list[Finding]]:
    """Scan both halves of every ruled layer.

    This is the one scanning entry point ``--all``, an explicit file list and
    ``--selftest`` all drive, so the selftest cannot prove a code path CI does
    not run. Layer ownership is decided HERE, so handing it a file from an
    unruled layer (a product form, an example, a test) is a no-op -- which is
    what makes "a form including a shared header stays quiet" a property of the
    scanner rather than of the caller's filtering.

    Args:
        c_targets: Candidate C-family files.
        cmake_targets: Candidate CMake listfiles.
        exclusive: Per-layer exclusive header-basename census.

    Returns:
        The number of files actually scanned and every finding, in scan order.
    """
    findings: list[Finding] = []
    scanned = 0
    for path, rel in c_targets:
        layer = layer_for_c(rel)
        if layer is None:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        findings.extend(scan_c_text(text, rel, layer, exclusive.get(layer.name, frozenset())))
        scanned += 1
    for path, rel in cmake_targets:
        layer = layer_for_cmake(rel)
        if layer is None:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        findings.extend(scan_cmake_text(text, rel, layer))
        scanned += 1
    return scanned, findings


def _write(root: Path, rel: str, text: str) -> Target:
    """Materialise one selftest fixture and return it as a scan target.

    Args:
        root: Temporary directory standing in for the repository root.
        rel: Repo-relative path the fixture pretends to occupy.
        text: File contents.

    Returns:
        The ``(path, rel)`` pair ``scan_targets`` consumes.
    """
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return (path, rel)


# Rule 1 -- a platform file reaching into any products category.
_FIRE_C_RULE1 = {
    "libs/ra8_tier/src/literal.c": '#include "apps/stand_alone/media_dl/inc/mdl_cache.h"\n',
    "libs/ra8_tier/src/shared.c": '#include "apps/shared/media_dl/inc/mdl_cache.h"\n',
    "libs/ra8_tier/src/relative.c": '#include "../../../apps/thing/inc/mdl_cache.h"\n',
    "libs/ra8_tier/src/bare.c": '#include "mdl_cache.h"\n',
    "tools/tier_tool/src/angle.c": "#include <mdl_cache.h>\n",
    "port/posix/src/nested.c": '#include "media_dl/inc/mdl_cache.h"\n',
    "src/secure_app/src/deep.c": '#include "apps/threadx_modules/dl/inc/dl.h"\n',
}

# Rule 2 -- shared product code reaching up into a product FORM.
_FIRE_C_RULE2 = {
    "apps/shared/media_dl/src/up_standalone.c": (
        '#include "apps/stand_alone/media_dl/inc/mdl_cli.h"\n'
    ),
    "apps/shared/media_dl/src/up_threadx.c": (
        '#include "apps/threadx_modules/downloader/inc/dl_module.h"\n'
    ),
    "apps/shared/media_dl/src/up_bare.c": '#include "mdl_cli.h"\n',
}

_FIRE_CMAKE = {
    "libs/ra8_tier/CMakeLists.txt": (
        "target_sources(ra8_tier PRIVATE\n  ${FW_ROOT}/apps/shared/media_dl/src/mdl_hash.c)\n"
    ),
    "cmake/tier.cmake": (
        "target_include_directories(t PRIVATE ${FW_ROOT}/apps/stand_alone/media_dl/inc)\n"
    ),
    # An exempt orchestrator may add_subdirectory a product -- but COMPILING one
    # is a different claim, and it still fires.
    "CMakeLists.txt": "target_sources(all PRIVATE apps/stand_alone/media_dl/src/main.c)\n",
    "apps/shared/media_dl/CMakeLists.txt": (
        "target_sources(mdl_core PRIVATE ${FW_ROOT}/apps/stand_alone/media_dl/src/mdl_cli.c)\n"
    ),
}


def _selftest_must_fire(
    root: Path, exclusive: dict[str, frozenset[str]], failures: list[str]
) -> None:
    """Prove every violation shape, for both rules, reaches the real scanner.

    Args:
        root: Temporary fixture root.
        exclusive: Census the fixtures are written against.
        failures: Accumulator from ``selftest_assert``.
    """
    for label, fixtures in (("rule 1", _FIRE_C_RULE1), ("rule 2", _FIRE_C_RULE2)):
        for rel, text in fixtures.items():
            _, found = scan_targets([_write(root, rel, text)], [], exclusive)
            expect(len(found) == 1, f"{label} include fires: {rel}", failures)
    for rel, text in _FIRE_CMAKE.items():
        _, found = scan_targets([], [_write(root, rel, text)], exclusive)
        expect(len(found) == 1, f"listfile fires: {rel}", failures)


_QUIET_C = {
    "libs/ra8_tier/src/legal.c": (
        '#include "ra8_attributes.h"\n'
        '#include "ra8_check.h"\n'
        "#include <stdint.h>\n"
        '/* #include "apps/stand_alone/media_dl/inc/mdl_cache.h" -- not compiled */\n'
        '// #include "mdl_cache.h"\n'
        'static const char *k_help = "#include \\"mdl_cache.h\\"";\n'
        'static const char *k_path = "apps/stand_alone/media_dl";\n'
    ),
    "libs/ra8_tier/src/lookalike.c": '#include "myapps/thing.h"\n#include "ra8_apps_registry.h"\n',
    # Shared consuming the platform and its own category is the whole point.
    "apps/shared/media_dl/src/legal.c": (
        '#include "ra8_check.h"\n'
        '#include "apps/shared/media_dl/inc/mdl_cache.h"\n'
        '#include "mdl_cache.h"\n'
        "#include <stdint.h>\n"
    ),
    # A product FORM consuming shared -- the arrow's legal direction. It is not
    # a ruled layer at all, which scan_targets decides for itself.
    "apps/stand_alone/media_dl/src/main.c": (
        '#include "apps/shared/media_dl/inc/mdl_cache.h"\n#include "mdl_cache.h"\n'
    ),
    "apps/threadx_modules/downloader/src/mod.c": (
        '#include "apps/shared/media_dl/inc/mdl_cache.h"\n'
    ),
}

_QUIET_CMAKE = {
    "tools/rabook_imagepack/CMakeLists.txt": (
        "# The single-unit counterpart to the batch producers "
        "(apps/stand_alone/media_dl, ...)\n"
        "add_executable(rabook_imagepack src/main.c)\n"
    ),
    "cmake/ra8_webp_vendor.cmake": (
        "#[[ tools/rabook_imagepack and apps/stand_alone/media_dl each faked\n"
        "    ra8_jof_priv_webp_transcode() ]]\n"
        "add_library(ra8_webp STATIC ${RA8_WEBP_SRCS})\n"
    ),
    "CMakeLists.txt": "add_subdirectory(apps/stand_alone/media_dl)\n",
    "libs/ra8_tier/CMakeLists.txt": "target_sources(ra8_tier PRIVATE src/tier.c)\n",
    "apps/shared/media_dl/CMakeLists.txt": (
        "target_sources(mdl_core PRIVATE src/mdl_hash.c)\n"
        "target_include_directories(mdl_core PUBLIC ${FW_ROOT}/apps/shared/media_dl/inc)\n"
    ),
    "apps/stand_alone/media_dl/CMakeLists.txt": (
        "target_sources(media_dl PRIVATE ${FW_ROOT}/apps/shared/media_dl/src/mdl_hash.c)\n"
    ),
}


def _selftest_must_stay_quiet(
    root: Path, exclusive: dict[str, frozenset[str]], failures: list[str]
) -> None:
    """Prove legal code, legal orchestration and prose produce no finding.

    Args:
        root: Temporary fixture root.
        exclusive: Census the fixtures are written against.
        failures: Accumulator from ``selftest_assert``.
    """
    for rel, text in _QUIET_C.items():
        _, found = scan_targets([_write(root, rel, text)], [], exclusive)
        expect(not found, f"legal source stays quiet: {rel}", failures)
    for rel, text in _QUIET_CMAKE.items():
        _, found = scan_targets([], [_write(root, rel, text)], exclusive)
        expect(not found, f"legal listfile stays quiet: {rel}", failures)


def _selftest_scope(failures: list[str]) -> None:
    """Prove the layer partition: ruled layers in, forms and consumers out."""
    for root in PLATFORM_C_ROOTS:
        layer = layer_for_c(f"{root}mod/src/thing.c")
        expect(
            layer is not None and layer.name == PLATFORM_LAYER_NAME, f"{root} is platform", failures
        )
    shared = layer_for_c("apps/shared/media_dl/src/core.c")
    expect(
        shared is not None and shared.name == SHARED_LAYER_NAME,
        "apps/shared is the product-shared layer",
        failures,
    )
    for category in FORM_CATEGORIES:
        expect(
            layer_for_c(f"{category}thing/src/main.c") is None,
            f"{category} is a form and consumes freely",
            failures,
        )
    for root in EXEMPT_CONSUMER_ROOTS:
        expect(layer_for_c(f"{root}app/main.c") is None, f"{root} is an exempt consumer", failures)
        expect(
            layer_for_cmake(f"{root}cmake/unit_tests.cmake") is None,
            f"{root} listfiles are exempt consumers",
            failures,
        )
    expect(layer_for_c("libs/third_party/miniz/miniz.c") is None, "vendored SOUP is out", failures)
    expect(
        layer_for_c("tools/foo/build/CMakeFiles/probe.c") is None, "build output is out", failures
    )
    expect(layer_for_cmake("CMakeLists.txt") is not None, "the root listfile is scanned", failures)
    expect(
        layer_for_cmake("cmake/ra8_add_app.cmake") is not None,
        "the shared cmake modules are scanned",
        failures,
    )
    expect(
        "cmake/ra8_add_app.cmake" not in ORCHESTRATION_EXEMPT,
        "the exemption does not cover the shared cmake modules",
        failures,
    )


def _selftest_census(failures: list[str]) -> None:
    """Prove the exclusive-basename census and its ambiguity rule."""
    rels = [
        "apps/shared/media_dl/inc/mdl_cache.h",
        "apps/stand_alone/media_dl/inc/mdl_cli.h",
        "apps/stand_alone/media_dl/inc/shared_name.h",
        "apps/shared/media_dl/inc/shared_name.h",
        "libs/ra8_core/inc/ra8_check.h",
        "apps/shared/media_dl/build/CMakeFiles/generated.h",
    ]
    census = build_exclusive(rels)
    expect(
        census[PLATFORM_LAYER_NAME] == {"mdl_cache.h", "mdl_cli.h", "shared_name.h"},
        "rule 1's census is every apps-exclusive header basename",
        failures,
    )
    expect(
        census[SHARED_LAYER_NAME] == {"mdl_cli.h"},
        "rule 2's census is form-exclusive only -- a shared sibling is not in it",
        failures,
    )
    expect(
        "generated.h" not in census[PLATFORM_LAYER_NAME],
        "build output never enters the census",
        failures,
    )
    expect(
        build_exclusive([r for r in rels if not r.startswith(SHARED_CATEGORY)])[SHARED_LAYER_NAME]
        == {"mdl_cli.h", "shared_name.h"},
        "an EMPTY apps/shared is a legal layout the census still handles",
        failures,
    )
    expect(
        classify_include("apps/x/y.h", (PRODUCTS_ROOT,), frozenset())[0] == KIND_PATH,
        "the literal path rule needs no census",
        failures,
    )
    expect(
        classify_include("shared_name.h", FORM_CATEGORIES, census[SHARED_LAYER_NAME]) is None,
        "an ambiguous bare include is not a finding",
        failures,
    )
    expect(
        classify_include("apps/shared/x.h", FORM_CATEGORIES, frozenset()) is None,
        "shared is not forbidden to itself",
        failures,
    )


_MEASURED_C_COUNTS = {"libs/": 938, "port/": 98, "src/": 16, "tools/": 214}
_MEASURED_CMAKE = 74
_MEASURED_APPS_C = 137
_MEASURED_APPS_HEADERS = 52


def _selftest_floors(failures: list[str]) -> None:
    """Prove every non-vacuity floor bites, and a healthy census does not."""

    def census(**overrides: object) -> Census:
        """Build a Census from the measured tree with one value perturbed."""
        fields = {
            "c_counts": dict(_MEASURED_C_COUNTS),
            "cmake_count": _MEASURED_CMAKE,
            "apps_c_count": _MEASURED_APPS_C,
            "apps_header_count": _MEASURED_APPS_HEADERS,
        }
        fields.update(overrides)
        return Census(**fields)  # type: ignore[arg-type]

    expect(not census_floor_errors(census()), "the measured census clears every floor", failures)
    narrowed = dict(_MEASURED_C_COUNTS)
    narrowed["port/"] = C_ROOT_FILE_FLOORS["port/"] - 1
    expect(
        bool(census_floor_errors(census(c_counts=narrowed))),
        "a narrowed platform root fails",
        failures,
    )
    # Every root exactly ON its floor, so only the aggregate can object.
    expect(
        bool(census_floor_errors(census(c_counts=dict(C_ROOT_FILE_FLOORS)))),
        "the aggregate platform C floor fails",
        failures,
    )
    expect(
        bool(census_floor_errors(census(cmake_count=CMAKE_TOTAL_FILE_FLOOR - 1))),
        "a collapsed platform listfile scan fails",
        failures,
    )
    expect(
        bool(census_floor_errors(census(apps_c_count=APPS_C_FILE_FLOOR - 1))),
        "a collapsed PRODUCTS census fails independently of the platform one",
        failures,
    )
    expect(
        bool(census_floor_errors(census(apps_header_count=APPS_HEADER_FLOOR - 1))),
        "a collapsed products header census fails, so bare-name cannot go vacuous",
        failures,
    )


def selftest() -> int:
    """Prove both rules fire, stay quiet, partition the layers, and are not vacuous.

    Returns:
        0 when every assertion held, 1 otherwise.
    """
    print("check_tier_imports.py --selftest")
    failures: list[str] = []
    exclusive = {
        PLATFORM_LAYER_NAME: frozenset({"mdl_cache.h", "mdl_cli.h", "dl_module.h"}),
        SHARED_LAYER_NAME: frozenset({"mdl_cli.h", "dl_module.h"}),
    }
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _selftest_must_fire(root, exclusive, failures)
        _selftest_must_stay_quiet(root, exclusive, failures)
    _selftest_scope(failures)
    _selftest_census(failures)
    _selftest_floors(failures)
    return report(failures)


def _report(scanned: int, findings: list[Finding]) -> int:
    """Print the zero-baseline verdict and return its exit status.

    Args:
        scanned: Files read.
        findings: Every tier violation.

    Returns:
        0 when clean, 1 when a layer reached into a region above it.
    """
    if not findings:
        print(f"check_tier_imports.py: {scanned} file(s) across the ruled layers, 0 violations.")
        return EXIT_OK
    sys.stderr.write(
        "check_tier_imports.py: tier boundary violated -- the platform must not import "
        "apps/, and apps/shared/ must not import a product form. Move the shared code "
        "down (libs/ or apps/shared/) or invert the dependency:\n"
    )
    for layer, kind, rel, lineno, named, source in findings:
        sys.stderr.write(f"  {rel}:{lineno}: [{layer}/{kind}] {named}\n      {source}\n")
    sys.stderr.write(f"\n{len(findings)} finding(s); baseline is zero.\n")
    return EXIT_VIOLATION


def main(argv: list[str]) -> int:
    """Dispatch the selftest, the full layer sweep, or an explicit scan.

    Args:
        argv: Process argv.

    Returns:
        0 clean, 1 violation, 2 usage or collapsed scan.
    """
    parser = argparse.ArgumentParser(description="tier-import boundary gate")
    parser.add_argument("--all", action="store_true", help="sweep every ruled layer")
    parser.add_argument("--selftest", action="store_true", help="prove the gate both ways")
    parser.add_argument("files", nargs="*", help="explicit files")
    args = parser.parse_args(argv[1:])
    if args.selftest:
        if args.all or args.files:
            parser.error("--selftest accepts no other arguments")
        return selftest()
    if args.all and args.files:
        parser.error("--all accepts no explicit files")
    if not args.all and not args.files:
        parser.error("provide --all or at least one file")
    if args.all:
        c_targets, cmake_targets, exclusive = _working_scope()
    else:
        c_targets, cmake_targets = _explicit_scope(args.files)
        exclusive = build_exclusive(_git_working_tree())
    try:
        scanned, findings = scan_targets(c_targets, cmake_targets, exclusive)
    except OSError as exc:
        sys.stderr.write(f"check_tier_imports.py: FATAL -- {exc}\n")
        return EXIT_VACUOUS
    return _report(scanned, findings)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
