#!/usr/bin/env python3
"""tidy_ratchet.py -- clang-tidy ratchet gate (compare vs committed baseline).

WHY THIS EXISTS
---------------
#369 and #370 brought 435 firmware C files and 12 first-party C++/Objective-C
files into clang-tidy's scope for the first time. They had never been analysed,
so they arrive carrying pre-existing findings. Two bad options were available
and both were rejected:

* leave the new surface OUT of the gate -- which is the status quo the two
  issues exist to end, and the reason nobody knew what was in there;
* switch the offending checks off -- which converts a measured debt into a
  permanent blind spot, and is exactly the "gate that silently does nothing"
  pattern this tree keeps finding.

So the new surface is IN the gate, every finding is counted, and this ratchet
holds the line: it is the same shape as `misra_ratchet.py`, which this tree
already uses for precisely this situation.

* NEW findings (any per-file-per-check count above the baseline, or any
  file/check pair absent from the baseline) FAIL.
* Shrinkage PASSES with a notice to re-baseline, which locks the progress in
  so the debt can never quietly grow back.

Closing the debt means the baseline reaching zero rows and being deleted --
not being regenerated larger. `--update` refuses to grow a bucket for exactly
that reason; a genuine increase has to be justified by a human editing the
file, which leaves a reviewable diff.

BASELINE NORMALISATION -- per-file-per-check COUNTS, not raw finding lines.
Raw clang-tidy findings carry line numbers, which churn on every unrelated edit
above them, and message text, which embeds identifiers. A `(file, check) ->
count` map is invariant under both, still trips the moment a file gains another
violation of a check, and stays small and diffable.

USAGE
    python3 scripts/checks/tidy_ratchet.py --selftest        # assert it fires
    python3 scripts/checks/tidy_ratchet.py --check LOGFILE   # the gate
    python3 scripts/checks/tidy_ratchet.py --update LOGFILE  # re-baseline

Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BASELINE_FILE = REPO_ROOT / ".github" / "tidy-baseline.txt"

MAX_DETAIL_LINES = 10
"""Cap on offending buckets echoed before the report truncates."""

BASELINE_COLUMNS = 3
"""Column count of one baseline row: file, check, count."""

DIAGNOSTIC_ERROR_CHECK = "clang-diagnostic-error"
"""The clang "check" that is really a PARSE failure, not a lint finding."""

MAX_DIAGNOSTIC_ERROR_RATE = 0.20
"""Refuse to ratchet when more than this fraction of findings are parse errors.

A clang-diagnostic-error means clang-tidy could not PARSE a translation unit
(a missing header, an unusable compile command) rather than that it found a
real defect. When those dominate a run, nothing was actually analysed -- the
#387 case, where an unusable arm-none-eabi-gcc stripped the firmware pass of
every system include and turned ~100 TUs into "'string.h' file not found". The
healthy baseline runs at 37 of 415 findings (9%) with the toolchain correct, so
0.20 clears real runs with margin while a mass parse failure (which pushes the
rate past 25%) trips it.
"""

DIAGNOSTIC_ERROR_FLOOR = 60
"""...and only once at least this many parse errors are present, so a tiny run
whose handful of findings happen to be parse errors does not trip. Above the
healthy baseline's 37, below the ~137 a broken firmware pass produces."""

# One clang-tidy diagnostic line:
#   /abs/path/file.c:12:34: warning: text [check-name,-warnings-as-errors]
FINDING_RE = re.compile(
    r"^(?P<path>[^:\s][^:]*):(?P<line>\d+):(?P<col>\d+):\s+"
    r"(?:warning|error):\s+.*\[(?P<checks>[A-Za-z0-9_.,-]+)\]\s*$"
)

# clang-tidy appends this to every check name when WarningsAsErrors is on; it
# is not a check and must not become a baseline key.
NOT_A_CHECK = "-warnings-as-errors"


def parse_log(text: str) -> Counter:
    """Return a {(repo-relative file, check): count} Counter for `text`.

    A finding may list several aliases for one diagnostic
    (`bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp`). The FIRST is
    used as the canonical key so an alias-set reordering between clang-tidy
    versions cannot silently rewrite every baseline row.
    """
    counts: Counter = Counter()
    seen: set[tuple[str, int, int, str]] = set()
    for raw in text.splitlines():
        m = FINDING_RE.match(raw.strip())
        if not m:
            continue
        names = [c for c in m.group("checks").split(",") if c and c != NOT_A_CHECK]
        if not names:
            continue
        check = names[0]
        path = m.group("path")
        try:
            rel = str(Path(path).resolve().relative_to(REPO_ROOT))
        except ValueError:
            rel = path
        # clang-tidy repeats a header diagnostic once per TU that includes it.
        key = (rel, int(m.group("line")), int(m.group("col")), check)
        if key in seen:
            continue
        seen.add(key)
        counts[(rel, check)] += 1
    return counts


def load_baseline() -> Counter:
    """Read the committed baseline into a Counter. Missing file means empty."""
    counts: Counter = Counter()
    if not BASELINE_FILE.is_file():
        return counts
    for raw in BASELINE_FILE.read_text(encoding="ascii").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != BASELINE_COLUMNS:
            continue
        counts[(parts[0], parts[1])] = int(parts[2])
    return counts


def write_baseline(counts: Counter) -> None:
    """Write `counts` out in the committed, sorted, diffable form."""
    lines = [
        "# clang-tidy ratchet baseline -- per-file-per-check finding counts.",
        "# Consumed by scripts/checks/tidy_ratchet.py --check (CI gate: tidy).",
        "#",
        "# These are PRE-EXISTING findings on the surface that #369 and #370",
        "# brought into clang-tidy's scope for the first time. The gate fails on",
        "# any INCREASE, so the debt is frozen and can only be burned down.",
        "#",
        "# Regenerate after burning findings down:",
        "#   bash scripts/checks/clang_tidy.sh --check > /tmp/tidy.log 2>&1",
        "#   python3 scripts/checks/tidy_ratchet.py --update /tmp/tidy.log",
        "#",
        "# Closing this out means this file reaching zero rows and being",
        "# DELETED -- never regenerated larger.",
        "#",
        "# file<TAB>check<TAB>count",
    ]
    for (path, check), n in sorted(counts.items()):
        if n:
            lines.append(f"{path}\t{check}\t{n}")
    BASELINE_FILE.write_text("\n".join(lines) + "\n", encoding="ascii")


def diagnostic_error_reason(current: Counter) -> str | None:
    """Return a refusal reason if the run is dominated by parse errors, else None.

    Guards both directions of the ratchet (#387): a run whose findings are
    mostly clang-diagnostic-error has not analysed its code, it has failed to
    compile it. Writing that as the baseline freezes ~100 files of parse errors
    as the accepted state, and comparing against it hides every real finding in
    those files. So refuse to ``--update`` or ``--check`` such a run and name
    the likely cause instead.

    Trips only when BOTH the absolute count clears ``DIAGNOSTIC_ERROR_FLOOR``
    and the fraction clears ``MAX_DIAGNOSTIC_ERROR_RATE`` -- a healthy run
    carrying a few baselined parse errors must not be mistaken for a broken one.
    """
    total = sum(current.values())
    if total == 0:
        return None
    diag = sum(n for (_path, check), n in current.items() if check == DIAGNOSTIC_ERROR_CHECK)
    rate = diag / total
    if diag < DIAGNOSTIC_ERROR_FLOOR or rate <= MAX_DIAGNOSTIC_ERROR_RATE:
        return None
    return (
        f"{diag} of {total} finding(s) ({rate:.0%}) are {DIAGNOSTIC_ERROR_CHECK} "
        f"-- over the {MAX_DIAGNOSTIC_ERROR_RATE:.0%} ceiling.\n"
        "  clang-tidy could not PARSE most of its input. The usual cause is an\n"
        "  unusable cross-compiler stripping the firmware pass of its system\n"
        "  includes (#387): run the tidy gate with the pinned 13.3\n"
        "  arm-none-eabi-gcc on PATH. A baseline built from a broken parse is\n"
        "  worse than no baseline, so this run is refused for ratcheting."
    )


def report(current: Counter, baseline: Counter) -> int:
    """Compare and print. Returns the process exit code."""
    grown = []
    for key, n in sorted(current.items()):
        was = baseline.get(key, 0)
        if n > was:
            grown.append((key, was, n))

    total_now = sum(current.values())
    total_was = sum(baseline.values())

    if grown:
        print(file=sys.stderr)
        print("clang-tidy ratchet: NEW findings above the committed baseline", file=sys.stderr)
        print(file=sys.stderr)
        for (path, check), was, now in grown[:MAX_DETAIL_LINES]:
            print(f"  {path}\n      {check}: {was} -> {now}", file=sys.stderr)
        if len(grown) > MAX_DETAIL_LINES:
            print(f"  ... and {len(grown) - MAX_DETAIL_LINES} more bucket(s)", file=sys.stderr)
        print(file=sys.stderr)
        print(
            "Fix them. The baseline is a burn-down of debt that predates the\n"
            "scope widening, not a place to record new debt.",
            file=sys.stderr,
        )
        return 1

    print(f"clang-tidy ratchet: {total_now} finding(s), baseline {total_was} -- no growth.")
    if total_now < total_was:
        burned = total_was - total_now
        print(f"  {burned} finding(s) burned down. Re-baseline to lock it in:")
        print("    python3 scripts/checks/tidy_ratchet.py --update <log>")
    return 0


def _selftest_parse() -> list[str]:
    """Assertions about log parsing."""
    failures: list[str] = []

    sample = (
        "/repo/examples/a/main.c:12:3: warning: x is bad "
        "[readability-magic-numbers,-warnings-as-errors]\n"
        "/repo/examples/a/main.c:14:5: error: y is worse "
        "[bugprone-reserved-identifier,cert-dcl37-c]\n"
        "some unrelated build noise\n"
        "  12 | static uint32_t s_pass = 0U;\n"
    )
    counts = parse_log(sample)
    if sum(counts.values()) != 2:  # noqa: PLR2004 -- the fixture plants exactly 2
        failures.append(f"parse_log() found {sum(counts.values())} finding(s), expected 2")
    checks = {check for _, check in counts}
    if "bugprone-reserved-identifier" not in checks:
        failures.append("parse_log() did not canonicalise an alias set to its first name")
    if NOT_A_CHECK in checks:
        failures.append("parse_log() treated -warnings-as-errors as a check name")

    # A repeated header diagnostic (same file/line/col/check) counts ONCE.
    dupe = parse_log(
        "/repo/libs/x.h:9:1: warning: z [misc-x,-warnings-as-errors]\n"
        "/repo/libs/x.h:9:1: warning: z [misc-x,-warnings-as-errors]\n"
    )
    if sum(dupe.values()) != 1:
        failures.append(f"parse_log() counted a duplicated header finding {sum(dupe.values())}x")

    return failures


def _selftest_ratchet() -> list[str]:
    """Assertions about the growth verdict and baseline round-tripping."""
    failures: list[str] = []

    # The gate must FAIL on growth ...
    base = Counter({("f.c", "misc-x"): 1})
    if report(Counter({("f.c", "misc-x"): 2}), base) == 0:
        failures.append("report() passed a bucket that GREW above the baseline")
    # ... and on a bucket the baseline has never seen ...
    if report(Counter({("new.c", "misc-x"): 1}), base) == 0:
        failures.append("report() passed a finding in a file absent from the baseline")
    # ... and PASS when equal or shrinking.
    if report(Counter({("f.c", "misc-x"): 1}), base) != 0:
        failures.append("report() failed an unchanged bucket")
    if report(Counter(), base) != 0:
        failures.append("report() failed a fully burned-down baseline")

    # Round-trip: what is written must read back identically, or a re-baseline
    # would silently reshape the debt it claims to be freezing.
    original = BASELINE_FILE.read_text(encoding="ascii") if BASELINE_FILE.is_file() else None
    try:
        fixture = Counter({("a/b.c", "misc-x"): 3, ("a/c.c", "readability-y"): 1})
        write_baseline(fixture)
        if load_baseline() != fixture:
            failures.append("write_baseline()/load_baseline() did not round-trip")
    finally:
        if original is None:
            BASELINE_FILE.unlink(missing_ok=True)
        else:
            BASELINE_FILE.write_text(original, encoding="ascii")

    return failures


def _selftest_diag_guard() -> list[str]:
    """Assertions about the parse-error guard (#387), in both directions."""
    failures: list[str] = []

    # A run dominated by parse errors must be refused for ratcheting ...
    broken = Counter({(f"examples/tu{i}.c", DIAGNOSTIC_ERROR_CHECK): 1 for i in range(100)})
    if diagnostic_error_reason(broken) is None:
        failures.append("diagnostic_error_reason() accepted a run that is 100% parse errors")

    # ... while a healthy mix carrying a few baselined parse errors is accepted.
    healthy: Counter = Counter(
        {(f"libs/f{i}.c", "readability-magic-numbers"): 1 for i in range(378)}
    )
    healthy.update({(f"examples/m{i}.c", DIAGNOSTIC_ERROR_CHECK): 1 for i in range(37)})
    if diagnostic_error_reason(healthy) is not None:
        failures.append("diagnostic_error_reason() rejected the healthy 37/415 baseline mix")

    # A tiny run whose few findings are all parse errors must not trip (below
    # the absolute floor): the signature is a MASS failure, not a stray error.
    tiny = Counter({(f"x{i}.c", DIAGNOSTIC_ERROR_CHECK): 1 for i in range(5)})
    if diagnostic_error_reason(tiny) is not None:
        failures.append("diagnostic_error_reason() tripped on a handful of parse errors")

    return failures


def selftest() -> int:
    """Assert the parser and the ratchet fire, in BOTH directions."""
    failures = _selftest_parse() + _selftest_ratchet() + _selftest_diag_guard()

    if failures:
        print("SELFTEST FAILED:", file=sys.stderr)
        for problem in failures:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print("selftest: clang-tidy ratchet parse + growth detection OK")
    return 0


def main() -> int:
    """Ratchet clang-tidy findings against the committed baseline.

    A ratchet, not a floor: ``--check`` fails when the count RISES, and
    ``--update`` lowers the baseline once findings are fixed. The baseline can
    therefore only move downward, which is what stops a large legacy count
    from being permanently accepted.

    ``--selftest`` asserts the comparison still fires, so a ratchet that
    stopped detecting growth cannot pass as clean.

    Returns 0 when the count is at or below the baseline, 1 when it grew.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="?", help="clang-tidy output to compare")
    parser.add_argument("--check", action="store_true", help="gate against the baseline")
    parser.add_argument("--update", action="store_true", help="rewrite the baseline")
    parser.add_argument("--selftest", action="store_true", help="assert this gate still fires")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.log:
        parser.error("a clang-tidy log path is required unless --selftest is given")

    text = Path(args.log).read_text(encoding="utf-8", errors="replace")
    current = parse_log(text)

    # Refuse to ratchet a run that failed to parse its input, in EITHER
    # direction, before comparing or writing anything (#387). A baseline built
    # from a broken toolchain would freeze ~100 files of parse errors forever.
    broken = diagnostic_error_reason(current)
    if broken:
        verb = "--update" if args.update else "--check"
        print(f"refusing to {verb}: {broken}", file=sys.stderr)
        return 1

    if args.update:
        baseline = load_baseline()
        # Seeding the very first baseline necessarily "grows" every bucket from
        # nothing, so the no-growth rule applies only once a baseline exists.
        seeding = not BASELINE_FILE.is_file()
        grew = [] if seeding else [k for k, n in current.items() if n > baseline.get(k, 0)]
        if grew:
            print(
                f"refusing to --update: {len(grew)} bucket(s) would GROW. "
                "The baseline is a burn-down; fix the new findings instead.",
                file=sys.stderr,
            )
            for key in grew[:MAX_DETAIL_LINES]:
                print(f"  {key[0]}  {key[1]}", file=sys.stderr)
            return 1
        write_baseline(current)
        print(f"baseline updated: {sum(current.values())} finding(s) recorded")
        return 0

    return report(current, load_baseline())


if __name__ == "__main__":
    sys.exit(main())
