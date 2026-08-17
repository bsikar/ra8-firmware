# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The repository's tier model: what the layers are, and what each may reach.

``check_tier_imports.py`` is the SCANNER -- it lexes C and CMake and reports
what it finds. This module is the MODEL it scans against: where each layer's
files live, which region each layer may not reach into, which populations the
non-vacuity floors are asserted on, and how the exclusive header-basename
census that judges a bare include is derived.

Keeping the two apart is what makes the boundary describable in one place. The
whole layout knowledge is four strings -- ``apps/``, ``apps/shared/`` and the
two form categories -- and two ``Layer`` rows built out of them. A product
moving between categories, or a new category arriving, is a change HERE and
nowhere else; the scanner never learns a path.

The tiers, and the one direction the arrow points:

* PLATFORM -- ``libs/``, ``port/``, ``tools/``. May not reach into
  ``apps/`` at all, in any category.
* PRODUCTS -- ``apps/``. ``apps/shared/`` is the portable product-tier code and
  sits BELOW the form categories ``apps/stand_alone/`` and
  ``apps/threadx_modules/``; it may not reach up into either. The forms consume
  shared, and the platform, freely.
* CONSUMERS -- ``examples/`` and ``tests/``. Outside every rule by design: they
  exist to demonstrate and to compile the other two.

This module holds no ``main`` and no ``--selftest`` of its own. It is a model,
not a detector: every predicate and every floor in it is proved in both
directions by ``check_tier_imports.py --selftest``, which is the entry point
the ``tier-imports`` gate drives.
"""

from __future__ import annotations

import re
import sys
from collections.abc import Iterable
from pathlib import Path
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path

REPO_ROOT = Path(__file__).resolve().parents[2]

# The products tier and its FORM categories. These four strings are the whole
# layout knowledge this gate has; everything else is derived from them, so a
# product moving between categories needs no change here.
PRODUCTS_ROOT = "apps/"

SHARED_CATEGORY = "apps/shared/"

FORM_CATEGORIES = ("apps/stand_alone/", "apps/threadx_modules/")

# Consumers of every tier, exempt by design. Named so the exemption is a stated
# part of the rule rather than an absence someone has to notice.
EXEMPT_CONSUMER_ROOTS = ("examples/", "tests/")


class Layer(NamedTuple):
    """One ruled layer: where its files live, and what it may not reach into."""

    name: str
    c_roots: tuple[str, ...]
    cmake_roots: tuple[str, ...]
    cmake_files: tuple[str, ...]
    forbidden: tuple[str, ...]


# The rule set. Order is presentation order only; the layers do not overlap.
#
# The platform's CMake half additionally covers cmake/ -- the shared platform
# CMake modules, which are platform infrastructure even though they compile
# nothing themselves -- and the root listfile, the tree's top orchestrator.
PLATFORM_LAYER_NAME = "platform"

SHARED_LAYER_NAME = "product-shared"

LAYERS = (
    Layer(
        name=PLATFORM_LAYER_NAME,
        c_roots=("libs/", "port/", "tools/"),
        cmake_roots=("libs/", "port/", "tools/", "cmake/"),
        cmake_files=("CMakeLists.txt",),
        forbidden=(PRODUCTS_ROOT,),
    ),
    Layer(
        name=SHARED_LAYER_NAME,
        c_roots=(SHARED_CATEGORY,),
        cmake_roots=(SHARED_CATEGORY,),
        cmake_files=(),
        forbidden=FORM_CATEGORIES,
    ),
)

PLATFORM_C_ROOTS = LAYERS[0].c_roots

# The exact listfiles permitted to name a forbidden region, and only in an
# add_subdirectory() call. An explicit tuple, never a pattern: a wildcard here
# ("any top-level listfile", "anything called CMakeLists.txt") would re-open the
# boundary the moment the tree grew another orchestrator.
ORCHESTRATION_EXEMPT = frozenset({"CMakeLists.txt"})

C_SUFFIXES = (".c", ".h", ".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx", ".inc", ".m", ".mm")

HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".inc")

LISTFILE_BASENAME = "CMakeLists.txt"

LISTFILE_SUFFIX = ".cmake"

# Vendored SOUP and generated font tables are not hand-authored first-party
# code and are exempt from every house rule, this one included.
EXCLUDED_PREFIXES = ("libs/third_party/", "libs/ra8_fonts/")

# Non-vacuity floors. A checker whose scope collapsed to zero files is also
# perfectly quiet, so a shrunken census is FATAL rather than clean. The
# PLATFORM and PRODUCTS populations are counted SEPARATELY, so either census
# collapsing fails loudly on its own terms rather than being hidden inside a
# total. Measured 2026-08-17 (tracked + untracked-not-ignored, SOUP and fonts
# excluded): libs 948, port 98, tools 214 C-family files; 73 platform
# listfiles; 143 C-family files and 52 headers under apps/. The libs/ figure
# absorbed the 10 files of the dissolved src/ root (#724), so the platform
# total is unchanged at 1260 and the per-root floors below still sum to 1070.
#
# Deliberately NOT floored: apps/shared/ and the individual form categories. A
# product is allowed to live entirely in one of them while another is a
# placeholder, and a floor there would fail the gate for a legal layout.
C_ROOT_FILE_FLOORS = {"libs/": 810, "port/": 80, "tools/": 180}

C_TOTAL_FILE_FLOOR = 1150

CMAKE_TOTAL_FILE_FLOOR = 60

APPS_C_FILE_FLOOR = 100

APPS_HEADER_FLOOR = 20


def normalize_rel(rel: str) -> str:
    """Return `rel` as a forward-slash repo-relative path with no `./` prefix.

    Args:
        rel: A path as it arrives from git, argv, or a selftest fixture.

    Returns:
        The same path in the one spelling every predicate here compares.
    """
    text = rel.replace("\\", "/")
    while text.startswith("./"):
        text = text[2:]
    return text.lstrip("/")


def region_parts(prefix: str) -> tuple[str, ...]:
    """Split a region prefix such as ``apps/stand_alone/`` into components.

    Args:
        prefix: A trailing-slash region prefix.

    Returns:
        Its path components, which is the form the include matcher compares.
    """
    return tuple(part for part in prefix.split("/") if part)


def _cmake_region_re(prefix: str) -> re.Pattern[str]:
    """Build the CMake path matcher for one forbidden region.

    The lookbehind rejects an identifier character, a dot or a dash immediately
    before, so ``myapps/``, ``foo_apps/`` and ``ra8-apps/`` are not this root.
    ``${FW_ROOT}/apps/...`` matches: the character before is a slash.

    Args:
        prefix: A trailing-slash region prefix.

    Returns:
        The compiled pattern.
    """
    return re.compile(r"(?<![A-Za-z0-9_.-])" + re.escape(prefix))


CMAKE_REGION_RES = {
    prefix: _cmake_region_re(prefix) for layer in LAYERS for prefix in layer.forbidden
}


def is_excluded(norm: str) -> bool:
    """Whether a normalized path is vendored, generated, or build output."""
    return norm.startswith(EXCLUDED_PREFIXES) or is_build_output_path(norm)


def layer_for_c(rel: str) -> Layer | None:
    """Return the ruled layer owning C-family file `rel`, or None.

    Args:
        rel: Repo-relative path.

    Returns:
        The owning ``Layer`` for hand-authored C/C++ inside a ruled layer;
        None for the product FORMS, the exempt consumers, vendored SOUP,
        generated font tables, and anything inside a build tree.
    """
    norm = normalize_rel(rel)
    if not norm.lower().endswith(C_SUFFIXES) or is_excluded(norm):
        return None
    return next((layer for layer in LAYERS if norm.startswith(layer.c_roots)), None)


def layer_for_cmake(rel: str) -> Layer | None:
    """Return the ruled layer owning CMake listfile `rel`, or None.

    Args:
        rel: Repo-relative path.

    Returns:
        The owning ``Layer`` for a ``CMakeLists.txt`` / ``*.cmake`` inside a
        ruled layer, or for an exact orchestrator listfile; None otherwise.
    """
    norm = normalize_rel(rel)
    exact = next((layer for layer in LAYERS if norm in layer.cmake_files), None)
    if exact is not None:
        return exact
    is_listfile = norm.endswith(("/" + LISTFILE_BASENAME, LISTFILE_SUFFIX))
    if not is_listfile or is_excluded(norm):
        return None
    return next((layer for layer in LAYERS if norm.startswith(layer.cmake_roots)), None)


def exclusive_basenames(rels: Iterable[str], region: tuple[str, ...]) -> frozenset[str]:
    """Return header basenames that exist inside `region` and nowhere else.

    Args:
        rels: Every repo-relative path in the working tree.
        region: Region prefixes forming the forbidden area.

    Returns:
        The unambiguous basenames -- the only ones a bare include can be judged
        on without guessing at the build's include-directory ordering.
    """
    inside: set[str] = set()
    outside: set[str] = set()
    for rel in rels:
        norm = normalize_rel(rel)
        if not norm.lower().endswith(HEADER_SUFFIXES) or is_build_output_path(norm):
            continue
        name = norm.rsplit("/", 1)[-1]
        if norm.startswith(region):
            inside.add(name)
        else:
            outside.add(name)
    return frozenset(inside - outside)


class Census(NamedTuple):
    """The measured populations every non-vacuity floor is asserted against."""

    c_counts: dict[str, int]
    cmake_count: int
    apps_c_count: int
    apps_header_count: int


def census_floor_errors(census: Census) -> list[str]:
    """Describe every non-vacuity floor the current census fails.

    Args:
        census: The measured platform and products populations.

    Returns:
        One message per violated floor; empty when the scan is honest.
    """
    errors: list[str] = []
    for root, floor in C_ROOT_FILE_FLOORS.items():
        actual = census.c_counts.get(root, 0)
        if actual < floor:
            errors.append(f"{root} enumerated {actual} C-family file(s); floor is {floor}")
    total = sum(census.c_counts.values())
    if total < C_TOTAL_FILE_FLOOR:
        errors.append(f"platform C scope enumerated {total} file(s); floor is {C_TOTAL_FILE_FLOOR}")
    if census.cmake_count < CMAKE_TOTAL_FILE_FLOOR:
        errors.append(
            f"platform CMake scope enumerated {census.cmake_count} listfile(s); "
            f"floor is {CMAKE_TOTAL_FILE_FLOOR}"
        )
    if census.apps_c_count < APPS_C_FILE_FLOOR:
        errors.append(
            f"products tier enumerated {census.apps_c_count} C-family file(s); "
            f"floor is {APPS_C_FILE_FLOOR}"
        )
    if census.apps_header_count < APPS_HEADER_FLOOR:
        errors.append(
            f"products tier enumerated {census.apps_header_count} header(s); "
            f"floor is {APPS_HEADER_FLOOR} -- the bare-name rule would be vacuous"
        )
    return errors


def measure(rels: Iterable[str]) -> Census:
    """Count the platform and products populations the floors are asserted on.

    Args:
        rels: Every repo-relative path in the working tree.

    Returns:
        The measured census.
    """
    c_counts = dict.fromkeys(PLATFORM_C_ROOTS, 0)
    cmake_count = 0
    apps_c_count = 0
    apps_header_count = 0
    for rel in rels:
        norm = normalize_rel(rel)
        if is_excluded(norm):
            continue
        is_c = norm.lower().endswith(C_SUFFIXES)
        if is_c:
            root = next((root for root in PLATFORM_C_ROOTS if norm.startswith(root)), None)
            if root is not None:
                c_counts[root] += 1
        cmake_layer = layer_for_cmake(norm)
        if cmake_layer is not None and cmake_layer.name == PLATFORM_LAYER_NAME:
            cmake_count += 1
        if norm.startswith(PRODUCTS_ROOT):
            apps_c_count += int(is_c)
            apps_header_count += int(norm.lower().endswith(HEADER_SUFFIXES))
    return Census(c_counts, cmake_count, apps_c_count, apps_header_count)


def build_exclusive(rels: Iterable[str]) -> dict[str, frozenset[str]]:
    """Build each layer's exclusive-basename census.

    Args:
        rels: Every repo-relative path in the working tree.

    Returns:
        Layer name -> the basenames a bare include can be judged on.
    """
    materialised = list(rels)
    return {layer.name: exclusive_basenames(materialised, layer.forbidden) for layer in LAYERS}
