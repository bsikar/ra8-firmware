#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Ratchet concrete RA8 driver reach-ins outside HAL and board composition.

The platform-architecture migration (#692) introduces neutral ``fw_if_*``
ports between portable logic and silicon-specific drivers.  Today that seam is
incomplete: first-party production code directly names ``ra8_cgc_*``,
``ra8_glcdc_*``, ``ra8_gpio_*`` and ``ra8_gpt_*`` symbols.  Removing every
reach-in is incremental work under #693, so a zero-debt gate would be a cliff.

This checker freezes the existing debt per ``(file, family)`` in
``.github/agnostic-register-baseline.txt``.  A new or increased bucket fails;
shrinkage passes and asks for a re-baseline.  Counts ignore comments and string
literals, so documentation does not masquerade as a dependency.

Scope is first-party C/C++ production code under ``libs/``, ``src/``,
``port/``, ``examples/`` and ``tools/``.  Concrete HAL implementations, named
backend translation units, and board composition libraries are allowed to name
the concrete symbols.  Tests are outside the production layering policy and
may exercise a concrete driver directly.  Vendored and generated sources
inherit ``lint_targets`` exclusions.

This is invariant 5 from #698.  The prefix-based neutral-code invariants 1 and
2 become enforceable with the final rename (#697).  The no-RTOS-symbol and
architecture-capability invariants belong to #695 and #694 respectively; they
are deliberately not duplicated here.

Usage::

    python3 scripts/checks/check_agnostic_registers.py --selftest
    python3 scripts/checks/check_agnostic_registers.py --check
    python3 scripts/checks/check_agnostic_registers.py --update
    python3 scripts/checks/check_agnostic_registers.py --list
"""

from __future__ import annotations

import argparse
import contextlib
import io
import re
import sys
import tempfile
from collections import Counter
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_magic_numbers import _strip_comments_and_strings
from lint_targets import first_party_paths

REPO_ROOT = Path(__file__).resolve().parents[2]
BASELINE_FILE = REPO_ROOT / ".github" / "agnostic-register-baseline.txt"

SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp", ".cc", ".cxx", ".hh", ".hxx")
POLICY_ROOTS = frozenset({"libs", "src", "port", "examples", "tools"})
EXCLUDED_PREFIXES = ("libs/ra8_hal/", "libs/third_party/", "libs/ra8_fonts/")
EXCLUDED_BACKEND_FILES = frozenset(
    {
        "libs/ra8_display_pal/src/ra8_display_pal_lcd.c",
    }
)

FAMILY_PATTERNS: dict[str, re.Pattern[str]] = {
    family: re.compile(rf"\bra8_{prefix}_[A-Za-z0-9_]+\b")
    for family, prefix in (
        ("clock", "cgc"),
        ("display", "glcdc"),
        ("gpio", "gpio"),
        ("timer", "gpt"),
    )
}

MIN_SCANNED_FILES = 900
"""Reject a live scan that lost a material part of its production scope.

The baseline was seeded with 1,191 eligible files.  A 900-file floor tolerates
real consolidation while catching a missing top-level tree or broken path
filter before the resulting smaller count can be mistaken for burn-down.
"""

MAX_DETAIL_LINES = 25
BASELINE_COLUMNS = 3
SELFTEST_ELIGIBLE_FILES = 2


@dataclass(frozen=True)
class Finding:
    """One concrete-driver symbol found in policy-controlled code."""

    path: str
    line: int
    family: str
    symbol: str


def is_policy_source(rel: str) -> bool:
    """Return whether ``rel`` is production code governed by this ratchet."""
    path = PurePosixPath(rel)
    if not path.parts or path.parts[0] not in POLICY_ROOTS:
        return False
    if (
        not rel.endswith(SOURCE_SUFFIXES)
        or rel.startswith(EXCLUDED_PREFIXES)
        or rel in EXCLUDED_BACKEND_FILES
    ):
        return False
    return not (
        len(path.parts) > 1 and path.parts[0] == "libs" and path.parts[1].startswith("ra8_board_")
    )


def scan_file(path: Path, rel: str) -> list[Finding]:
    """Return concrete-driver references in one source file."""
    text = path.read_text(encoding="utf-8", errors="replace")
    code_lines = _strip_comments_and_strings(text).splitlines()
    findings: list[Finding] = []
    for line_number, line in enumerate(code_lines, 1):
        for family, pattern in FAMILY_PATTERNS.items():
            findings.extend(
                Finding(rel, line_number, family, match.group(0))
                for match in pattern.finditer(line)
            )
    return findings


def scan_files(files: list[Path], root: Path) -> tuple[list[Finding], int]:
    """Scan explicit files and return ``(findings, eligible_file_count)``."""
    findings: list[Finding] = []
    eligible = 0
    for path in files:
        try:
            rel = path.relative_to(root).as_posix()
        except ValueError:
            continue
        if not is_policy_source(rel):
            continue
        eligible += 1
        findings.extend(scan_file(path, rel))
    return findings, eligible


def bucket(findings: list[Finding]) -> Counter[tuple[str, str]]:
    """Reduce findings to stable per-file, per-family counts."""
    return Counter((finding.path, finding.family) for finding in findings)


def parse_baseline(text: str) -> Counter[tuple[str, str]]:
    """Parse baseline text, rejecting malformed or duplicate rows."""
    counts: Counter[tuple[str, str]] = Counter()
    for line_number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != BASELINE_COLUMNS:
            message = f"line {line_number}: expected path<TAB>family<TAB>count"
            raise ValueError(message)
        path, family, count_text = parts
        if family not in FAMILY_PATTERNS:
            message = f"line {line_number}: unknown family {family!r}"
            raise ValueError(message)
        try:
            count = int(count_text)
        except ValueError as exc:
            message = f"line {line_number}: invalid count {count_text!r}"
            raise ValueError(message) from exc
        key = (path, family)
        if count < 1 or key in counts:
            message = f"line {line_number}: non-positive or duplicate bucket"
            raise ValueError(message)
        counts[key] = count
    return counts


def load_baseline() -> Counter[tuple[str, str]]:
    """Load the committed baseline; a missing file means zero allowed debt."""
    if not BASELINE_FILE.is_file():
        return Counter()
    return parse_baseline(BASELINE_FILE.read_text(encoding="ascii"))


def format_baseline(counts: Counter[tuple[str, str]]) -> str:
    """Return stable, reviewable baseline text for ``counts``."""
    totals = Counter()
    for (_path, family), count in counts.items():
        totals[family] += count
    lines = [
        "# Concrete RA8 driver reach-in debt, per (file, peripheral family).",
        "# Consumed by scripts/checks/check_agnostic_registers.py --check",
        "# (CI gate: agnostic-registers; issue #698).",
        "#",
        "# New or increased counts fail. Shrinkage passes and should be locked in",
        "# with --update. This baseline may only shrink; do not add new debt.",
        "# HAL implementations, named backend translation units, board composition",
        "# libraries, tests, vendored code, and generated code are outside this ratchet.",
        "#",
    ]
    lines.extend(f"# {family}: {totals[family]} reference(s)" for family in FAMILY_PATTERNS)
    lines.extend(["#", "# path<TAB>family<TAB>count"])
    lines.extend(
        f"{path}\t{family}\t{count}" for (path, family), count in sorted(counts.items()) if count
    )
    return "\n".join(lines) + "\n"


def write_baseline(counts: Counter[tuple[str, str]]) -> None:
    """Write the baseline in stable ASCII form."""
    BASELINE_FILE.write_text(format_baseline(counts), encoding="ascii")


def regressions(
    actual: Counter[tuple[str, str]], baseline: Counter[tuple[str, str]]
) -> list[tuple[tuple[str, str], int, int]]:
    """Return ``(bucket, allowed, actual)`` rows that grew."""
    return [
        (key, baseline.get(key, 0), count)
        for key, count in sorted(actual.items())
        if count > baseline.get(key, 0)
    ]


def scope_error(files_scanned: int, floor: int = MIN_SCANNED_FILES) -> str | None:
    """Describe a collapsed live scan, or return ``None``."""
    if files_scanned >= floor:
        return None
    return (
        f"only {files_scanned} production file(s) scanned; floor is "
        f"{floor}. A partial scan looks like debt burn-down."
    )


def report_verdict(
    findings: list[Finding],
    actual: Counter[tuple[str, str]],
    baseline: Counter[tuple[str, str]],
) -> int:
    """Compare with the baseline, print diagnostics, and return a gate status."""
    grown = regressions(actual, baseline)
    if grown:
        grown_keys = {key for key, _allowed, _count in grown}
        print("FAIL: concrete RA8 driver reach-ins grew above baseline:", file=sys.stderr)
        for (path, family), allowed, count in grown:
            print(f"  {path}: {family} {allowed} -> {count}", file=sys.stderr)
        print("\nOffending references:", file=sys.stderr)
        offenders = [f for f in findings if (f.path, f.family) in grown_keys]
        for finding in offenders[:MAX_DETAIL_LINES]:
            print(
                f"  {finding.path}:{finding.line}: {finding.symbol} [{finding.family}]",
                file=sys.stderr,
            )
        if len(offenders) > MAX_DETAIL_LINES:
            print(f"  ... and {len(offenders) - MAX_DETAIL_LINES} more", file=sys.stderr)
        print(
            "\nUse or extend a neutral fw_if_* port. The baseline records existing\n"
            "migration debt; it is not an allowance for new reach-ins.",
            file=sys.stderr,
        )
        return 1

    shrunk = sum(1 for key, count in baseline.items() if actual.get(key, 0) < count)
    print(
        f"agnostic-registers: {sum(actual.values())} reference(s) in "
        f"{len(actual)} bucket(s); no growth."
    )
    if shrunk:
        print(
            f"  {shrunk} bucket(s) shrank; run "
            "`python3 scripts/checks/check_agnostic_registers.py --update` to lock it in."
        )
    return 0


def _write_fixture(root: Path, rel: str, text: str) -> Path:
    """Write one self-test fixture and return its path."""
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="ascii")
    return path


def _build_selftest_tree(root: Path) -> list[Path]:
    """Create must-fire, must-stay-quiet, and exemption fixtures."""
    bad = _write_fixture(
        root,
        "examples/demo/main.c",
        "void demo(void)\n{\n"
        "  ra8_cgc_init();\n  ra8_glcdc_start();\n"
        "  ra8_gpio_write();\n  ra8_gpt_init();\n"
        "  // ra8_gpio_write() in a comment is quiet.\n"
        '  const char *s = "ra8_cgc_init";\n}\n',
    )
    clean = _write_fixture(root, "libs/portable/src/logic.c", "void portable_logic(void) {}\n")
    hal = _write_fixture(root, "libs/ra8_hal/src/backend.c", "void ra8_gpio_backend(void) {}\n")
    board = _write_fixture(root, "libs/ra8_board_demo/src/board.c", "void ra8_cgc_board(void) {}\n")
    adapter = _write_fixture(
        root,
        "libs/ra8_display_pal/src/ra8_display_pal_lcd.c",
        "void ra8_glcdc_adapter(void) {}\n",
    )
    test = _write_fixture(root, "tests/test_gpio.c", "void ra8_gpio_test(void) {}\n")
    return [bad, clean, hal, board, adapter, test]


def _selftest_scan(root: Path, files_to_scan: list[Path]) -> list[str]:
    """Prove all four families fire and exemptions stay quiet."""
    findings, files = scan_files(files_to_scan, root)
    failures: list[str] = []
    counts = Counter(finding.family for finding in findings)
    if counts != Counter(dict.fromkeys(FAMILY_PATTERNS, 1)):
        failures.append(f"bad fixture produced {dict(counts)}, expected one of each family")
    if files != SELFTEST_ELIGIBLE_FILES:
        failures.append(
            f"scope counted {files} eligible fixture files, expected {SELFTEST_ELIGIBLE_FILES}"
        )
    if any(finding.path != "examples/demo/main.c" for finding in findings):
        failures.append(
            "an exempt HAL, backend, board, test, comment or string reference was reported"
        )
    return failures


def _selftest_gate(root: Path, files_to_scan: list[Path]) -> list[str]:
    """Drive the same check entry point CI uses, in both directions."""
    failures: list[str] = []
    findings, _files = scan_files(files_to_scan, root)
    accepted = bucket(findings)
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        if check_files(files_to_scan, root, Counter(), SELFTEST_ELIGIBLE_FILES) == 0:
            failures.append("the CI check entry point passed concrete references above zero debt")
        if check_files(files_to_scan, root, accepted, SELFTEST_ELIGIBLE_FILES) != 0:
            failures.append("the CI check entry point failed references at their baseline")
    return failures


def _selftest_ratchet() -> list[str]:
    """Prove growth fails while unchanged and shrinking buckets pass."""
    failures: list[str] = []
    base = Counter({("examples/a.c", "gpio"): 2})
    if regressions(Counter({("examples/a.c", "gpio"): 2}), base):
        failures.append("an unchanged bucket failed")
    if regressions(Counter({("examples/a.c", "gpio"): 1}), base):
        failures.append("a shrinking bucket failed")
    if not regressions(Counter({("examples/a.c", "gpio"): 3}), base):
        failures.append("a growing bucket passed")
    if not regressions(Counter({("examples/new.c", "gpio"): 1}), base):
        failures.append("a new file bucket passed")

    fixture = Counter({("examples/a.c", "clock"): 3, ("src/b.c", "timer"): 1})
    if parse_baseline(format_baseline(fixture)) != fixture:
        failures.append("baseline formatting did not round-trip")
    return failures


def _selftest_floor() -> list[str]:
    """Prove the live-scan floor rejects collapse and accepts its boundary."""
    failures: list[str] = []
    if scope_error(MIN_SCANNED_FILES - 1) is None:
        failures.append("the scope floor accepted too few files")
    if scope_error(MIN_SCANNED_FILES) is not None:
        failures.append("the scope floor rejected its documented boundary")
    return failures


def selftest() -> int:
    """Run must-fire, must-stay-quiet, ratchet and floor assertions."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        files_to_scan = _build_selftest_tree(root)
        failures = _selftest_scan(root, files_to_scan)
        failures.extend(_selftest_gate(root, files_to_scan))
    failures.extend(_selftest_ratchet())
    failures.extend(_selftest_floor())
    if failures:
        print("SELFTEST FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print(
        "selftest: agnostic-registers OK (all families fire; comments, strings, "
        "HAL, backends, boards and tests stay quiet; growth fails; floor holds)."
    )
    return 0


def check_files(
    files: list[Path],
    root: Path,
    baseline: Counter[tuple[str, str]],
    floor: int = MIN_SCANNED_FILES,
) -> int:
    """Run the blocking check over explicit files; this is CI's check path."""
    findings, files_scanned = scan_files(files, root)
    broken = scope_error(files_scanned, floor)
    if broken is not None:
        print(f"ERROR: {broken}", file=sys.stderr)
        return 2
    actual = bucket(findings)
    print(f"agnostic-registers: scanned {files_scanned} production file(s).")
    return report_verdict(findings, actual, baseline)


def update_baseline(actual: Counter[tuple[str, str]], findings: list[Finding]) -> int:
    """Shrink the committed baseline to ``actual`` without permitting growth."""
    try:
        baseline = load_baseline()
    except ValueError as exc:
        print(f"ERROR: malformed baseline: {exc}", file=sys.stderr)
        return 2
    seeding = not BASELINE_FILE.is_file()
    grown = [] if seeding else regressions(actual, baseline)
    if grown:
        print("ERROR: --update refuses to grow the baseline", file=sys.stderr)
        for (path, family), allowed, count in grown:
            print(f"  {path}: {family} {allowed} -> {count}", file=sys.stderr)
        return 1
    write_baseline(actual)
    print(f"baseline updated: {len(findings)} reference(s) across {len(actual)} bucket(s).")
    return 0


def run_scan(mode: str) -> int:
    """Run a live scan in check, update, or list mode."""
    files = [REPO_ROOT / rel for rel in first_party_paths(SOURCE_SUFFIXES)]
    if mode == "check":
        try:
            baseline = load_baseline()
        except ValueError as exc:
            print(f"ERROR: malformed baseline: {exc}", file=sys.stderr)
            return 2
        return check_files(files, REPO_ROOT, baseline)

    findings, files_scanned = scan_files(files, REPO_ROOT)
    broken = scope_error(files_scanned)
    if broken is not None:
        print(f"ERROR: {broken}", file=sys.stderr)
        return 2
    actual = bucket(findings)
    print(f"agnostic-registers: scanned {files_scanned} production file(s).")
    if mode == "list":
        for finding in findings:
            print(f"{finding.path}:{finding.line}: {finding.symbol} [{finding.family}]")
        print(f"total: {len(findings)} concrete-driver reference(s)")
        return 0
    return update_baseline(actual, findings)


def main(argv: list[str] | None = None) -> int:
    """Parse one required mode and return its exit status."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--selftest", action="store_true")
    modes.add_argument("--check", action="store_true")
    modes.add_argument("--update", action="store_true")
    modes.add_argument("--list", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.update:
        return run_scan("update")
    if args.list:
        return run_scan("list")
    return run_scan("check")


if __name__ == "__main__":
    sys.exit(main())
