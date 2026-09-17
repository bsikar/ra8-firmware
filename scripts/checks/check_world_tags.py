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

A finite EXACT-PATH inventory (.github/world-tag-legacy-inventory.txt)
grandfathers the pre-tag-system libs/ra8_hal/ and tests/ files. It replaced an
open-ended PREFIX exemption on those two roots, which let a brand-new untagged
file under either of them pass the strict gate while the comment beside it said
new code had no route into the exemption (#842). Membership is by exact path,
so a file that did not exist when the rule landed cannot be exempt; an
inventoried file also leaves the exemption the moment it grows either tag, and
a row whose file is gone or is tagged now is reported as stale. The exemption
therefore only ever shrinks.
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

# Files that lived in the tree before the world-tag system was introduced
# (baseline). They are exempt from world-tag enforcement until the wave that
# retrofits them. As soon as an inventoried file gains its [Ring N / NAME] +
# {World: ...} tag pair, it leaves the exemption automatically: the script
# enforces consistency on any file that already carries at least one tag.
#
# The inventory is an EXACT-PATH list, not a prefix list (#842). Prefixes
# ("libs/ra8_hal/", "tests/") exempted every FUTURE file under those roots too,
# so a brand-new untagged HAL or test file passed the strict gate. An exact
# list cannot grandfather a file that did not exist when the rule landed.
LEGACY_INVENTORY_PATH = REPO_ROOT / ".github" / "world-tag-legacy-inventory.txt"


def load_legacy_inventory(path: pathlib.Path = LEGACY_INVENTORY_PATH) -> frozenset[str]:
    """Read the exact set of repo-relative paths grandfathered out of the tag rule.

    A missing inventory yields an EMPTY set rather than an error, which fails
    CLOSED: every Ring 3+ file is then required to carry its tags. The opposite
    default would turn a deleted or unreadable inventory into a silently
    tag-free tree, which is the failure mode this gate exists to prevent.

    Blank lines and ``#`` comments are ignored so the file can explain itself.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return frozenset()
    entries = set()
    for line in text.splitlines():
        entry = line.strip()
        if entry and not entry.startswith("#"):
            entries.add(entry)
    return frozenset(entries)


LEGACY_RING3_EXEMPT_PATHS = load_legacy_inventory()

# Header-window size for tag scanning. The tags must appear in the
# first N lines of the file (just inside the file-level Doxygen
# block).
HEADER_LINE_WINDOW = 80

RING_RE = re.compile(r"\[\s*Ring\s+(\d)\s*/\s*([A-Za-z_]+)\s*\]")
WORLD_RE = re.compile(r"\{\s*World\s*:\s*(S|NS|NSC|MIXED)\s*\}")
NSC_ENTRY_RE = re.compile(r"__attribute__\s*\(\s*\(\s*cmse_nonsecure_entry\s*\)\s*\)")


def is_legacy_exempt(rel_path: str, inventory: frozenset[str] | None = None) -> bool:
    """Whether a path predates the World-tag requirement and is grandfathered.

    Exact membership of the finite inventory, deliberately not a prefix test:
    the list records what was already in the tree when the rule landed. New
    code has no route into it, so the exemption shrinks as those files are
    tagged and never grows.
    """
    inv = LEGACY_RING3_EXEMPT_PATHS if inventory is None else inventory
    return rel_path in inv


def stale_inventory_entries(inventory: frozenset[str] | None = None) -> list[str]:
    """Report inventory rows that no longer describe an untagged file on disk.

    Two ways a row goes stale: the file was deleted, or it was tagged and so
    left the exemption on its own. Either way the row now grants nothing and
    must come out, which is what keeps the inventory a burn-down list rather
    than a place debt can hide. Reported as findings, so the shrink-only
    ratchet is the gate itself and not a habit.
    """
    inv = LEGACY_RING3_EXEMPT_PATHS if inventory is None else inventory
    findings: list[str] = []
    for rel in sorted(inv):
        path = REPO_ROOT / rel
        if not path.is_file():
            findings.append(
                f"{rel}: stale world-tag legacy inventory entry -- file does not exist; "
                f"remove the row from {LEGACY_INVENTORY_PATH.name}"
            )
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        head = "\n".join(text.splitlines()[:HEADER_LINE_WINDOW])
        if RING_RE.search(head) is not None and WORLD_RE.search(head) is not None:
            findings.append(
                f"{rel}: stale world-tag legacy inventory entry -- file is tagged now; "
                f"remove the row from {LEGACY_INVENTORY_PATH.name}"
            )
    return findings


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


def check_file(path: pathlib.Path, rel_override: str | None = None) -> list[str]:
    """Report a missing or malformed ``{World: ...}`` tag in one file.

    Ring membership decides whether the tag is required at all, so the ring
    tests run before the tag is looked for -- a Ring 2 file with no tag is
    correct, not a finding.

    ``rel_override`` judges the bytes at ``path`` AS the named repo-relative
    path. Only the selftest passes it: a must-fire fixture for a NEW untagged
    file has to sit at a real in-tree location (``libs/ra8_hal/...``) without
    writing one into the working tree, and every scope decision here keys on
    that path.

    Returns one message per finding; an empty list means the file is fine or
    out of scope.
    """
    findings: list[str] = []
    rel = _to_repo_relative(path) if rel_override is None else rel_override

    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [f"{rel}: read error: {exc}"]

    head_lines = text.splitlines()[:HEADER_LINE_WINDOW]
    head = "\n".join(head_lines)

    ring_match = RING_RE.search(head)
    world_match = WORLD_RE.search(head)

    if file_is_in_ring3_plus(rel):
        # A legacy-exempt file is only exempt while it carries
        # NEITHER tag. As soon as it grows one, both are enforced.
        legacy = is_legacy_exempt(rel)
        has_any_tag = ring_match is not None or world_match is not None
        if (not legacy) or has_any_tag:
            if ring_match is None:
                findings.append(f"{rel}: missing [Ring N / NAME] tag in file header")
            if world_match is None:
                findings.append(f"{rel}: missing {{World: S|NS|NSC}} tag in file header")

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
    _selftest_legacy_inventory(failures)
    return report(failures)


TAGGED_FIXTURE = "/* [Ring 3 / HAL] {World: S} */\nvoid f(void) { }\n"
UNTAGGED_FIXTURE = "void f(void) { }\n"
NSC_FIXTURE = "void f(void) __attribute__((cmse_nonsecure_entry));\n"

# The two roots the old prefix exemption covered wholesale, and therefore the
# two roots a brand-new untagged file could hide under.
NEW_FILE_FIXTURES = (
    "libs/ra8_hal/inc/ra8_world_tag_fixture.h",
    "tests/hal/src/test_world_tag_fixture.c",
)


def _judge_as(tmp: pathlib.Path, rel: str, body: str) -> list[str]:
    """Run the real check over ``body`` as if it were the in-tree file ``rel``.

    The fixture is written outside the repository on purpose: a must-fire test
    for a BRAND-NEW untagged file cannot create that file in the tree it is
    guarding, or the sweep two lines later would report it.
    """
    fixture = tmp / "fixture.c"
    fixture.write_text(body, encoding="utf-8")
    return check_file(fixture, rel_override=rel)


def _selftest_new_file_fixtures(tmpdir: pathlib.Path, failures: list[str]) -> None:
    """Must-fire and must-stay-quiet fixtures for a new file under each legacy root."""
    for rel in NEW_FILE_FIXTURES:
        expect(
            bool(_judge_as(tmpdir, rel, UNTAGGED_FIXTURE)),
            f"a newly created untagged file at {rel} fires",
            failures,
        )
        expect(
            not _judge_as(tmpdir, rel, TAGGED_FIXTURE),
            f"a newly created TAGGED file at {rel} stays quiet",
            failures,
        )


def _selftest_legacy_inventory(failures: list[str]) -> None:
    """Prove the exemption is exact: new files fire, inventoried ones stay quiet."""
    inventory = load_legacy_inventory()
    expect(
        bool(inventory),
        "legacy inventory is non-empty (an unreadable list must not read as clean)",
        failures,
    )
    expect(
        not is_legacy_exempt("libs/ra8_hal/inc/ra8_world_tag_fixture.h"),
        "a NEW libs/ra8_hal/ path is not exempt (the prefix exemption was #842)",
        failures,
    )
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = pathlib.Path(tmp)
        _selftest_new_file_fixtures(tmpdir, failures)
        for rel in sorted(inventory)[:1]:
            expect(
                not _judge_as(tmpdir, rel, UNTAGGED_FIXTURE),
                f"an inventoried legacy file ({rel}) keeps its exemption while untagged",
                failures,
            )
            expect(
                bool(_judge_as(tmpdir, rel, NSC_FIXTURE)),
                "the NSC-location ban stays global, inventoried paths included",
                failures,
            )


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

    findings: list[str] = []
    file_count = 0
    for f in iter_source_files(targets):
        file_count += 1
        findings.extend(check_file(f))

    # Full sweep only: a narrowed pre-commit run over three paths has no
    # business ruling on the whole inventory.
    if not args.paths:
        findings.extend(stale_inventory_entries())

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
