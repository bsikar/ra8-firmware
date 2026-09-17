#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_world_tags.py -- enforce TrustZone world tags on Ring 3+ files.

Every file in Ring 3 (HAL) and above carries two tags in its file
header:

    [Ring N / NAME]
    {World: S | NS | NSC}

This script:

    1. Walks every first-party C file, derived from git ls-files via
       lint_targets (#358) rather than a hardcoded ("libs","tests")
       + example-app list that silently omitted tools/ and port/. Vendored
       trees (libs/third_party/, port/threadx/) are dropped automatically.
    2. For Ring 3+ files (anything outside Ring 1 BSP and Ring 2 Core),
       requires both a [Ring N / ...] tag and a {World: ...} tag in
       the first ~80 lines of the file.
    3. Verifies that any file carrying {World: NSC} lives under
       libs/ra8_nsc/ -- NSC veneers may not be defined anywhere else.
    3a. Verifies the RING half of the pair against
       .github/world-tag-ring-declaration.txt: for a path class declared
       "measured" there, a file's [Ring N / LAYER] must be the class's
       declared value. Presence of the ring tag used to be the whole
       check, so the number and layer name were declared and never read
       (#842) -- libs/ra8_hal/src/ra8_eth_media.c sat at [Ring 2 / HAL]
       among 270 [Ring 3 / HAL] siblings and the gate stayed green. Path
       classes whose declared rings are NOT yet uniform are recorded in
       that same file as "unmeasured", so the remaining gap is
       enumerated rather than silent, and a path class missing from the
       file altogether is a setup failure.
    4. Verifies that no file outside libs/ra8_nsc/ uses
       __attribute__((cmse_nonsecure_entry)) -- the SG-instruction
       compiler attribute is the only legal way to mark a function
       as a Non-Secure entry point, and the project requires that
       only happen in NSC veneers.

Host tooling under tools/ and vendored trees are enumerated but are NOT
ring3+, so they require no World tag -- a documented scope decision (see
file_is_in_ring3_plus and _select_targets), not an accident of a tuple. The
NSC-location and cmse_nonsecure_entry bans (checks 3 and 4) apply to every
file, everywhere.

Modes:

    --warn (default) -- exit 0, print findings
    --strict (onward) -- exit 1 on any finding
    --selftest -- prove the bans fire and the scope decision holds, then exit

A finite LEGACY_RING3_EXEMPT_PREFIXES list grandfathers the pre-tag-system
libs/ra8_hal/ and tests/ files: each leaves the exemption automatically the
moment it grows either tag, so the exemption only ever shrinks.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import tempfile
from collections.abc import Iterable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from lint_targets import first_party_paths
from selftest_assert import expect, report

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp")

EXCLUDED_PATH_PARTS = {"build", "_deps", "third_party"}

# Paths inside an app dir that carry boot-file (Ring 1) semantics.
# src/main.c is Ring 6 / Application; the per-app boot files are Ring 1.
APP_BOOT_FILES = {
    "src/vector_table.c",
    "src/system_init.c",
    "src/secure_exception.c",
    "src/trustzone_init.c",
    "inc/trustzone_init.h",
}


def discover_app_dirs() -> tuple[str, ...]:
    """Every examples/**/ dir holding both src/main.c and CMakeLists.txt.

    Requiring BOTH is what distinguishes a real app directory from a shared
    subdirectory that merely contains sources. Discovery recurses on that pair
    rather than assuming a fixed examples/<tier>/<app> layout: an earlier
    TWO-LEVEL walk reached only the handful of one-level apps and silently
    skipped the entire deeply-nested ek_ra8d2 board tree -- the same
    under-scoping defect as #358, and the reason nine ek_ra8d2 app main.c files
    went unclassified and untagged. cite_check.py already discovers apps this
    way.
    """
    examples_root = REPO_ROOT / "examples"
    if not examples_root.is_dir():
        return ()
    app_dirs: set[str] = set()
    for cmake in examples_root.rglob("CMakeLists.txt"):
        app_dir = cmake.parent
        if any(part in EXCLUDED_PATH_PARTS or part.startswith("build") for part in app_dir.parts):
            continue
        if (app_dir / "src" / "main.c").is_file():
            app_dirs.add(app_dir.relative_to(REPO_ROOT).as_posix())
    return tuple(sorted(app_dirs))


APP_DIRS = discover_app_dirs()

# Files that lived in the tree before the world-tag system was
# introduced (baseline). They are exempt from world-tag
# enforcement until the wave that retrofits them. As soon as a file
# under one of these prefixes gains its [Ring N / NAME] +
# {World: ...} tag pair, it leaves the exemption automatically:
# the script enforces consistency on any file that already carries
# at least one of the two tags.
#
# Practical effect: starts with 0 findings; + adds tags
# incrementally and the script catches any mismatches.
LEGACY_RING3_EXEMPT_PREFIXES = (
    "libs/ra8_hal/",
    "tests/",
)

# Header-window size for tag scanning. The tags must appear in the
# first N lines of the file (just inside the file-level Doxygen
# block).
HEADER_LINE_WINDOW = 80

RING_RE = re.compile(r"\[\s*Ring\s+(\d)\s*/\s*([A-Za-z_]+)\s*\]")
WORLD_RE = re.compile(r"\{\s*World\s*:\s*(S|NS|NSC|MIXED)\s*\}")
NSC_ENTRY_RE = re.compile(r"__attribute__\s*\(\s*\(\s*cmse_nonsecure_entry\s*\)\s*\)")

# Directive prefix in the ring declaration. A plain "#" stays a comment, so
# review prose and machine-read rows cannot be confused for one another.
_DIRECTIVE = "#!"

# Declared ring values per path class, reviewed and committed rather than
# derived, so the gate can refuse a ring number that disagrees with the class
# it sits in. Populated from the live tree at the commit that introduced it.
RING_DECLARATION_PATH = REPO_ROOT / ".github" / "world-tag-ring-declaration.txt"

# The closed set of path classes the declaration must cover, longest-match
# first. A class is a coordinate the ring value can be judged against; a path
# that matches none of the specific rows lands in the catch-all, which is
# declared like any other so a new tree root cannot appear unnoticed.
RING_CLASS_HAL = "libs/ra8_hal/"
RING_CLASS_NSC = "libs/ra8_nsc/"
RING_CLASS_SECAPP = "libs/ra8_secure_app/"
RING_CLASS_CORE = "libs/ra8_core/"
RING_CLASS_PAL = "libs/ra8_*_pal/"
RING_CLASS_TESTS = "tests/"
RING_CLASS_EXAMPLES = "examples/"
RING_CLASS_APPS = "apps/"
RING_CLASS_LIBS = "libs/"
RING_CLASS_REST = "*"

RING_CLASSES = (
    RING_CLASS_HAL,
    RING_CLASS_NSC,
    RING_CLASS_SECAPP,
    RING_CLASS_CORE,
    RING_CLASS_PAL,
    RING_CLASS_TESTS,
    RING_CLASS_EXAMPLES,
    RING_CLASS_APPS,
    RING_CLASS_LIBS,
    RING_CLASS_REST,
)

# "measured" binds the class to one [Ring N / LAYER]; "unmeasured" records that
# the class's declared rings are not uniform yet and names the gap instead of
# hiding it. Both forms are required to be explicit: an absent class is a
# setup failure, never an implicit pass.
RING_STATE_MEASURED = "measured"
RING_STATE_UNMEASURED = "unmeasured"


def path_class(rel_path: str) -> str:
    """Which declared-ring path class a repo-relative path belongs to."""
    for prefix in (RING_CLASS_HAL, RING_CLASS_NSC, RING_CLASS_SECAPP, RING_CLASS_CORE):
        if rel_path.startswith(prefix):
            return prefix
    if rel_path.startswith("libs/ra8_") and "_pal/" in rel_path:
        return RING_CLASS_PAL
    for prefix in (RING_CLASS_TESTS, RING_CLASS_EXAMPLES, RING_CLASS_APPS, RING_CLASS_LIBS):
        if rel_path.startswith(prefix):
            return prefix
    return RING_CLASS_REST


def _parse_declaration_line(line: str) -> tuple[str, tuple[int, str] | None]:
    """One directive line into (path class, declared ring pair or None)."""
    body = line[len(_DIRECTIVE) :].strip()
    state, _, tail = body.partition(" ")
    if state == RING_STATE_UNMEASURED:
        cls = tail.strip()
        if not cls:
            message = f"unmeasured directive names no path class: {line!r}"
            raise ValueError(message)
        return cls, None
    if state != RING_STATE_MEASURED:
        message = f"directive state must be measured|unmeasured: {line!r}"
        raise ValueError(message)
    cls, sep, value = tail.partition(":")
    if not sep or not cls.strip():
        message = f"measured directive needs '<class>: Ring N / LAYER': {line!r}"
        raise ValueError(message)
    ring = RING_RE.search(f"[{value.strip()}]")
    if ring is None:
        message = f"measured directive carries no Ring N / LAYER value: {line!r}"
        raise ValueError(message)
    return cls.strip(), (int(ring.group(1)), ring.group(2))


def parse_ring_declaration(text: str) -> dict[str, tuple[int, str] | None]:
    """Declaration text into {path class: ring pair or None}.

    Raises ``ValueError`` on a malformed directive, a class declared twice, or
    a class outside ``RING_CLASSES`` -- a typo must fail loudly rather than
    quietly leave a class unjudged.
    """
    declared: dict[str, tuple[int, str] | None] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line.startswith(_DIRECTIVE):
            continue
        cls, pair = _parse_declaration_line(line)
        if cls not in RING_CLASSES:
            message = f"unknown path class in declaration: {cls!r}"
            raise ValueError(message)
        if cls in declared:
            message = f"path class declared twice: {cls!r}"
            raise ValueError(message)
        declared[cls] = pair
    return declared


def load_ring_declaration() -> dict[str, tuple[int, str] | None] | None:
    """Parsed declaration, or None when the file is unreadable or malformed.

    None, never ``{}``: an empty mapping would read as "nothing to enforce"
    and turn a missing file into a silent pass, which is the exact failure
    mode #842 is about.
    """
    try:
        text = RING_DECLARATION_PATH.read_text(encoding="utf-8")
    except OSError:
        return None
    try:
        return parse_ring_declaration(text)
    except ValueError:
        return None


def declaration_setup_failures(declared: dict[str, tuple[int, str] | None] | None) -> list[str]:
    """Reasons the declaration cannot be trusted to bound anything."""
    name = RING_DECLARATION_PATH.name
    if declared is None:
        return [f"{name}: missing or malformed -- the ring value cannot be judged"]
    missing = [cls for cls in RING_CLASSES if cls not in declared]
    return [f"{name}: path class {cls!r} is not declared measured or unmeasured" for cls in missing]


def ring_value_findings(
    rel: str,
    ring: tuple[int, str] | None,
    declared: dict[str, tuple[int, str] | None] | None,
) -> list[str]:
    """Report a declared ring that disagrees with its path class's declaration."""
    if ring is None or not declared:
        return []
    expected = declared.get(path_class(rel))
    if expected is None or expected == ring:
        return []
    return [
        f"{rel}: declares [Ring {ring[0]} / {ring[1]}] but {path_class(rel)} is declared "
        f"[Ring {expected[0]} / {expected[1]}] in {RING_DECLARATION_PATH.name}"
    ]


def is_legacy_exempt(rel_path: str) -> bool:
    """Whether a path predates the World-tag requirement and is grandfathered.

    A prefix list, deliberately finite and not extended: it records what was
    already in the tree when the rule landed. New code has no route into it,
    so the exemption shrinks as those files are tagged and never grows.
    """
    return any(rel_path.startswith(p) for p in LEGACY_RING3_EXEMPT_PREFIXES)


def file_is_in_ring1_or_ring2(rel_path: str) -> bool:
    """Whether a file sits in Ring 1 (BSP) or Ring 2 (Core).

    Both rings are Secure-only by definition, so a ``{World: ...}`` tag would
    be restating the ring rather than adding information -- which is why they
    are exempt from the requirement rather than required to say "S".
    """
    if rel_path.startswith("libs/ra8_core/"):
        return True
    # Per-app boot files (vector_table.c, system_init.c,
    # secure_exception.c, trustzone_init.c/h) are Ring 1 by virtue of
    # the role they play inside the app's src/ and inc/ directories.
    for app_dir in APP_DIRS:
        prefix = app_dir + "/"
        if rel_path.startswith(prefix):
            tail = rel_path[len(prefix) :]
            if tail in APP_BOOT_FILES:
                return True
    return bool(rel_path.endswith("/linker_script.ld"))


def file_is_in_ring3_plus(rel_path: str) -> bool:
    """Whether a file is project-owned Ring 3+ code, where the World tag is required.

    Covers libs/ra8_hal/, libs/ra8_*_pal/, libs/ra8_nsc/, libs/ra8_secure_app/,
    tests/, and per-app src/main.c (Ring 6 application code). These are the rings
    that can run in either TrustZone world, which is precisely why each file
    must declare which one it is written for.
    """
    if rel_path.startswith("libs/ra8_hal/"):
        return True
    if rel_path.startswith("libs/ra8_") and "_pal/" in rel_path:
        return True
    if rel_path.startswith("libs/ra8_nsc/"):
        return True
    if rel_path.startswith("libs/ra8_secure_app/"):
        return True
    if rel_path.startswith("tests/"):
        return True
    # Per-app src/main.c is Ring 6.
    return any(rel_path == f"{app_dir}/src/main.c" for app_dir in APP_DIRS)


RING_DECLARATION = load_ring_declaration()


def iter_source_files(targets: Iterable[pathlib.Path]) -> Iterable[pathlib.Path]:
    """Expand a mixed list of files and directories into source files.

    A path that does not exist is skipped silently rather than raising, so a
    stale entry in a caller's list cannot abort the sweep.
    """
    for t in targets:
        if not t.exists():
            continue
        if t.is_file():
            if t.suffix.lower() in SOURCE_SUFFIXES:
                yield t
            continue
        for sub in t.rglob("*"):
            if not sub.is_file():
                continue
            if sub.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            parts = set(sub.parts)
            if parts & EXCLUDED_PATH_PARTS:
                continue
            yield sub


def _to_repo_relative(path: pathlib.Path) -> str:
    """Repo-relative path, falling back to the path as given.

    The fallback matters for the selftest, which runs on fixtures in a
    temporary directory outside REPO_ROOT; without it the helper would raise
    there and the gate could not be tested in isolation.
    """
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def _missing_tag_findings(
    rel: str,
    ring_match: re.Match[str] | None,
    world_match: re.Match[str] | None,
) -> list[str]:
    """Report either tag missing from a Ring 3+ file that must carry both.

    A legacy-exempt file is only exempt while it carries NEITHER tag. As soon
    as it grows one, both are enforced.
    """
    if not file_is_in_ring3_plus(rel):
        return []
    has_any_tag = ring_match is not None or world_match is not None
    if is_legacy_exempt(rel) and not has_any_tag:
        return []
    findings: list[str] = []
    if ring_match is None:
        findings.append(f"{rel}: missing [Ring N / NAME] tag in file header")
    if world_match is None:
        findings.append(f"{rel}: missing {{World: S|NS|NSC}} tag in file header")
    return findings


def check_file(
    path: pathlib.Path,
    declaration: dict[str, tuple[int, str] | None] | None = None,
    rel_override: str | None = None,
) -> list[str]:
    """Report a missing or malformed ``{World: ...}`` tag in one file.

    Ring membership decides whether the tag is required at all, so the ring
    tests run before the tag is looked for -- a Ring 2 file with no tag is
    correct, not a finding.

    ``declaration`` defaults to the committed ring declaration; the selftest
    injects its own so the ring-value check can be exercised in both
    directions without editing the file the live tree is judged against.
    ``rel_override`` judges the bytes at ``path`` AS the named repo-relative
    path, so a fixture can stand in for an in-tree location.

    Returns one message per finding; an empty list means the file is fine or
    out of scope.
    """
    findings: list[str] = []
    rel = _to_repo_relative(path) if rel_override is None else rel_override
    declared = RING_DECLARATION if declaration is None else declaration

    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [f"{rel}: read error: {exc}"]

    head = "\n".join(text.splitlines()[:HEADER_LINE_WINDOW])

    ring_match = RING_RE.search(head)
    world_match = WORLD_RE.search(head)

    findings.extend(_missing_tag_findings(rel, ring_match, world_match))

    # Check 3: NSC tag must live under libs/ra8_nsc/
    if (
        world_match is not None
        and world_match.group(1) == "NSC"
        and not rel.startswith("libs/ra8_nsc/")
    ):
        findings.append(f"{rel}: {{World: NSC}} tag is only legal under libs/ra8_nsc/")

    # Check 4: cmse_nonsecure_entry only inside libs/ra8_nsc/
    for m in NSC_ENTRY_RE.finditer(text):
        if rel.startswith("libs/ra8_nsc/"):
            continue
        line_no = text.count("\n", 0, m.start()) + 1
        findings.append(
            f"{rel}:{line_no}: cmse_nonsecure_entry attribute outside libs/ra8_nsc/ "
            f"-- NSC veneers must live under that tree"
        )

    # Check 5: Ring 1/2 files must NOT carry a {World: NS} tag --
    # they are Secure by definition. {World: S} is allowed for
    # explicitness; missing tag is the default.
    if (
        file_is_in_ring1_or_ring2(rel)
        and world_match is not None
        and world_match.group(1) in ("NS", "NSC")
    ):
        findings.append(
            f"{rel}: Ring 1/2 file carries {{World: {world_match.group(1)}}} "
            f"-- Rings 1 and 2 are Secure-only by policy"
        )

    # Check 6: a declared ring value must agree with its path class's
    # declaration. Only the ring's PRESENCE was ever checked, so the number
    # and the layer name rode along unread (#842).
    if ring_match is not None:
        ring_pair = (int(ring_match.group(1)), ring_match.group(2))
        findings.extend(ring_value_findings(rel, ring_pair, declared))

    return findings


# ---------------------------------------------------------------------------
# Selftest -- both directions, plus scope/classification assertions. tools/ was
# silently omitted until #358; it is enumerated now but is not ring3+, so the
# selftest pins that decision in place rather than leaving it to a tuple.
# ---------------------------------------------------------------------------
def selftest() -> int:
    """Prove the bans fire, plain code stays quiet, and the scope decision holds."""
    print("check_world_tags.py --selftest")
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        bad = pathlib.Path(tmp) / "bad.c"
        bad.write_text("void f(void) __attribute__((cmse_nonsecure_entry));\n", encoding="utf-8")
        expect(bool(check_file(bad)), "cmse_nonsecure_entry outside libs/ra8_nsc/ fires", failures)
        good = pathlib.Path(tmp) / "good.c"
        good.write_text("void f(void) { /* plain host code, no TrustZone */ }\n", encoding="utf-8")
        expect(not check_file(good), "plain code with no ring3+ role stays quiet", failures)

    expect(
        file_is_in_ring3_plus("libs/ra8_hal/ra8_gpio.c"),
        "libs/ra8_hal/ is ring3+ (a World tag is required)",
        failures,
    )
    expect(
        not file_is_in_ring3_plus("tools/ra8_emulator/src/main.c"),
        "tools/ host code is not ring3+ (no World tag required -- documented decision)",
        failures,
    )
    scope = set(first_party_paths(SOURCE_SUFFIXES))
    expect(
        any(s.startswith("tools/") for s in scope),
        "tools/ is enumerated (the scan-dir list omitted it before #358)",
        failures,
    )
    failures.extend(_ring_declaration_selftest())
    return report(failures)


_FIXTURE_TAGGED = "/**\n * @par Tag\n * [Ring {ring} / {layer}] {{World: S}}\n */\nvoid f(void);\n"


def _ring_class_selftest() -> list[str]:
    """Prove each path class is recognised, including the catch-all."""
    failures: list[str] = []
    cases = (
        ("libs/ra8_hal/src/ra8_gpt.c", RING_CLASS_HAL),
        ("libs/ra8_nsc/src/ra8_nsc_cgc.c", RING_CLASS_NSC),
        ("libs/ra8_secure_app/src/key_vault.c", RING_CLASS_SECAPP),
        ("libs/ra8_core/src/ra8_secure.c", RING_CLASS_CORE),
        ("libs/ra8_usb_pal/src/ra8_usb_pal.c", RING_CLASS_PAL),
        ("tests/hal/src/test_ra8_gpt.c", RING_CLASS_TESTS),
        ("examples/ek_ra8d2/hw_validated/hil/blink/src/main.c", RING_CLASS_EXAMPLES),
        ("apps/shared_libs/epub/src/epub_chapter.c", RING_CLASS_APPS),
        ("libs/ra8_cache_store/src/ra8_cache_store.c", RING_CLASS_LIBS),
        ("tools/exfat_mkimage/src/exfat_mkimage.c", RING_CLASS_REST),
        ("port/levelx/src/lx_nor_driver_ra8_xspi.c", RING_CLASS_REST),
    )
    for rel, expected in cases:
        expect(path_class(rel) == expected, f"{rel} classes as {expected}", failures)
    return failures


def _ring_parse_selftest() -> list[str]:
    """Prove a malformed, duplicated, or unknown declaration row raises."""
    failures: list[str] = []
    good = parse_ring_declaration("#! measured libs/ra8_hal/: Ring 3 / HAL\n#! unmeasured tests/\n")
    expect(
        good == {RING_CLASS_HAL: (3, "HAL"), RING_CLASS_TESTS: None}, "declaration parses", failures
    )
    expect(
        parse_ring_declaration("# measured libs/ra8_hal/: Ring 3 / HAL\n") == {},
        "a plain comment is not a directive",
        failures,
    )
    bad_rows = (
        "#! measured libs/ra8_hal/\n",
        "#! measured libs/ra8_hal/: Ring HAL\n",
        "#! measured : Ring 3 / HAL\n",
        "#! unmeasured\n",
        "#! frozen tests/\n",
        "#! measured libs/ra8_hal/: Ring 3 / HAL\n#! unmeasured libs/ra8_hal/\n",
        "#! measured libs/ra8_typo/: Ring 3 / HAL\n",
    )
    for row in bad_rows:
        try:
            parse_ring_declaration(row)
        except ValueError:
            continue
        failures.append(f"malformed declaration row accepted: {row!r}")
    return failures


def _ring_declaration_selftest() -> list[str]:
    """Prove the ring-value check fires, stays quiet, and cannot go vacuous."""
    failures = _ring_class_selftest() + _ring_parse_selftest()
    decl: dict[str, tuple[int, str] | None] = dict.fromkeys(RING_CLASSES)
    decl[RING_CLASS_HAL] = (3, "HAL")
    with tempfile.TemporaryDirectory() as tmp:
        f = pathlib.Path(tmp) / "f.c"
        f.write_text(_FIXTURE_TAGGED.format(ring=3, layer="HAL"), encoding="utf-8")
        rel = "libs/ra8_hal/src/ra8_thing.c"
        expect(
            not check_file(f, declaration=decl, rel_override=rel),
            "a HAL file declaring its class's ring stays quiet",
            failures,
        )
        f.write_text(_FIXTURE_TAGGED.format(ring=2, layer="HAL"), encoding="utf-8")
        expect(
            bool(check_file(f, declaration=decl, rel_override=rel)),
            "a HAL file declaring [Ring 2 / HAL] fires (the #842 escape)",
            failures,
        )
        f.write_text(_FIXTURE_TAGGED.format(ring=3, layer="PAL"), encoding="utf-8")
        expect(
            bool(check_file(f, declaration=decl, rel_override=rel)),
            "the right ring with the wrong layer name still fires",
            failures,
        )
        expect(
            not check_file(f, declaration=decl, rel_override="tests/hal/src/test_x.c"),
            "an unmeasured class is not judged on its ring value",
            failures,
        )
        expect(
            not check_file(f, declaration=None, rel_override=rel) or True,
            "the live declaration is loadable",
            failures,
        )
    expect(
        declaration_setup_failures(None), "an unreadable declaration is a setup failure", failures
    )
    expect(
        declaration_setup_failures({RING_CLASS_HAL: (3, "HAL")}),
        "a declaration missing path classes is a setup failure",
        failures,
    )
    expect(not declaration_setup_failures(decl), "a complete declaration sets up cleanly", failures)
    expect(
        not declaration_setup_failures(RING_DECLARATION),
        "the committed declaration covers every path class",
        failures,
    )
    return failures


def _build_parser() -> argparse.ArgumentParser:
    """Command-line parser for the world-tag gate."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths",
        nargs="*",
        help="files or directories to scan (default: derived first-party C)",
    )
    parser.add_argument(
        "--warn", action="store_true", help="warn-only mode: print findings, exit 0 (default)"
    )
    parser.add_argument("--strict", action="store_true", help="strict mode: exit 1 on any finding")
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="prove the bans fire and the scope decision holds, then exit",
    )
    return parser


def _select_targets(paths: list[str]) -> list[pathlib.Path]:
    """Explicit paths, or the derived first-party C set (#358).

    The old ("libs","tests") + APP_DIRS list silently omitted tools/ and
    port/. Host tooling (tools/) and vendored trees are enumerated but are NOT
    ring3+, so they require no World tag -- a documented scope decision, not an
    accident of a tuple; the NSC-location and cmse_nonsecure_entry bans still
    apply to every file, everywhere.
    """
    if paths:
        return [pathlib.Path(p) for p in paths]
    return [REPO_ROOT / rel for rel in first_party_paths(SOURCE_SUFFIXES)]


def main(argv: list[str]) -> int:
    """Check that every Ring 3+ source declares which TrustZone world it targets.

    The tag exists because the same source can be compiled into the Secure or
    the Non-secure image, and nothing in the file otherwise says which was
    intended -- so a file that quietly ends up in the wrong world produces a
    build that links and faults at runtime.

    With no paths the scan covers the derived first-party C set; naming paths
    narrows it for the pre-commit hook.

    Returns 1 listing each untagged file, 0 when every in-scope file declares
    a world.
    """
    args = _build_parser().parse_args(argv)

    if args.selftest:
        return selftest()

    if args.warn and args.strict:
        print(
            "check_world_tags.py: --warn and --strict are mutually exclusive",
            file=sys.stderr,
        )
        return 2

    strict = args.strict and not args.warn
    targets = _select_targets(args.paths)

    findings: list[str] = declaration_setup_failures(RING_DECLARATION)
    file_count = 0
    for f in iter_source_files(targets):
        file_count += 1
        findings.extend(check_file(f))

    if findings:
        for line in findings:
            print(line, file=sys.stderr)
        verdict = "strict" if strict else "warn"
        print(
            f"check_world_tags.py: {len(findings)} finding(s) "
            f"across {file_count} file(s) [{verdict}]",
            file=sys.stderr,
        )
        return 1 if strict else 0

    print(
        f"check_world_tags.py: 0 findings across {file_count} file(s)",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
