#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Prove that EVERY code file in this repository is linted and formatted.

WHY THIS EXISTS
---------------
"Is everything linted?" was, until this gate, answerable only by opening each
checker and reading its scan list by hand. That audit was performed five times
and was wrong five times -- #296, #332, #358, #359, #360. Every one of those
was the same defect in a different checker: a hardcoded root list that stopped
matching the tree, so the checker reported a clean run over a subset while
files outside it sat unchecked for months. A gate that scans nothing reports
success, and success is indistinguishable from having done the work.

This gate answers the question mechanically:

  1. Enumerate every file from ``git ls-files`` -- never a directory list.
     A hardcoded directory list is the exact defect being killed here, so this
     gate must not contain one.
  2. Classify each file by exact name, then extension, then shebang.
  3. Ask each checker, in its own "list what you would scan" mode, which files
     it claims. The gate does NOT restate any checker's scope: a second copy of
     the coverage map is a new instance of the original bug.
  4. Assert every CODE file is claimed by at least one linter and at least one
     formatter.
  5. FAIL on any file whose type has no classification rule at all.

Point 5 is the one that earns the gate its keep. The day someone commits a
``.rs``, a ``.ts`` or a ``.proto``, this goes red and somebody has to decide
how that language is checked -- rather than it entering the tree silently and
being discovered by the sixth hand audit.

LAYOUT AGNOSTICISM IS A DESIGN REQUIREMENT
------------------------------------------
Checkers are located by BASENAME through ``git ls-files``, not by a hardcoded
path. ``scripts/`` was reorganised into subdirectories in #359 and every
checker this gate resolves changed directory; the basename lookup carried that
move without a single edit. A gate that hardcoded full paths would have broken
on it and -- far worse -- could then have been "fixed" by dropping the
provider, silently shrinking coverage. By name, a MOVE is invisible and a
DELETION is loud.

USAGE
-----
    check_lint_coverage.py --selftest    # assert the gate itself still fires
    check_lint_coverage.py               # the real check
    check_lint_coverage.py --matrix      # print the coverage matrix and exit 0
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_coverage_rules import (
    CLASSES,
    EXT_CLASS,
    FORMAT,
    KNOWN_GAPS,
    LINT,
    NAME_CLASS,
    SHEBANG_CLASS,
    GapCtx,
    exemption_reason,
    validate_tables,
)

REPO_ROOT = Path(
    subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],  # noqa: S607 -- trusted: fixed git argv
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
)

# A tree this size cannot legitimately collapse to a handful of files. If the
# enumeration returns less than this, something broke (a bad cwd, a failed git)
# and reporting "all covered" would be a lie. Same trip-wire as check_ruff.py.
FILE_FLOOR = 2000

# How many offending paths to print before truncating the list.
MAX_SHOWN = 40

# A checker basename must resolve to exactly one tracked path; zero means it
# was deleted, more than one means the name is ambiguous. Both are failures.
EXACTLY_ONE = 1

# The selftest fixture below plants exactly three exempt paths.
EXPECTED_FIXTURE_EXEMPT = 3

# An uncovered file yields one pair per role: lint and format.
BOTH_ROLES = 2


# ---------------------------------------------------------------------------
# Provider descriptors
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Provider:
    """A checker, the roles it fills, and how to ask it what it scans.

    ``script`` is a BASENAME resolved through git at run time -- see the module
    docstring on layout agnosticism.
    """

    name: str
    roles: tuple[str, ...]
    classes: tuple[str, ...]
    script: str
    args: tuple[str, ...]
    runner: str = "python3"


PROVIDERS: tuple[Provider, ...] = (
    Provider("clang-tidy", (LINT,), ("c-family",), "clang_tidy.sh", ("--list-files",), "bash"),
    Provider("clang-format", (FORMAT,), ("c-family",), "format_code.sh", ("--list-files",), "bash"),
    Provider("ruff", (LINT, FORMAT), ("python",), "check_ruff.py", ("--list-files",)),
    Provider("shellcheck+shfmt", (LINT, FORMAT), ("shell",), "check_shell.py", ("--list-files",)),
    Provider("cmake-lint+format", (LINT, FORMAT), ("cmake",), "lint_targets.py", ("cmake",)),
    Provider("yamllint+actionlint", (LINT, FORMAT), ("yaml",), "lint_targets.py", ("yaml",)),
    Provider("check_makefiles", (LINT, FORMAT), ("make",), "check_makefiles.py", ("--list-files",)),
    Provider(
        "check_linker_scripts",
        (LINT, FORMAT),
        ("linker-script",),
        "check_linker_scripts.py",
        ("--list-files",),
    ),
    Provider("check_asm", (LINT, FORMAT), ("asm",), "check_asm.py", ("--list-files",)),
    Provider(
        "hadolint+zsh",
        (LINT, FORMAT),
        ("dockerfile", "zsh"),
        "check_devcontainer.py",
        ("--list-files",),
    ),
)


# ---------------------------------------------------------------------------
# Enumeration and classification
# ---------------------------------------------------------------------------
def git_files() -> list[str]:
    """Every tracked or untracked-but-not-ignored path, repo-relative."""
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
        sys.stderr.write("check_lint_coverage.py: FATAL -- `git ls-files` failed\n")
        sys.exit(2)
    return sorted(p for p in proc.stdout.split("\0") if p)


def read_shebang(rel: str) -> str:
    """First line of `rel` if it is a shebang, else the empty string."""
    try:
        with (REPO_ROOT / rel).open("rb") as handle:
            first = handle.readline(200)
    except OSError:
        return ""
    if not first.startswith(b"#!"):
        return ""
    return first.decode("utf-8", errors="replace").strip()


def classify(rel: str) -> str | None:
    """Return the class name for `rel`, or None when nothing claims it.

    Order is exact name, then extension, then shebang. Name beats extension so
    ``CMakeLists.txt`` is cmake rather than text; shebang comes last so it only
    rescues files the tables genuinely miss -- which is how an extensionless
    ``scripts/git/pre-commit`` is recognised as shell.
    """
    name = rel.rsplit("/", 1)[-1]
    if name in NAME_CLASS:
        return NAME_CLASS[name]
    suffix = ""
    if "." in name[1:]:
        suffix = "." + name.rsplit(".", 1)[-1]
    if suffix.lower() in EXT_CLASS:
        return EXT_CLASS[suffix.lower()]
    line = read_shebang(rel)
    if line:
        for token, cls in SHEBANG_CLASS:
            if token in line:
                return cls
    return None


# ---------------------------------------------------------------------------
# Asking the checkers what they scan
# ---------------------------------------------------------------------------
def resolve_script(basename: str, tracked: list[str]) -> str | None:
    """Locate a checker by basename anywhere in the tree. None if absent."""
    hits = [p for p in tracked if p.rsplit("/", 1)[-1] == basename]
    if len(hits) != EXACTLY_ONE:
        return None
    return hits[0]


def provider_files(prov: Provider, tracked: list[str]) -> tuple[set[str], str | None]:
    """Run `prov` in list mode. Returns (files, error). Never swallows failure."""
    path = resolve_script(prov.script, tracked)
    if path is None:
        return set(), f"cannot locate {prov.script} (moved, deleted or ambiguous)"
    runner = shutil.which(prov.runner) or prov.runner
    proc = subprocess.run(  # noqa: S603 -- argv built from the fixed table above
        [runner, path, *prov.args],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        detail = proc.stderr.strip().splitlines()
        tail = detail[-1] if detail else f"exit {proc.returncode}"
        return set(), f"{prov.script} --list-files failed: {tail}"
    return {ln.strip() for ln in proc.stdout.splitlines() if ln.strip()}, None


# ---------------------------------------------------------------------------
# The pure evaluation core -- shared by the real run and by --selftest.
# ---------------------------------------------------------------------------
class Report:
    """Outcome of one evaluation."""

    def __init__(self) -> None:
        self.unclassified: list[str] = []
        self.uncovered: list[tuple[str, str, str]] = []
        self.gap_growth: list[str] = []
        self.gap_sizes: dict[str, int] = {}
        self.counts: dict[str, int] = {}
        self.exempt: int = 0

    @property
    def ok(self) -> bool:
        return not (self.unclassified or self.uncovered or self.gap_growth)


def _read_text(rel: str) -> str:
    """File contents for a gap predicate, empty when unreadable or binary."""
    try:
        return (REPO_ROOT / rel).read_text(errors="replace")
    except OSError:
        return ""


def _bucket_gaps(raw: list[tuple[str, str, str]], report: Report) -> list[tuple[str, str, str]]:
    """Split uncovered pairs into recorded gaps and genuine violations.

    Also runs the ratchet: a gap that has grown past its recorded count is a
    failure, because "carried deliberately while it is closed" and "quietly
    becoming permanent" must not look the same.
    """
    hits: dict[str, set[str]] = {gap.name: set() for gap in KNOWN_GAPS}
    violations: list[tuple[str, str, str]] = []
    text_cache: dict[str, str] = {}
    for rel, cls, role in raw:
        if rel not in text_cache:
            text_cache[rel] = _read_text(rel)
        ctx = GapCtx(rel=rel, cls=cls, text=text_cache[rel])
        for gap in KNOWN_GAPS:
            if gap.match(ctx):
                hits[gap.name].add(rel)
                break
        else:
            violations.append((rel, cls, role))

    for gap in KNOWN_GAPS:
        got = len(hits[gap.name])
        report.gap_sizes[gap.name] = got
        if got > gap.count:
            report.gap_growth.append(
                f"known gap {gap.name!r} ({gap.issue}) grew from {gap.count} to {got} "
                "file(s). Close it -- raising the recorded count needs a stated reason."
            )
    return violations


def evaluate(files: list[str], claimed: dict[str, set[str]]) -> Report:
    """Decide coverage for `files` given each provider's claimed set.

    `claimed` maps provider name -> the set of paths that provider scans. The
    real run fills it from the checkers themselves; --selftest fills it by
    hand, which is what makes both directions assertable without touching the
    working tree.
    """
    report = Report()
    lint_by: dict[str, set[str]] = {}
    fmt_by: dict[str, set[str]] = {}
    for prov in PROVIDERS:
        got = claimed.get(prov.name, set())
        if LINT in prov.roles:
            lint_by[prov.name] = got
        if FORMAT in prov.roles:
            fmt_by[prov.name] = got

    all_lint = set().union(*lint_by.values()) if lint_by else set()
    all_fmt = set().union(*fmt_by.values()) if fmt_by else set()

    raw: list[tuple[str, str, str]] = []
    for rel in files:
        if exemption_reason(rel) is not None:
            report.exempt += 1
            continue
        cls = classify(rel)
        if cls is None:
            report.unclassified.append(rel)
            continue
        report.counts[cls] = report.counts.get(cls, 0) + 1
        if CLASSES[cls].kind != "code":
            continue
        for role, pool in ((LINT, all_lint), (FORMAT, all_fmt)):
            if rel not in pool:
                raw.append((rel, cls, role))

    report.uncovered = _bucket_gaps(raw, report)
    return report


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def print_matrix(report: Report, claimed: dict[str, set[str]]) -> None:
    by_class: dict[str, list[str]] = {}
    for prov in PROVIDERS:
        for cls in prov.classes:
            by_class.setdefault(cls, []).append(prov.name)
    print(f"{'CLASS':<18}{'COUNT':>7}  {'KIND':<6} PROVIDERS")
    print("-" * 78)
    for cls in sorted(report.counts):
        spec = CLASSES[cls]
        provs = ", ".join(by_class.get(cls, [])) or "-- none --"
        n = report.counts[cls]
        print(f"{cls:<18}{n:>7}  {spec.kind:<6} {provs}")
    print("-" * 78)
    total = sum(report.counts.values())
    print(f"{'total classified':<18}{total:>7}")
    print(f"{'exempt':<18}{report.exempt:>7}")
    for prov in PROVIDERS:
        print(f"  scanned by {prov.name:<22} {len(claimed.get(prov.name, set())):>6} file(s)")
    if report.gap_sizes:
        print("\nRECORDED GAPS -- code with no checker, held flat by the ratchet:")
        for gap in KNOWN_GAPS:
            got = report.gap_sizes.get(gap.name, 0)
            print(f"  {gap.name:<28}{got:>4}/{gap.count:<4} {gap.issue}  {gap.reason[:60]}")


def print_failures(report: Report) -> None:
    if report.unclassified:
        print("\nUNCLASSIFIED FILE TYPES -- no rule says how these are checked:", file=sys.stderr)
        for rel in report.unclassified[:MAX_SHOWN]:
            print(f"  {rel}", file=sys.stderr)
        extra = len(report.unclassified) - MAX_SHOWN
        if extra > 0:
            print(f"  ... and {extra} more", file=sys.stderr)
        print(
            "  Add the type to EXT_CLASS/NAME_CLASS in lint_coverage_rules.py and,\n"
            "  if it is code, wire a linter and a formatter for it.",
            file=sys.stderr,
        )
    if report.uncovered:
        print(
            f"\nUNCOVERED CODE FILES -- {len(report.uncovered)} file/role pair(s) "
            "that no checker claims:",
            file=sys.stderr,
        )
        noun = {LINT: "linter", FORMAT: "formatter"}
        for rel, cls, role in report.uncovered[:MAX_SHOWN]:
            print(f"  {rel}  [{cls}] has no {noun[role]}", file=sys.stderr)
        extra = len(report.uncovered) - MAX_SHOWN
        if extra > 0:
            print(f"  ... and {extra} more", file=sys.stderr)
    for msg in report.gap_growth:
        print(f"\nGAP RATCHET: {msg}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Selftest -- both directions, run BEFORE the real check.
# ---------------------------------------------------------------------------
def _fixture() -> tuple[list[str], dict[str, set[str]]]:
    """A miniature repo that is fully covered, used as the quiet baseline."""
    files = [
        "libs/ra8_core/src/ra8_err.c",
        "libs/ra8_core/inc/ra8_err.h",
        "scripts/checks/check_thing.py",  # PATHREF-OK: synthetic fixture
        "scripts/git/pre-commit",
        "CMakeLists.txt",
        "Makefile",
        "examples/app/linker_script.ld",
        "esp32/boot/start.S",
        ".devcontainer/Dockerfile",
        ".devcontainer/zshrc",
        ".github/workflows/firmware.yml",
        "README.md",
        "libs/third_party/miniz/miniz.c",
        "content/library/book.epub",
        "docs/reference/ra8d2-datasheet.pdf",
    ]
    claimed = {
        "clang-tidy": {"libs/ra8_core/src/ra8_err.c", "libs/ra8_core/inc/ra8_err.h"},
        "clang-format": {"libs/ra8_core/src/ra8_err.c", "libs/ra8_core/inc/ra8_err.h"},
        "ruff": {"scripts/checks/check_thing.py"},  # PATHREF-OK: synthetic
        "shellcheck+shfmt": {"scripts/git/pre-commit"},
        "cmake-lint+format": {"CMakeLists.txt"},
        "yamllint+actionlint": {".github/workflows/firmware.yml"},
        "check_makefiles": {"Makefile"},
        "check_linker_scripts": {"examples/app/linker_script.ld"},
        "check_asm": {"esp32/boot/start.S"},
        "hadolint+zsh": {".devcontainer/Dockerfile", ".devcontainer/zshrc"},
    }
    return files, claimed


def _expect(cond: bool, label: str, failures: list[str]) -> None:
    print(f"  [{'ok' if cond else 'FAIL'}] {label}")
    if not cond:
        failures.append(label)


def selftest() -> int:
    print("check_lint_coverage.py --selftest")
    failures: list[str] = []

    problems = validate_tables()
    _expect(not problems, f"classification tables self-consistent ({problems})", failures)

    # --- QUIET direction -------------------------------------------------
    files, claimed = _fixture()
    base = evaluate(files, claimed)
    _expect(base.ok, "a fully-covered tree passes", failures)
    _expect(
        base.exempt == EXPECTED_FIXTURE_EXEMPT,
        f"exempt paths counted, not flagged (got {base.exempt})",
        failures,
    )

    plus = [*files, "libs/ra8_core/src/ra8_new.c"]
    claimed2 = {k: set(v) for k, v in claimed.items()}
    claimed2["clang-tidy"].add("libs/ra8_core/src/ra8_new.c")
    claimed2["clang-format"].add("libs/ra8_core/src/ra8_new.c")
    _expect(
        evaluate(plus, claimed2).ok,
        "a new file of a covered type in a covered dir stays quiet",
        failures,
    )

    # --- MUST-FIRE direction ---------------------------------------------
    rust = evaluate([*files, "tools/agent/src/main.rs"], claimed)
    _expect(
        rust.unclassified == ["tools/agent/src/main.rs"],
        "an unclassified file type (.rs) fires",
        failures,
    )

    orphan = evaluate([*files, "newdir/thing.c"], claimed)
    _expect(
        sorted({r for r, _, _ in orphan.uncovered}) == ["newdir/thing.c"]
        and len(orphan.uncovered) == BOTH_ROLES,
        "a code file in a directory no checker enumerates fires (lint AND format)",
        failures,
    )

    narrowed = {k: set(v) for k, v in claimed.items()}
    narrowed["clang-format"].discard("libs/ra8_core/inc/ra8_err.h")
    drop = evaluate(files, narrowed)
    _expect(
        drop.uncovered == [("libs/ra8_core/inc/ra8_err.h", "c-family", FORMAT)],
        "narrowing a checker's scan list fires on the file that dropped out",
        failures,
    )

    missing_py = {k: set(v) for k, v in claimed.items()}
    missing_py["ruff"] = set()
    _expect(
        len(evaluate(files, missing_py).uncovered) == BOTH_ROLES,
        "a provider that returns nothing fires for both its roles",
        failures,
    )

    # 13 unclaimed .cpp files exceed the recorded 12 of
    # cxx-objc-not-in-tidy-scope (#370); one does not.
    many = [f"libs/ra8_x/src/f{n}.cpp" for n in range(13)]
    grew = evaluate([*files, *many], claimed)
    _expect(bool(grew.gap_growth), "a known gap that grows fires the ratchet", failures)
    _expect(
        not evaluate([*files, many[0]], claimed).gap_growth,
        "a known gap at or under its recorded count stays quiet",
        failures,
    )
    # A .S file is no longer a recorded gap: #371 gave assembly a checker, so an
    # unclaimed one is now a plain violation. This asserts the gap really was
    # closed rather than merely deleted from the table.
    orphan_asm = evaluate([*files, "newdir/boot.S"], claimed)
    _expect(
        sorted({r for r, _, _ in orphan_asm.uncovered}) == ["newdir/boot.S"]
        and not orphan_asm.gap_growth,
        "an unclaimed .S is a violation now, not a recorded gap",
        failures,
    )

    if failures:
        print(f"\nSELFTEST FAILED: {len(failures)} assertion(s)", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    print("selftest: all assertions held (both directions).")
    return 0


# ---------------------------------------------------------------------------
def run_check(show_matrix: bool) -> int:
    tracked = git_files()
    if len(tracked) < FILE_FLOOR:
        sys.stderr.write(
            f"check_lint_coverage.py: FATAL -- only {len(tracked)} file(s) enumerated, "
            f"floor is {FILE_FLOOR}.\n"
            "  A collapsed enumeration reports full coverage because it saw nothing.\n"
        )
        return 2

    claimed: dict[str, set[str]] = {}
    errors: list[str] = []
    for prov in PROVIDERS:
        got, err = provider_files(prov, tracked)
        if err:
            errors.append(f"{prov.name}: {err}")
        claimed[prov.name] = got
    if errors:
        sys.stderr.write("check_lint_coverage.py: FATAL -- provider enumeration failed.\n")
        for err in errors:
            sys.stderr.write(f"  {err}\n")
        sys.stderr.write(
            "  A provider that cannot report its scope leaves coverage unknown;\n"
            "  unknown is a failure, never a pass.\n"
        )
        return 2

    report = evaluate(tracked, claimed)
    if show_matrix or report.ok:
        print_matrix(report, claimed)
    if report.ok:
        held = sum(report.gap_sizes.values())
        if held:
            print(
                f"\ncheck_lint_coverage.py: every code file is linted and formatted, "
                f"except {held} file(s) in the recorded gaps above -- each tracked by "
                "an issue and held flat by the ratchet."
            )
        else:
            print("\ncheck_lint_coverage.py: every code file is linted and formatted.")
        return 0
    print_failures(report)
    return 1


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--selftest", action="store_true", help="assert both directions")
    ap.add_argument("--matrix", action="store_true", help="print the coverage matrix")
    args = ap.parse_args(argv[1:])
    if args.selftest:
        return selftest()
    return run_check(args.matrix)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
