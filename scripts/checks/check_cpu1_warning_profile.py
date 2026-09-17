#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Classify every Cortex-M33 (CPU1) C translation unit and count the warning escape.

WHY THIS EXISTS
===============
``ra8_add_cpu1_image()`` (``cmake/ra8_add_app.cmake``) applies the canonical
first-party warning profile -- ``-Wall -Wextra -Werror`` ... ``-Wstack-usage``
plus ``-fstack-usage`` -- PER-SOURCE to the helper's own ``SOURCES`` only.  An
app may bolt further translation units onto the same CPU1 executable with its
own ``target_sources()``, and those compile at ``-mcpu=cortex-m33 ... -Os``
with no warning flags at all and no ``.su`` stack data.  That hole was
recorded as a prose ``TODO(T1-09)`` and nothing measured it, so a first-party
M33 source added tomorrow joins the escape silently (#843).

This checker is the measurement half.  It does NOT widen warnings onto
anything: it enumerates the CPU1 sources, classifies each as first-party or
vendored SOUP, and holds the set of first-party translation units OUTSIDE the
profile to an explicit, shrink-only inventory
(``.github/cpu1-warning-profile-baseline.txt``).  A new escape fails the gate.
An inventoried escape that has since been brought into the profile (or
deleted) must be removed from the inventory, so the list can only shrink.

WHAT IT ENFORCES, PRECISELY
---------------------------
  * Every first-party app-added CPU1 translation unit whose source token ends
    in ``.c`` is an inventory row.  A new one that is not in the inventory
    FAILS: extend the profile, or add the row deliberately and say why in
    review.  A token with any other suffix is not judged at all; see SCOPE,
    HONESTLY.
  * An inventory row that no longer escapes -- the source is now a helper
    ``SOURCES`` entry, routed through ``ra8_cpu1_add_first_party_sources()``,
    no longer attached to that image, or gone from disk -- FAILS as stale and
    must be deleted.  Rows are never auto-rewritten.
  * ``ra8_cpu1_add_first_party_sources(<image> <files...>)`` is the way OFF
    this inventory: it bolts the sources on AND applies the same per-source
    profile, so this checker counts them as covered.  Plain
    ``target_sources()`` does not, and never will.
  * Vendored SOUP (``third_party/``, generated ``libs/ra8_fonts/``) must NOT
    appear in the inventory: the inventory is first-party debt, and SOUP is
    deliberately outside the first-party bar (#843 forbids widening blanket
    warnings onto it).
  * A ``target_sources()`` token on a CPU1 image that this checker cannot
    resolve to real paths FAILS, where "cannot resolve" means an unexpanded
    ``${VAR}`` this checker never bound or a path still carrying a ``$``.  A
    parse that stops seeing sources that way must say so rather than report a
    clean tree.  A token dropped for its SUFFIX is a different case and stays
    silent; see SCOPE, HONESTLY.
  * A CPU1 executable an app HAND-ROLLS -- ``add_executable()`` plus a
    ``target_compile_options(... -mcpu=cortex-m33 ...)`` of its own, instead of
    calling ``ra8_add_cpu1_image()`` -- is scanned too, and every first-party
    translation unit on it escapes by construction: no helper means no
    per-source profile on ANY of its sources, ``cpu1_main.c`` included.
    Scanning only the helper's images would have hidden two whole images
    (``cpu1_pingpong``, ``cpu1_pingpong_ipc``) and eight escaping TUs.
  * A missing or unreadable inventory yields the EMPTY set, so every escape
    reports.  The gate fails closed.

NON-VACUITY FLOOR
-----------------
The scan must find at least one ``ra8_add_cpu1_image()`` call, at least one
profile-covered translation unit, and a non-empty inventory.  If the helper is
renamed or the parse collapses, this exits 2 (FATAL) instead of passing.

SCOPE, HONESTLY
---------------
This gate judges CMake INTENT: which sources the profile is attached to.  It
is not a compile-commands audit, so it cannot prove the flags survive to the
compiler; #843's compile-commands and stack-usage-census criteria stay open
and are unticked.  What it does buy is that the escape is now enumerated,
bounded and shrink-only instead of a prose TODO.

The judged set is C ONLY, and that is a hole rather than a convention.
``resolve_source()`` returns the EMPTY list -- not the ``None`` that raises an
unresolved-token finding -- for any source token whose suffix is not ``.c``,
and ``glob_vars()`` keeps only glob words ending in ``.c``, ``.h`` or ``*``.
So a first-party ``.cpp`` / ``.cc`` / ``.S`` / ``.s`` translation unit bolted
onto a CPU1 image with ``target_sources()`` is neither profile-covered, nor an
escape row, nor an unresolved-token finding: it is dropped, and the gate still
prints "none new".  First-party C++ already exists in this tree
(``apps/shared_libs/reflow/v2/src/reflow_v2.cpp``), so the shape is live even
though all 33 inventory rows are ``.c`` today.  Closing the hole means giving
the parser a translation-unit suffix vocabulary and failing on an unknown
suffix; until that lands, this docstring rather than the parser is where the
boundary is written down.

Run with ``--selftest`` to prove both directions, ``--list`` to print the
current classification.

Exit 0 when the escape matches the inventory, 1 on a finding, 2 when the scan
itself collapsed.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
import textwrap
from pathlib import Path

HELPER = "ra8_add_cpu1_image"
# The T1-09 opt-in: an app bolts first-party TUs onto a CPU1 image THROUGH
# this helper, which applies the same per-source profile, so its sources
# count as covered.  Plain target_sources() does not.
FIRST_PARTY_HELPER = "ra8_cpu1_add_first_party_sources"
INVENTORY_REL = ".github/cpu1-warning-profile-baseline.txt"

# The flag that makes a hand-rolled executable a CPU1 image.  An app that skips
# the helper still has to say this, so it is the reliable marker.
M33_FLAG = "-mcpu=cortex-m33"

# add_executable() option keywords, never sources.
EXE_KEYWORDS = ("WIN32", "MACOSX_BUNDLE", "EXCLUDE_FROM_ALL", "IMPORTED", "ALIAS")

# An inventory row is exactly "<cpu1-target> <path>"; anything else is not a row.
ROW_FIELDS = 2

# Sources ra8_add_cpu1_image() appends to every CPU1 image itself; they are
# part of the helper's per-source profile, so they are covered by definition.
HELPER_APPENDED = (
    "libs/ra8_core/src/ra8_freestanding_mem.c",
    "libs/ra8_core/src/ra8_freestanding_str.c",
    "libs/ra8_core/src/ra8_freestanding_math.c",
)

# Vendored / generated trees, spelled the way the rest of the checks spell
# them (see check_asm.py, check_mcdc_floor.py).  SOUP is out of first-party
# scope on purpose: #843 forbids widening blanket warnings onto it.
SOUP_MARKERS = ("third_party/",)
SOUP_PREFIXES = ("libs/ra8_fonts/",)

SEARCH_ROOTS = ("examples", "apps", "tests", "tools", "libs", "src", "port")


def is_soup(rel: str) -> bool:
    """True when ``rel`` is vendored or generated, so outside first-party."""
    if any(marker in rel for marker in SOUP_MARKERS):
        return True
    return rel.startswith(SOUP_PREFIXES)


def call_block(text: str, start: int) -> str:
    """Return the parenthesised argument text of the call opening at ``start``."""
    open_paren = text.find("(", start)
    if open_paren < 0:
        return ""
    depth = 0
    for index in range(open_paren, len(text)):
        char = text[index]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1 : index]
    return ""


def strip_comments(text: str) -> str:
    """Drop ``#`` comments so commentary cannot register as a call or source."""
    out = []
    for line in text.splitlines():
        hash_at = line.find("#")
        out.append(line if hash_at < 0 else line[:hash_at])
    return "\n".join(out)


def parse_keyword_args(block: str, keywords: tuple[str, ...]) -> dict[str, list[str]]:
    """Split a cmake_parse_arguments-style block into keyword -> value tokens."""
    args: dict[str, list[str]] = {key: [] for key in keywords}
    current = None
    for token in block.split():
        if token in keywords:
            current = token
            continue
        if current is not None:
            args[current].append(token)
    return args


def glob_vars(text: str, root: Path) -> dict[str, list[str]]:
    """Resolve ``file(GLOB var CONFIGURE_DEPENDS <pattern>)`` to repo-relative paths."""
    resolved: dict[str, list[str]] = {}
    pattern = re.compile(r"file\s*\(\s*GLOB(?:_RECURSE)?\s+([A-Za-z0-9_]+)([^)]*)\)")
    for match in pattern.finditer(text):
        name, rest = match.group(1), match.group(2)
        hits: list[str] = []
        for word in rest.split():
            if word == "CONFIGURE_DEPENDS" or not word.endswith((".c", ".h", "*")):
                continue
            spec = word.replace("${RA8_REPO_ROOT}/", "").replace("${RA8_ROOT}/", "")
            if spec.startswith("$"):
                continue
            hits += sorted(str(hit.relative_to(root)) for hit in root.glob(spec))
        resolved[name] = hits
    return resolved


def set_vars(text: str) -> dict[str, str]:
    """Resolve single-token ``set(NAME value)`` assignments, e.g. ``CPU1_NAME``."""
    found: dict[str, str] = {}
    for match in re.finditer(r"set\s*\(\s*([A-Za-z0-9_]+)\s+([^\s()]+)\s*\)", text):
        name, value = match.group(1), match.group(2)
        if "$" not in value:
            found[name] = value
    return found


def expand_vars(token: str, setvars: dict[str, str]) -> str:
    """Substitute the ``set()`` variables this checker resolved; leave the rest."""
    return re.sub(r"\$\{([A-Za-z0-9_]+)\}", lambda m: setvars.get(m.group(1), m.group(0)), token)


def m33_targets(text: str, setvars: dict[str, str]) -> set[str]:
    """Targets handed ``-mcpu=cortex-m33`` by an explicit target_compile_options()."""
    targets: set[str] = set()
    for match in re.finditer(r"target_compile_options\s*\(", text):
        tokens = call_block(text, match.start()).split()
        if len(tokens) > 1 and M33_FLAG in tokens[1:]:
            targets.add(expand_vars(tokens[0], setvars))
    return targets


def resolve_source(token: str, app_dir: str, globs: dict[str, list[str]]) -> list[str] | None:
    """Repo-relative paths for one source token, or None when unresolvable."""
    token = token.strip('"')
    if token.startswith("${") and token.endswith("}"):
        name = token[2:-1]
        if name in globs:
            return list(globs[name])
        return None
    expanded = token.replace("${RA8_REPO_ROOT}/", "").replace("${RA8_ROOT}/", "")
    expanded = expanded.replace("${CMAKE_CURRENT_SOURCE_DIR}/", f"{app_dir}/")
    if "$" in expanded:
        return None
    if not expanded.endswith(".c"):
        return []
    if not expanded.startswith(tuple(f"{r}/" for r in SEARCH_ROOTS)):
        expanded = f"{app_dir}/{expanded}"
    return [expanded]


class Image:
    """One CPU1 executable: which sources carry the profile and which escape."""

    def __init__(self, target: str, listfile: str) -> None:
        """Start empty; the listfile parse fills the three source buckets."""
        self.target = target
        self.listfile = listfile
        self.covered: list[str] = []
        self.added: list[str] = []
        self.unresolved: list[str] = []

    def escapes(self) -> list[str]:
        """App-added first-party translation units, i.e. the #843 hole."""
        covered = set(self.covered)
        return sorted({rel for rel in self.added if rel not in covered and not is_soup(rel)})

    def soup(self) -> list[str]:
        """App-added vendored sources, deliberately outside the first-party bar."""
        return sorted({rel for rel in self.added if is_soup(rel)})


def parse_listfile(root: Path, rel: str) -> list[Image]:
    """Every CPU1 image declared in one CMakeLists.txt, with its source split."""
    text = strip_comments((root / rel).read_text(encoding="utf-8", errors="replace"))
    squashed = text.replace(" ", "")
    if f"{HELPER}(" not in squashed and M33_FLAG not in squashed:
        return []
    app_dir = str(Path(rel).parent)
    globs = glob_vars(text, root)
    setvars = set_vars(text)
    images: list[Image] = []
    for match in re.finditer(rf"{HELPER}\s*\(", text):
        block = call_block(text, match.start())
        args = parse_keyword_args(block, ("PARENT", "NAME", "LINKER", "SOURCES", "INCLUDES"))
        name = (args["NAME"] or [f"{(args['PARENT'] or ['unknown'])[0]}_cpu1"])[0]
        image = Image(f"{name}.elf", rel)
        image.covered = list(HELPER_APPENDED)
        for token in args["SOURCES"]:
            hits = resolve_source(token, app_dir, globs)
            if hits is None:
                image.unresolved.append(token)
            else:
                image.covered += hits
        images.append(image)
    images += handrolled_images(text, app_dir, globs, setvars, rel, {i.target for i in images})
    for image in images:
        collect_opt_in(text, app_dir, globs, image)
        collect_added(text, app_dir, globs, image)
    return images


def handrolled_images(
    text: str,
    app_dir: str,
    globs: dict[str, list[str]],
    setvars: dict[str, str],
    rel: str,
    helper_owned: set[str],
) -> list[Image]:
    """CPU1 images an app builds with its own add_executable() instead of the helper.

    Nothing here is profile-covered: the per-source warning set lives in
    ``ra8_add_cpu1_image()``, so an app that hand-rolls the executable gives
    NONE of its translation units the bar, its own ``cpu1_main.c`` included.
    """
    images: list[Image] = []
    m33 = m33_targets(text, setvars)
    for match in re.finditer(r"add_executable\s*\(", text):
        tokens = call_block(text, match.start()).split()
        if not tokens:
            continue
        name = expand_vars(tokens[0], setvars)
        if name in helper_owned:
            continue
        if name not in m33:
            # An unexpanded target name in a listfile that does build for the
            # M33 is a parse this checker cannot vouch for: say so, do not
            # silently drop the image.
            if "$" in name and M33_FLAG in text:
                unknown = Image(name, rel)
                unknown.unresolved.append(tokens[0])
                images.append(unknown)
            continue
        image = Image(name, rel)
        for token in tokens[1:]:
            if token in EXE_KEYWORDS:
                continue
            hits = resolve_source(expand_vars(token, setvars), app_dir, globs)
            if hits is None:
                image.unresolved.append(token)
            else:
                image.added += hits
        images.append(image)
    return images


def collect_opt_in(text: str, app_dir: str, globs: dict[str, list[str]], image: Image) -> None:
    """Attach ``ra8_cpu1_add_first_party_sources()`` tokens to ``image`` as COVERED.

    That helper does the ``target_sources()`` AND puts the per-source warning
    profile on the same files, so a source routed through it is on the bar.
    An unresolvable token still fails: a blind parse must not read as coverage.
    """
    for match in re.finditer(rf"{FIRST_PARTY_HELPER}\s*\(", text):
        block = call_block(text, match.start())
        tokens = block.split()
        if not tokens or tokens[0] != image.target:
            continue
        for token in tokens[1:]:
            hits = resolve_source(token, app_dir, globs)
            if hits is None:
                image.unresolved.append(token)
            else:
                image.covered += hits


def collect_added(text: str, app_dir: str, globs: dict[str, list[str]], image: Image) -> None:
    """Attach the app's own ``target_sources()`` tokens to ``image``."""
    for match in re.finditer(r"target_sources\s*\(", text):
        block = call_block(text, match.start())
        tokens = block.split()
        if not tokens or tokens[0] != image.target:
            continue
        for token in tokens[1:]:
            if token in ("PRIVATE", "PUBLIC", "INTERFACE"):
                continue
            hits = resolve_source(token, app_dir, globs)
            if hits is None:
                image.unresolved.append(token)
            else:
                image.added += hits


def find_images(root: Path) -> list[Image]:
    """Every CPU1 image in the tree, in listfile order."""
    images: list[Image] = []
    for search_root in SEARCH_ROOTS:
        base = root / search_root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("CMakeLists.txt")):
            images += parse_listfile(root, str(path.relative_to(root)))
    return images


def read_inventory(root: Path) -> set[tuple[str, str]]:
    """Inventory rows as (target, path).  Missing or unreadable means EMPTY."""
    path = root / INVENTORY_REL
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return set()
    rows: set[tuple[str, str]] = set()
    for raw in text.splitlines():
        fields = raw.split("#", 1)[0].split()
        if len(fields) != ROW_FIELDS:
            continue
        rows.add((fields[0], fields[1]))
    return rows


def inventory_findings(root: Path, images: list[Image]) -> list[str]:
    """Every disagreement between the measured escape and the inventory."""
    listed = read_inventory(root)
    live: set[tuple[str, str]] = set()
    findings: list[str] = []
    for image in images:
        findings.extend(
            f"{image.listfile}: {image.target} token {unknown} does not resolve; "
            "this checker cannot see what it compiles"
            for unknown in sorted(set(image.unresolved))
        )
        for rel in image.escapes():
            live.add((image.target, rel))
    for target, rel in sorted(live - listed):
        findings.append(
            f"{rel}: first-party CPU1 source on {target} compiles with no "
            f"-Wall/-Wextra/-Werror/-fstack-usage and is not in {INVENTORY_REL}"
        )
    for target, rel in sorted(listed - live):
        reason = "no longer attached to that image, now profile-covered, or deleted"
        findings.append(f"{rel}: stale {INVENTORY_REL} row for {target} ({reason}); remove it")
    for target, rel in sorted(listed):
        if is_soup(rel):
            findings.append(
                f"{rel}: vendored SOUP must not sit in {INVENTORY_REL} (row for {target}); "
                "the inventory is first-party debt and SOUP stays outside the first-party bar"
            )
    return findings


def vacuity_error(root: Path, images: list[Image]) -> str:
    """Non-empty message when the scan itself collapsed."""
    if not images:
        return f"no {HELPER}() call found under {', '.join(SEARCH_ROOTS)}"
    if not any(image.covered for image in images):
        return f"no profile-covered CPU1 source found; {HELPER}() parse collapsed"
    if not read_inventory(root):
        return f"{INVENTORY_REL} is missing, unreadable or empty"
    return ""


def print_listing(images: list[Image]) -> None:
    """Print the classification this gate judges."""
    for image in images:
        print(f"{image.target}  ({image.listfile})")
        for rel in sorted(set(image.covered)):
            print(f"  profile   {rel}")
        for rel in image.escapes():
            print(f"  ESCAPE    {rel}")
        for rel in image.soup():
            print(f"  soup      {rel}")
        for token in sorted(set(image.unresolved)):
            print(f"  UNRESOLVED {token}")


def write_fixture(base: Path, rel: str, text: str) -> None:
    """Write one fixture file, creating parents."""
    path = base / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(text), encoding="utf-8")


APP_REL = "examples/board/demo/CMakeLists.txt"
FIXTURE_LISTFILE = """\
    ra8_add_cpu1_image(
      PARENT demo
      NAME demo_cpu1
      SOURCES src/cpu1_main.c
    )
    if(TARGET demo_cpu1.elf)
      target_sources(
        demo_cpu1.elf
        PRIVATE ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_ipc.c
                ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz/miniz.c
      )
    endif()
    """


def build_fixture(base: Path, inventory: str, listfile: str = FIXTURE_LISTFILE) -> None:
    """A minimal tree with one CPU1 image: one covered, one escape, one SOUP."""
    write_fixture(base, APP_REL, listfile)
    write_fixture(base, "examples/board/demo/src/cpu1_main.c", "int main(void){return 0;}\n")
    write_fixture(base, "libs/ra8_hal/src/ra8_ipc.c", "void ipc(void){}\n")
    write_fixture(base, "apps/shared_libs/third_party/miniz/miniz.c", "void mz(void){}\n")
    for rel in HELPER_APPENDED:
        write_fixture(base, rel, "void freestanding(void){}\n")
    write_fixture(base, INVENTORY_REL, inventory)


HANDROLLED_EXTRA = """\
    set(CPU1_NAME rolled_cpu1)
    add_executable(
      ${CPU1_NAME}.elf
      ${RA8_REPO_ROOT}/libs/ra8_core/src/ra8_log.c
    )
    target_compile_options(${CPU1_NAME}.elf PRIVATE -mcpu=cortex-m33 -Os)
    """

ROLLED_ROW = "rolled_cpu1.elf libs/ra8_core/src/ra8_log.c\n"


def selftest_cases() -> list[tuple[str, str, str, str]]:
    """(name, inventory text, substring the findings must contain, listfile) tuples."""
    ipc_row = "demo_cpu1.elf libs/ra8_hal/src/ra8_ipc.c\n"
    rolled = FIXTURE_LISTFILE + textwrap.dedent(HANDROLLED_EXTRA)
    return [
        ("inventoried escape is quiet", ipc_row, "", FIXTURE_LISTFILE),
        ("unlisted escape fires", "# nothing\n", "not in .github", FIXTURE_LISTFILE),
        ("empty inventory fails closed", "", "missing, unreadable or empty", FIXTURE_LISTFILE),
        (
            "stale row fires",
            ipc_row + "demo_cpu1.elf libs/ra8_core/src/gone.c\n",
            "stale",
            FIXTURE_LISTFILE,
        ),
        (
            "soup row fires",
            ipc_row + "apps/shared_libs/third_party/miniz/miniz.c".join(("demo_cpu1.elf ", "\n")),
            "must not sit in",
            FIXTURE_LISTFILE,
        ),
        ("hand-rolled CPU1 escape fires", ipc_row, "not in .github", rolled),
        ("hand-rolled CPU1 escape inventoried is quiet", ipc_row + ROLLED_ROW, "", rolled),
    ]


# The same fixture with its one first-party app-added source routed through
# the opt-in helper instead of plain target_sources(): the image then has no
# escape left, and the SOUP source still sits outside the first-party bar.
_IPC_SRC = "${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_ipc.c"
OPT_IN_LISTFILE = FIXTURE_LISTFILE.replace(
    f"      target_sources(\n        demo_cpu1.elf\n        PRIVATE {_IPC_SRC}\n",
    "      ra8_cpu1_add_first_party_sources(\n"
    f"        demo_cpu1.elf\n        {_IPC_SRC}\n      )\n"
    "      target_sources(\n        demo_cpu1.elf\n        PRIVATE\n",
)


def opt_in_cases() -> list[tuple[str, str, str]]:
    """(name, inventory, expected substring) for the opt-in coverage route.

    In OPT_IN_LISTFILE the one first-party app-added source is routed through
    ``ra8_cpu1_add_first_party_sources()``, so the image has NO escape left:
    every inventory row is therefore stale, and none of these cases may report
    that source as escaping.
    """
    return [
        (
            "row for an opt-in source is stale",
            "demo_cpu1.elf libs/ra8_hal/src/ra8_ipc.c\n",
            "stale",
        ),
        (
            "row for a helper-appended source is stale",
            "demo_cpu1.elf libs/ra8_core/src/ra8_freestanding_mem.c\n",
            "stale",
        ),
    ]


def run_case(inventory: str, listfile: str = FIXTURE_LISTFILE) -> tuple[str, list[str]]:
    """Build a fixture tree, return (vacuity error, findings)."""
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        build_fixture(base, inventory, listfile)
        images = find_images(base)
        return vacuity_error(base, images), inventory_findings(base, images)


def selftest() -> int:
    """Prove the gate fires and stays quiet; assert against the live tree too."""
    failures: list[str] = []
    for name, inventory, expect, listfile in selftest_cases():
        error, findings = run_case(inventory, listfile)
        blob = " ".join([error, *findings])
        if expect and expect not in blob:
            failures.append(f"{name}: expected {expect!r} in {blob!r}")
        if not expect and blob.strip():
            failures.append(f"{name}: expected silence, got {blob!r}")
    unresolved_listfile = FIXTURE_LISTFILE.replace(
        "${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_ipc.c", "${SOME_APP_VAR}"
    )
    error, findings = run_case(
        "demo_cpu1.elf libs/ra8_hal/src/ra8_ipc.c\n", unresolved_listfile
    )
    if "does not resolve" not in " ".join([error, *findings]):
        failures.append("unresolved token: expected a finding")
    for name, inventory, expect in opt_in_cases():
        error, findings = run_case(inventory, OPT_IN_LISTFILE)
        blob = " ".join([error, *findings])
        if expect not in blob:
            failures.append(f"{name}: expected {expect!r} in {blob!r}")
        if "ra8_ipc.c: first-party CPU1 source" in blob:
            failures.append(f"{name}: opt-in source still counted as an escape: {blob!r}")
    blind = OPT_IN_LISTFILE.replace(_IPC_SRC, "${SOME_APP_VAR}")
    error, findings = run_case("demo_cpu1.elf libs/ra8_hal/src/ra8_ipc.c\n", blind)
    if "does not resolve" not in " ".join([error, *findings]):
        failures.append("opt-in unresolved token: expected a finding, not silent coverage")
    if failures:
        for failure in failures:
            print(f"check_cpu1_warning_profile.py: SELFTEST FAIL -- {failure}", file=sys.stderr)
        return 1
    print(
        "check_cpu1_warning_profile.py: selftest PASS "
        f"({len(selftest_cases()) + len(opt_in_cases()) + 2} cases, both directions)"
    )
    return 0


def main(argv: list[str]) -> int:
    """Judge the tree, or run one of the two diagnostic modes."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true", help="prove both directions")
    parser.add_argument("--list", action="store_true", help="print the classification")
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    args = parser.parse_args(argv[1:])
    if args.selftest:
        return selftest()
    root = Path(args.root)
    images = find_images(root)
    if args.list:
        print_listing(images)
        return 0
    error = vacuity_error(root, images)
    if error:
        print(f"check_cpu1_warning_profile.py: FATAL -- {error}", file=sys.stderr)
        return 2
    findings = inventory_findings(root, images)
    if findings:
        print(f"\n{len(findings)} CPU1 warning-profile finding(s):\n", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        print(
            "\nEvery first-party Cortex-M33 translation unit outside the "
            f"ra8_add_cpu1_image() profile must be an explicit {INVENTORY_REL} row "
            "(#843). Bring the source into the profile, or add the row and justify it "
            "in review. Rows only ever get deleted.",
            file=sys.stderr,
        )
        return 1
    escapes = sum(len(image.escapes()) for image in images)
    print(
        f"check_cpu1_warning_profile.py: {len(images)} CPU1 image(s); "
        f"{escapes} inventoried first-party escape(s), none new."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
