#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""mcdc_compound_ratchet.py -- MC/DC compound-decision ratchet (vs baseline).

WHY THIS EXISTS
---------------
This project targets DO-178C Level B, where MC/DC coverage of every compound
boolean decision is the core evidence. `check_new_compound_has_mcdc.py` is the
detector for that rule and it works -- but it audited nothing in CI.

`scripts/ci/gates/checks.sh` ran only its `--selftest`. The blocking `--range`
invocation existed solely as a commented-out line (added commented, never
enabled), while issue #426 carried a comment asserting it was blocking. So a
newly-added uncovered compound decision passed CI, and the gate meant to stop
exactly that was decorative -- this tree's signature defect.

Turning the `--range` delta scan on was not the fix. A delta scan flags any
decision whose normalized source line is new, so with a backlog this size it
fails on a *reformat* of a pre-existing uncovered decision. That is a cliff,
and a cliff gets bypassed. This ratchet is the mechanism the tree already uses
for a measured debt it intends to burn down rather than grandfather -- it is
the same shape as `tidy_ratchet.py` and `misra_ratchet.py`:

* NEW findings (any per-file-per-function count above the baseline, or any
  file/function bucket absent from the baseline) FAIL.
* Shrinkage PASSES with a notice to re-baseline, which locks the progress in so
  the debt can never quietly grow back.
* Reformatting, renaming a variable inside, or moving an existing uncovered
  decision changes no count, so it passes -- a ratchet, not a cliff.

Closing the debt means the baseline reaching zero rows and being deleted -- not
being regenerated larger. `--update` refuses to grow a bucket for exactly that
reason; a genuine increase has to be justified by a human editing the file,
which leaves a reviewable diff.

BASELINE NORMALISATION -- per-file-per-FUNCTION counts, not raw finding lines.
Raw findings carry line numbers, which churn on every unrelated edit above
them. A `(file, function) -> count` map is invariant under that, still trips
the moment a function gains another uncovered decision, and is keyed on exactly
the granularity an MC/DC citation uses (`path@function`) -- so a baseline row
names the function whose vectors are missing. It is strictly tighter than a
per-file count: covering one decision while adding another elsewhere in the
same file still fails.

The measurement itself is `check_new_compound_has_mcdc.py::audit_tree`, the
same detection primitives the delta modes use, imported rather than re-parsed
from console output. There is therefore exactly one definition of "this
decision lacks MC/DC vectors", and no text seam between detector and gate that
could silently stop matching.

USAGE
    python3 scripts/checks/mcdc_compound_ratchet.py --selftest  # assert it fires
    python3 scripts/checks/mcdc_compound_ratchet.py --check     # the CI gate
    python3 scripts/checks/mcdc_compound_ratchet.py --update    # re-baseline
    python3 scripts/checks/mcdc_compound_ratchet.py --list      # burn-down list

Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_new_compound_has_mcdc import audit_tree, collect_tree_citations

REPO_ROOT = Path(__file__).resolve().parents[2]
BASELINE_FILE = REPO_ROOT / ".github" / "mcdc-compound-baseline.txt"

MAX_DETAIL_LINES = 10
"""Cap on offending buckets echoed before the report truncates."""

BASELINE_COLUMNS = 3
"""Column count of one baseline row: file, function, count."""

MIN_PRODUCTION_FILES = 200
"""Refuse to ratchet a scan that saw implausibly few production files.

The tree holds ~400 production `.c` files under `libs/` `src/` `port/`. A scan
that finds a fraction of that is not looking at this repository -- a wrong
`--root`, a partial checkout, a build snapshot missing a subtree. It would
report FEWER findings, which reads as a burn-down, and `--update` would freeze
that as the accepted state. Half the tree is the failure signature; a genuine
deletion of half the firmware is not a thing that happens quietly.
"""

MIN_CITATIONS = 50
"""...and refuse a scan whose citation index is implausibly small.

Every finding is "a decision whose enclosing function is not cited", so an
empty or truncated `tests/` makes the ENTIRE tree look uncovered. The tree
carries ~240 `path@function` citations today. Below this floor the citation
index, not the code, is what changed, and the resulting count is meaningless in
both directions.
"""


def scan(root: Path) -> tuple[Counter, int, int]:
    """Scan ``root`` and return ``(counts, production_file_count, citation_count)``.

    ``counts`` maps ``(repo-relative file, enclosing function)`` to the number
    of compound decisions in that function with no matching MC/DC vector set.
    The two scalars accompany it so the caller can refuse a scan whose scope
    never got established -- a count is only trustworthy once the thing that
    produced it is known to have looked at the tree.
    """
    files, findings = audit_tree(root)
    counts: Counter = Counter()
    for path, function, _line, _snippet in findings:
        counts[(path, function)] += 1
    return counts, len(files), len(collect_tree_citations(root))


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
    total = sum(counts.values())
    lines = [
        "# MC/DC compound-decision ratchet baseline -- per-file-per-function counts.",
        "# Consumed by scripts/checks/mcdc_compound_ratchet.py --check",
        "# (CI gate: pre-commit-checks).",
        "#",
        "# Each row is a function holding N compound boolean decisions (`&&` / `||`)",
        "# with NO matching `@par MC/DC:` `path@function` citation in any",
        "# tests/test_*.{c,cpp}. These predate enforcement (issue #426). The gate fails",
        "# on any INCREASE, so the debt is frozen and can only be burned down; a",
        "# newly-added uncovered decision raises a count and fails.",
        "#",
        f"# Total at this baseline: {total} uncovered compound decision(s)",
        f"# across {len({p for p, _ in counts})} file(s).",
        "#",
        "# Burn one down: add a `test_mcdc_<decision>` function with N+1 vectors",
        "# to the matching tests/test_<module>.{c,cpp} whose MC/DC block cites",
        "# `path@function`. See docs/MCDC.md for the worked example.",
        "#",
        "# Regenerate after burning findings down:",
        "#   python3 scripts/checks/mcdc_compound_ratchet.py --update",
        "#",
        "# RENAMING a function (or moving a file) retires its row and creates a",
        "# new one, which reads as growth -- so the gate fails and --update",
        "# refuses, by design. Rename the row(s) here BY HAND, keeping the counts",
        "# identical. That is a one-line reviewable diff, and it is deliberately",
        "# not automated: rename detection that guessed wrong would silently",
        "# absorb a genuinely new uncovered decision.",
        "#",
        "# Closing this out means this file reaching zero rows and being",
        "# DELETED -- never regenerated larger.",
        "#",
        "# file<TAB>function<TAB>count",
    ]
    for (path, function), n in sorted(counts.items()):
        if n:
            lines.append(f"{path}\t{function}\t{n}")
    BASELINE_FILE.write_text("\n".join(lines) + "\n", encoding="ascii")


def scope_reason(files: int, citations: int) -> str | None:
    """Return a refusal reason when the scan's scope is not credible, else None.

    Guards BOTH directions of the ratchet: a scan that examined a fragment of
    the tree, or read a fragment of the citation index, produces a number that
    must not be compared against the baseline and must certainly never be
    written as one.
    """
    if files < MIN_PRODUCTION_FILES:
        return (
            f"only {files} production .c file(s) found under libs/ src/ port/, "
            f"below the {MIN_PRODUCTION_FILES} floor.\n"
            "  The scan is not looking at this repository (wrong --root, or a\n"
            "  partial checkout). A partial scan reports FEWER uncovered\n"
            "  decisions, which reads as a burn-down; refusing it is the only\n"
            "  way that cannot be mistaken for progress."
        )
    if citations < MIN_CITATIONS:
        return (
            f"only {citations} MC/DC citation(s) found in tests/test_*.{{c,cpp}}, "
            f"below the {MIN_CITATIONS} floor.\n"
            "  Every finding is 'this function is not cited', so a truncated\n"
            "  citation index makes the whole tree look uncovered. What changed\n"
            "  is the index, not the code, so the count is meaningless."
        )
    return None


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
        print(
            "MC/DC ratchet: NEW uncovered compound decisions above the baseline",
            file=sys.stderr,
        )
        print(file=sys.stderr)
        for (path, function), was, now in grown[:MAX_DETAIL_LINES]:
            print(f"  {path}\n      {function}(): {was} -> {now}", file=sys.stderr)
        if len(grown) > MAX_DETAIL_LINES:
            print(f"  ... and {len(grown) - MAX_DETAIL_LINES} more bucket(s)", file=sys.stderr)
        print(file=sys.stderr)
        print(
            "Every compound boolean decision under libs/ src/ port/ needs MC/DC\n"
            "vectors: add a `test_mcdc_<decision>` function with N+1 vectors to\n"
            "the matching tests/test_<module>.{c,cpp} and cite the decision as\n"
            "`path@function` in its `@par MC/DC:` block (see docs/MCDC.md).\n"
            "\n"
            "The baseline is a burn-down of debt that predates enforcement, not\n"
            "a place to record new debt. Do not --update to make this pass.",
            file=sys.stderr,
        )
        return 1

    print(
        f"MC/DC ratchet: {total_now} uncovered compound decision(s), "
        f"baseline {total_was} -- no growth."
    )
    if total_now < total_was:
        burned = total_was - total_now
        print(f"  {burned} decision(s) burned down. Re-baseline to lock it in:")
        print("    python3 scripts/checks/mcdc_compound_ratchet.py --update")
    return 0


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

_ST_COVERED_C = """\
ra8_err_t ra8_covered_fn(int c, int d)
{
  if (c && d) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_UNCOVERED_C = """\
ra8_err_t ra8_uncovered_fn(int a, int b)
{
  if (a || b) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_SOUP_C = """\
int soup_fn(int e, int f)
{
  if (e && f) {
    return 1;
  }
  return 0;
}
"""

_ST_TEST_C = """\
/**
 * @test ra8_covered_fn_mcdc
 *
 * @par MC/DC:
 * Decision: `if (c && d)` cites libs/covered.c@ra8_covered_fn
 * - Vector 1: c=1, d=1 -> true
 * - Vector 2: c=0, d=1 -> false (varies c)
 * - Vector 3: c=1, d=0 -> false (varies d)
 */
void test_mcdc_ra8_covered_fn(void) {}
"""

# A second uncovered decision appended to the ALREADY-uncovered function: the
# "new decision lands in a function the baseline already knows about" case,
# which a whole-file-count ratchet would catch but only a count-based one
# catches at all -- a delta scan keyed on new source lines would also fire, and
# both must.
_ST_UNCOVERED_C_GROWN = """\
ra8_err_t ra8_uncovered_fn(int a, int b)
{
  if (a || b) {
    return k_ra8_ok;
  }
  if (a && !b) {
    return k_ra8_err;
  }
  return k_ra8_err;
}
"""


def _st_write(root: Path, rel: str, body: str) -> None:
    """Write ``body`` to ``rel`` under ``root``, creating parent dirs."""
    dst = root / rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(body, encoding="ascii")


def _st_build_tree(root: Path) -> None:
    """Lay down a fixture tree: one covered, one uncovered, one SOUP decision."""
    _st_write(root, "libs/covered.c", _ST_COVERED_C)
    _st_write(root, "libs/uncovered.c", _ST_UNCOVERED_C)
    _st_write(root, "libs/third_party/soup.c", _ST_SOUP_C)
    _st_write(root, "tests/test_covered.c", _ST_TEST_C)


def _selftest_scan(tmp: Path) -> list[str]:
    """Assert the whole-tree measurement counts the right decisions and no others."""
    failures: list[str] = []
    root = tmp / "tree"
    _st_build_tree(root)
    counts, files, cites = scan(root)

    if counts.get(("libs/uncovered.c", "ra8_uncovered_fn")) != 1:
        failures.append("scan() did not count the uncovered decision in libs/uncovered.c")
    if any(path == "libs/covered.c" for path, _fn in counts):
        failures.append("scan() counted libs/covered.c, which carries MC/DC vectors")
    if any("third_party" in path for path, _fn in counts):
        failures.append("scan() counted a libs/third_party/ decision (SOUP is exempt)")
    if sum(counts.values()) != 1:
        failures.append(f"scan() found {sum(counts.values())} decision(s), expected exactly 1")

    # The measurement must SEE the growth it is meant to gate on.
    _st_write(root, "libs/uncovered.c", _ST_UNCOVERED_C_GROWN)
    grown, _files, _cites = scan(root)
    if grown.get(("libs/uncovered.c", "ra8_uncovered_fn")) != 2:  # noqa: PLR2004 -- fixture count
        failures.append("scan() did not see a second uncovered decision added to a known function")

    # Scope scalars must be real, or the guards below have nothing to guard.
    if files < 2 or cites < 1:  # noqa: PLR2004 -- fixture plants 2 files, 1 citation
        failures.append(
            f"scan() reported an implausible scope: {files} file(s), {cites} citation(s)"
        )
    return failures


def _selftest_ratchet() -> list[str]:
    """Assertions about the growth verdict and baseline round-tripping."""
    failures: list[str] = []

    base = Counter({("libs/f.c", "fn_a"): 1})
    # FAILS when a known bucket grows ...
    if report(Counter({("libs/f.c", "fn_a"): 2}), base) == 0:
        failures.append("report() passed a bucket that GREW above the baseline")
    # ... when a new function in a known file appears ...
    if report(Counter({("libs/f.c", "fn_a"): 1, ("libs/f.c", "fn_b"): 1}), base) == 0:
        failures.append("report() passed a NEW function bucket in a baselined file")
    # ... and when a file the baseline never saw appears.
    if report(Counter({("libs/new.c", "fn_a"): 1}), base) == 0:
        failures.append("report() passed a finding in a file absent from the baseline")
    # PASSES when unchanged or shrinking -- the ratchet must not be a cliff.
    if report(Counter({("libs/f.c", "fn_a"): 1}), base) != 0:
        failures.append("report() failed an unchanged bucket")
    if report(Counter(), base) != 0:
        failures.append("report() failed a fully burned-down baseline")

    # Round-trip: what is written must read back identically, or a re-baseline
    # would silently reshape the debt it claims to be freezing.
    original = BASELINE_FILE.read_text(encoding="ascii") if BASELINE_FILE.is_file() else None
    try:
        fixture = Counter({("libs/a.c", "fn_x"): 3, ("src/b.c", "fn_y"): 1})
        write_baseline(fixture)
        if load_baseline() != fixture:
            failures.append("write_baseline()/load_baseline() did not round-trip")
    finally:
        if original is None:
            BASELINE_FILE.unlink(missing_ok=True)
        else:
            BASELINE_FILE.write_text(original, encoding="ascii")

    return failures


def _selftest_scope_guard() -> list[str]:
    """Assertions about the scope guards, in both directions."""
    failures: list[str] = []
    if scope_reason(MIN_PRODUCTION_FILES - 1, MIN_CITATIONS) is None:
        failures.append("scope_reason() accepted a scan that saw too few production files")
    if scope_reason(MIN_PRODUCTION_FILES, MIN_CITATIONS - 1) is None:
        failures.append("scope_reason() accepted a scan with a truncated citation index")
    if scope_reason(MIN_PRODUCTION_FILES, MIN_CITATIONS) is not None:
        failures.append("scope_reason() rejected a scan that is exactly at both floors")
    return failures


def selftest() -> int:
    """Assert the measurement and the ratchet fire, in BOTH directions.

    Runs the REAL scan against a throwaway fixture tree, so a detector that
    quietly stopped matching cannot pass as clean: it must COUNT the uncovered
    decision, must NOT count the vector-covered or SOUP ones, and must see a
    second uncovered decision appear in a function it already knew about.
    """
    with tempfile.TemporaryDirectory() as td:
        failures = _selftest_scan(Path(td))
    failures += _selftest_ratchet() + _selftest_scope_guard()

    if failures:
        print("SELFTEST FAILED:", file=sys.stderr)
        for problem in failures:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print(
        "selftest: MC/DC compound-decision ratchet OK "
        "(counts an uncovered decision; ignores vector-covered and SOUP ones; "
        "fails on growth, passes on shrinkage; refuses an implausible scope)."
    )
    return 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def _list_backlog(root: Path) -> int:
    """Print every uncovered decision, grouped for burn-down work."""
    _files, findings = audit_tree(root)
    for path, function, line, snippet in findings:
        print(f"{path}:{line}: {function}(): {snippet}")
    print(f"total: {len(findings)} uncovered compound decision(s)")
    return 0


def main() -> int:
    """Ratchet uncovered compound decisions against the committed baseline.

    A ratchet, not a floor: ``--check`` fails when a count RISES, and
    ``--update`` lowers the baseline once vectors are written. The baseline can
    therefore only move downward, which is what stops a large legacy count from
    being permanently accepted while still letting CI block a new one today.

    Returns 0 when every count is at or below the baseline, 1 when one grew or
    the scan's scope was not credible.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(REPO_ROOT), help="tree to scan (default: this repo)")
    parser.add_argument("--check", action="store_true", help="gate against the baseline")
    parser.add_argument("--update", action="store_true", help="rewrite the baseline")
    parser.add_argument("--list", action="store_true", help="print the whole backlog")
    parser.add_argument("--selftest", action="store_true", help="assert this gate still fires")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    root = Path(args.root).resolve()
    if args.list:
        return _list_backlog(root)
    if not (args.check or args.update):
        parser.error("one of --check / --update / --list / --selftest is required")

    current, files, citations = scan(root)

    # Refuse to ratchet a scan whose scope never got established, in EITHER
    # direction, before comparing or writing anything.
    broken = scope_reason(files, citations)
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
                "The baseline is a burn-down; write the missing MC/DC vectors instead.",
                file=sys.stderr,
            )
            for path, function in grew[:MAX_DETAIL_LINES]:
                print(f"  {path}  {function}()", file=sys.stderr)
            return 1
        write_baseline(current)
        print(
            f"baseline updated: {sum(current.values())} uncovered compound decision(s) "
            f"recorded across {len({p for p, _ in current})} file(s) "
            f"({files} production file(s) scanned)."
        )
        return 0

    print(
        f"MC/DC ratchet: scanned {files} production file(s), "
        f"{citations} citation(s) in tests/test_*.{{c,cpp}}."
    )
    return report(current, load_baseline())


if __name__ == "__main__":
    sys.exit(main())
