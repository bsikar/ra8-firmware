#!/usr/bin/env python3
"""cite_ratchet.py -- HUM citation-COVERAGE ratchet (vs committed baseline).

WHY THIS EXISTS
---------------
`CLAUDE.md`, `docs/STYLE_GUIDE.md` and `docs/CITATION_POLICY.md` all state the
same MANDATORY rule: every register read/write or access must carry an external
Hardware User's Manual citation immediately above it. The detector for that rule
exists -- `cite_check.py --require-cites` -- and it ran in NO gate (#534).

Both call sites (the `cite-check` gate and `scripts/git/pre-commit`) invoked
`cite_check.py --strict`, which is the cite-VALIDATION pass: it checks that
citations which ALREADY EXIST parse and point at a real chapter and page. An
MMIO write with no citation at all is invisible to it. So the headline half of
the policy -- does an access HAVE a cite? -- was enforced nowhere, and a
reviewer running exactly the command `CLAUDE.md` prescribes would approve an
entirely uncited new driver.

Turning `--require-cites --strict` on wholesale was not available: the tree
carries a measured backlog of 2884 uncited accesses across 254 files. Those
need ACCURATE per-register HUM subsection and page citations, which cannot be
machine-fabricated -- a wrong subsection would pass validation while being
factually false, which is worse than no citation. Two bad options were rejected:

* leave the coverage pass out of every gate -- the status quo #534 exists to
  end, and the reason the backlog's size was unknown until now;
* exclude the directories carrying most of it -- which converts a measured
  debt into a permanent blind spot, the "gate that silently does nothing"
  pattern this tree keeps finding.

So the coverage pass is IN the gate, every finding is counted, and this ratchet
holds the line. It is the same shape as `tidy_ratchet.py` and
`mcdc_compound_ratchet.py`, which this tree already uses for exactly this
situation:

* NEW findings (any per-file count above the baseline, or any file absent from
  the baseline that now has findings) FAIL.
* Shrinkage PASSES with a notice to re-baseline, which locks the progress in so
  the debt can never quietly grow back.
* Reformatting, renaming a local, or moving an existing uncited access changes
  no count, so it passes -- a ratchet, not a cliff.

Closing the debt means the baseline reaching zero rows and being DELETED -- not
being regenerated larger. `--update` refuses to grow a bucket for exactly that
reason; a genuine increase has to be justified by a human editing the file,
which leaves a reviewable diff.

WHAT COUNTS AS AN ACCESS
------------------------
The measurement is `cite_check.find_uncited_accesses`, imported rather than
re-parsed from console output. There is therefore exactly one definition of
"this access lacks a citation", and no text seam between detector and gate that
could silently stop matching.

Note that `&reg->FIELD` -- taking the address of a register field -- counts.
That is deliberate and is asserted in `cite_check.py --selftest`. In this tree
address-of is overwhelmingly a register handed to a register-agnostic poll
helper (`ra8_hw_wait_flag_set32(&reg->CFDGSTS, ...)`), an aliasing volatile
pointer that is then read and written, or a DMA/DTC descriptor field naming the
register the engine will write. In every one of those the load or store happens
somewhere that does NOT name the register, so the address-of site is the only
reviewable place a citation can live.

BASELINE NORMALISATION -- per-file COUNTS, not raw finding lines.
Raw findings carry line numbers, which churn on every unrelated edit above them,
and a snippet of source, which embeds identifiers. A `file -> count` map is
invariant under both, still trips the moment a file gains another uncited
access, and stays small and diffable. Per-file rather than per-function because
an uncited access is a property of a source location, not of a decision: many
sit in register-map headers and in table-driven initialisation blocks that have
no enclosing function at all.

USAGE
    python3 scripts/checks/cite_ratchet.py --selftest  # assert it fires
    python3 scripts/checks/cite_ratchet.py --check     # the CI gate
    python3 scripts/checks/cite_ratchet.py --update    # re-baseline
    python3 scripts/checks/cite_ratchet.py --list      # burn-down list

Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile
from collections import Counter

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from cite_check import SOURCE_SUFFIXES, iter_source_files, scan_access_coverage
from lint_targets import first_party_paths

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINE_FILE = REPO_ROOT / ".github" / "cite-baseline.txt"

MAX_DETAIL_LINES = 10
"""Cap on offending buckets echoed before the report truncates."""

BASELINE_COLUMNS = 2
"""Column count of one baseline row: file, count."""

MIN_SCANNED_FILES = 1700
"""Refuse to ratchet a scan that saw implausibly few source files.

`cite_check`'s derived first-party C scope is 2120 files today (libs 790,
tests 618, examples 408, tools 210, port 78, src 16). A scan that finds a
fraction of that is not looking at this repository -- a partial checkout, a
`git ls-files` that came back short, an enumeration that stopped covering a
top-level directory. It would report FEWER uncited accesses, which reads as a
burn-down, and `--update` would freeze that as the accepted state. 1700 leaves
room for genuine deletion while catching the loss of any whole top-level tree.
"""

MIN_ACCESS_LINES = 3000
"""...and refuse a scan whose DETECTOR stopped matching register accesses.

The file floor above cannot catch this: the right 2120 files get opened and
`ACCESS_RE` simply matches nothing, so the backlog collapses to zero and
`--check` passes cleanly forever. That is this repository's dominant defect
class, so the count of MMIO access lines the detector found -- cited or not --
is measured alongside the findings and floored too. The tree carries 3971 of
them today; 3000 clears real churn while a detector that broke drops to
near zero.
"""


def scan_files(files: list[pathlib.Path], root: pathlib.Path) -> tuple[Counter[str], int, int]:
    """Measure uncited accesses over an explicit file list.

    Args:
        files: Source files to scan. Read as UTF-8 with replacement, matching
            `cite_check.check_file`.
        root: Directory the returned keys are made relative to.

    Returns:
        A ``(counts, scanned_file_count, access_line_count)`` triple. ``counts``
        maps a root-relative path to the number of uncited accesses in it. The
        two scalars accompany it so the caller can refuse a scan whose scope
        never got established -- a count is only trustworthy once the thing
        that produced it is known to have looked at the tree AND to still be
        matching register accesses at all.
    """
    counts: Counter[str] = Counter()
    access_lines = 0
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        findings, seen = scan_access_coverage(path, text)
        access_lines += seen
        if findings:
            counts[str(path.relative_to(root))] = len(findings)
    return counts, len(files), access_lines


def tree_files() -> list[pathlib.Path]:
    """Every first-party C file `cite_check` scans, as absolute paths.

    Returns:
        The same derived scope `cite_check.main` uses with no path arguments,
        so the ratchet and the detector can never disagree about what is in
        scope.
    """
    targets = [REPO_ROOT / rel for rel in first_party_paths(SOURCE_SUFFIXES)]
    return list(iter_source_files(targets))


def scan_tree() -> tuple[Counter[str], int, int]:
    """Run `scan_files` over the whole repository.

    Returns:
        The ``(counts, scanned_file_count, access_line_count)`` triple for this
        repository, keyed on repo-relative paths.
    """
    return scan_files(tree_files(), REPO_ROOT)


def load_baseline() -> Counter[str]:
    """Read the committed baseline into a Counter.

    Returns:
        A ``file -> count`` Counter. A missing baseline file means an empty
        one, which is the end state this ratchet is driving toward.
    """
    counts: Counter[str] = Counter()
    if not BASELINE_FILE.is_file():
        return counts
    for raw in BASELINE_FILE.read_text(encoding="ascii").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != BASELINE_COLUMNS:
            continue
        counts[parts[0]] = int(parts[1])
    return counts


def write_baseline(counts: Counter[str]) -> None:
    """Write `counts` out in the committed, sorted, diffable form.

    Args:
        counts: The ``file -> count`` map to freeze. Zero-valued buckets are
            dropped so a burned-down file leaves the file entirely.
    """
    total = sum(counts.values())
    lines = [
        "# HUM citation-COVERAGE ratchet baseline -- per-file uncited-access counts.",
        "# Consumed by scripts/checks/cite_ratchet.py --check (CI gate: cite-check).",
        "#",
        "# Each row is a source file holding N direct MMIO register accesses with NO",
        '# `/* HUM Ch X.Y "..." p NNNN */` citation above them. CLAUDE.md, the style',
        "# guide and docs/CITATION_POLICY.md all call that citation MANDATORY, but the",
        "# detector for it (cite_check.py --require-cites) ran in no gate at all, so",
        "# the rule was aspirational and the debt went unmeasured (#534).",
        "#",
        f"# Total at this baseline: {total} uncited access(es)",
        f"# across {len(counts)} file(s).",
        "#",
        "# The gate fails on any INCREASE, so the debt is frozen and can only be",
        "# burned down; a newly-added uncited access raises a count and fails.",
        "#",
        "# Burn one down: read the register in the Hardware User's Manual",
        "# (docs/reference/r01uh1065ej0130-ra8d2.pdf), then put the real chapter,",
        "# subsection and page above the access:",
        "#",
        '#   /* HUM Ch 25.2.3 "AGT Control Register" p 1194 */',
        "#   reg->AGTCR = k_ra8_agt_start;",
        "#",
        "# One citation covers the contiguous block of accesses beneath it. Do NOT",
        "# guess a subsection: a wrong one passes cite_check --strict while being",
        "# factually false, which is worse than the missing citation it replaced.",
        "#",
        "# Regenerate after burning findings down:",
        "#   python3 scripts/checks/cite_ratchet.py --update",
        "#",
        "# MOVING a file retires its row and creates a new one, which reads as growth",
        "# -- so the gate fails and --update refuses, by design. Rename the row here",
        "# BY HAND, keeping the count identical. That is a one-line reviewable diff,",
        "# and it is deliberately not automated: rename detection that guessed wrong",
        "# would silently absorb a genuinely new uncited access.",
        "#",
        "# Closing this out means this file reaching zero rows and being",
        "# DELETED -- never regenerated larger.",
        "#",
        "# file<TAB>count",
    ]
    for path, n in sorted(counts.items()):
        if n:
            lines.append(f"{path}\t{n}")
    BASELINE_FILE.write_text("\n".join(lines) + "\n", encoding="ascii")


def scope_reason(files: int, access_lines: int) -> str | None:
    """Return a refusal reason when the scan's scope is not credible, else None.

    Guards BOTH directions of the ratchet: a scan that examined a fragment of
    the tree, or whose detector stopped recognising register accesses, produces
    a number that must not be compared against the baseline and must certainly
    never be written as one.

    Args:
        files: How many source files the scan actually opened.
        access_lines: How many MMIO access lines it recognised, cited or not.

    Returns:
        A human-readable reason to refuse, or None when both floors are met.
    """
    if files < MIN_SCANNED_FILES:
        return (
            f"only {files} first-party C file(s) scanned, "
            f"below the {MIN_SCANNED_FILES} floor.\n"
            "  The scan is not looking at this repository (a partial checkout,\n"
            "  or a git ls-files enumeration that came back short). A partial\n"
            "  scan reports FEWER uncited accesses, which reads as a burn-down;\n"
            "  refusing it is the only way that cannot be mistaken for progress."
        )
    if access_lines < MIN_ACCESS_LINES:
        return (
            f"only {access_lines} MMIO access line(s) recognised, "
            f"below the {MIN_ACCESS_LINES} floor.\n"
            "  The right files were opened and the DETECTOR matched almost\n"
            "  nothing in them, so every count is meaningless in both\n"
            "  directions. What changed is cite_check.ACCESS_RE, not the code.\n"
            "  Run `python3 scripts/checks/cite_check.py --selftest` first."
        )
    return None


def report(current: Counter[str], baseline: Counter[str]) -> int:
    """Compare a scan against the baseline and print the verdict.

    Args:
        current: Freshly measured ``file -> count`` map.
        baseline: The committed ``file -> count`` map.

    Returns:
        0 when every count is at or below the baseline, 1 when one grew.
    """
    grown = []
    for path, n in sorted(current.items()):
        was = baseline.get(path, 0)
        if n > was:
            grown.append((path, was, n))

    total_now = sum(current.values())
    total_was = sum(baseline.values())

    if grown:
        print(file=sys.stderr)
        print("cite ratchet: NEW uncited MMIO accesses above the baseline", file=sys.stderr)
        print(file=sys.stderr)
        for path, was, now in grown[:MAX_DETAIL_LINES]:
            print(f"  {path}: {was} -> {now}", file=sys.stderr)
        if len(grown) > MAX_DETAIL_LINES:
            print(f"  ... and {len(grown) - MAX_DETAIL_LINES} more file(s)", file=sys.stderr)
        print(file=sys.stderr)
        print(
            "Every direct register read/write or access needs a Hardware User's\n"
            "Manual citation immediately above it:\n"
            "\n"
            '    /* HUM Ch 25.2.3 "AGT Control Register" p 1194 */\n'
            "    reg->AGTCR = k_ra8_agt_start;\n"
            "\n"
            "One citation covers the contiguous block of accesses beneath it. See\n"
            "the offending lines with:\n"
            "    python3 scripts/checks/cite_check.py --require-cites <file>\n"
            "\n"
            "The baseline is a burn-down of debt that predates enforcement, not a\n"
            "place to record new debt. Do not --update to make this pass.",
            file=sys.stderr,
        )
        return 1

    print(f"cite ratchet: {total_now} uncited access(es), baseline {total_was} -- no growth.")
    if total_now < total_was:
        burned = total_was - total_now
        print(f"  {burned} access(es) burned down. Re-baseline to lock it in:")
        print("    python3 scripts/checks/cite_ratchet.py --update")
    return 0


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

# One uncited MMIO write. The measurement must COUNT it.
_ST_UNCITED_C = """\
void drv_init(void)
{
  volatile r_agt_regs_t* reg = agt0();
  reg->AGTCR = 0U;
}
"""

# The same write, cited. The measurement must NOT count it.
_ST_CITED_C = """\
void drv_start(void)
{
  volatile r_agt_regs_t* reg = agt0();
  /* HUM Ch 25.2.3 "AGT Control Register" p 1194 */
  reg->AGTCR = 1U;
}
"""

# A SECOND uncited access added to a file the baseline already knows about --
# the growth shape a whole-tree pass/fail check would miss entirely.
_ST_UNCITED_C_GROWN = """\
void drv_init(void)
{
  volatile r_agt_regs_t* reg = agt0();
  reg->AGTCR = 0U;
  reg->AGTMR1 = 0U;
}
"""


_ST_FIXTURE_FILES = 2
"""Files the selftest fixture plants: one uncited, one cited."""

_ST_GROWN_COUNT = 2
"""Uncited accesses in the fixture after the growth edit."""


def _st_write(root: pathlib.Path, rel: str, body: str) -> pathlib.Path:
    """Write `body` to `rel` under `root`, creating parent directories.

    Args:
        root: Fixture tree root.
        rel: Path relative to `root`.
        body: ASCII file contents.

    Returns:
        The absolute path written.
    """
    dst = root / rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(body, encoding="ascii")
    return dst


def _selftest_scan(tmp: pathlib.Path) -> list[str]:
    """Assert the REAL measurement counts the right accesses and no others.

    Args:
        tmp: A throwaway directory to build the fixture tree in.

    Returns:
        A list of failure descriptions; empty when every assertion held.
    """
    failures: list[str] = []
    root = tmp / "tree"
    uncited = _st_write(root, "libs/uncited.c", _ST_UNCITED_C)
    cited = _st_write(root, "libs/cited.c", _ST_CITED_C)

    counts, files, access_lines = scan_files([uncited, cited], root)
    if counts.get("libs/uncited.c") != 1:
        failures.append("scan_files() did not count the uncited MMIO write")
    if "libs/cited.c" in counts:
        failures.append("scan_files() counted an access that carries a HUM citation")
    if files != _ST_FIXTURE_FILES:
        failures.append(f"scan_files() reported {files} scanned file(s), expected 2")
    if access_lines != _ST_FIXTURE_FILES:
        failures.append(f"scan_files() saw {access_lines} access line(s), expected 2")

    # The measurement must SEE the growth it is meant to gate on.
    _st_write(root, "libs/uncited.c", _ST_UNCITED_C_GROWN)
    grown, _files, _lines = scan_files([uncited, cited], root)
    if grown.get("libs/uncited.c") != _ST_GROWN_COUNT:
        failures.append("scan_files() did not see a second uncited access added to a known file")
    return failures


def _selftest_ratchet() -> list[str]:
    """Assert the growth verdict and the baseline round-trip, both directions.

    Returns:
        A list of failure descriptions; empty when every assertion held.
    """
    failures: list[str] = []

    base: Counter[str] = Counter({"libs/f.c": 1})
    # FAILS when a known bucket grows ...
    if report(Counter({"libs/f.c": 2}), base) == 0:
        failures.append("report() passed a file whose count GREW above the baseline")
    # ... and when a file the baseline has never seen appears.
    if report(Counter({"libs/new.c": 1}), base) == 0:
        failures.append("report() passed a finding in a file absent from the baseline")
    # PASSES when unchanged or shrinking -- the ratchet must not be a cliff.
    if report(Counter({"libs/f.c": 1}), base) != 0:
        failures.append("report() failed an unchanged bucket")
    if report(Counter(), base) != 0:
        failures.append("report() failed a fully burned-down baseline")

    # Round-trip: what is written must read back identically, or a re-baseline
    # would silently reshape the debt it claims to be freezing.
    original = BASELINE_FILE.read_text(encoding="ascii") if BASELINE_FILE.is_file() else None
    try:
        fixture: Counter[str] = Counter({"libs/a.c": 3, "tests/test_b.c": 1})
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
    """Assert the scope guards refuse an implausible scan, in both directions.

    Returns:
        A list of failure descriptions; empty when every assertion held.
    """
    failures: list[str] = []
    if scope_reason(MIN_SCANNED_FILES - 1, MIN_ACCESS_LINES) is None:
        failures.append("scope_reason() accepted a scan that saw too few source files")
    if scope_reason(MIN_SCANNED_FILES, MIN_ACCESS_LINES - 1) is None:
        failures.append("scope_reason() accepted a scan whose detector matched almost nothing")
    if scope_reason(MIN_SCANNED_FILES, MIN_ACCESS_LINES) is not None:
        failures.append("scope_reason() rejected a scan that is exactly at both floors")
    return failures


def selftest() -> int:
    """Assert the measurement and the ratchet fire, in BOTH directions.

    Runs the REAL `find_uncited_accesses` against a throwaway fixture tree, so
    a detector that quietly stopped matching cannot pass as clean: it must
    COUNT the uncited write, must NOT count the cited one, and must see a
    second uncited access appear in a file it already knew about.

    Returns:
        0 when every assertion held, 1 otherwise.
    """
    with tempfile.TemporaryDirectory() as td:
        failures = _selftest_scan(pathlib.Path(td))
    failures += _selftest_ratchet() + _selftest_scope_guard()

    if failures:
        print("SELFTEST FAILED:", file=sys.stderr)
        for problem in failures:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print(
        "selftest: HUM citation-coverage ratchet OK "
        "(counts an uncited access; ignores a cited one; fails on growth and on "
        "an unseen file, passes on equal and on shrinkage; refuses an "
        "implausible scope)."
    )
    return 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def _list_backlog() -> int:
    """Print every uncited access, worst file first, for burn-down work.

    Returns:
        0 always -- listing is a report, not a verdict.
    """
    counts, _files, _lines = scan_tree()
    for path, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
        print(f"{n:5d}  {path}")
    print(f"total: {sum(counts.values())} uncited access(es) across {len(counts)} file(s)")
    return 0


def _do_update(current: Counter[str], files: int) -> int:
    """Rewrite the baseline, refusing to grow any existing bucket.

    Args:
        current: The freshly measured ``file -> count`` map.
        files: How many source files the scan opened, for the summary line.

    Returns:
        0 when the baseline was written, 1 when the update was refused.
    """
    baseline = load_baseline()
    # Seeding the very first baseline necessarily "grows" every bucket from
    # nothing, so the no-growth rule applies only once a baseline exists.
    seeding = not BASELINE_FILE.is_file()
    grew = [] if seeding else [p for p, n in current.items() if n > baseline.get(p, 0)]
    if grew:
        print(
            f"refusing to --update: {len(grew)} file(s) would GROW. "
            "The baseline is a burn-down; write the missing HUM citations instead.",
            file=sys.stderr,
        )
        for path in grew[:MAX_DETAIL_LINES]:
            print(f"  {path}", file=sys.stderr)
        return 1
    write_baseline(current)
    print(
        f"baseline updated: {sum(current.values())} uncited access(es) recorded "
        f"across {len(current)} file(s) ({files} file(s) scanned)."
    )
    return 0


def main() -> int:
    """Ratchet uncited MMIO accesses against the committed baseline.

    A ratchet, not a floor: ``--check`` fails when a count RISES, and
    ``--update`` lowers the baseline once citations are written. The baseline
    can therefore only move downward, which is what stops a large legacy count
    from being permanently accepted while still letting CI block a new one
    today.

    Returns:
        0 when every count is at or below the baseline, 1 when one grew or the
        scan's scope was not credible.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="gate against the baseline")
    parser.add_argument("--update", action="store_true", help="rewrite the baseline")
    parser.add_argument("--list", action="store_true", help="print the whole backlog")
    parser.add_argument("--selftest", action="store_true", help="assert this gate still fires")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if args.list:
        return _list_backlog()
    if not (args.check or args.update):
        parser.error("one of --check / --update / --list / --selftest is required")

    current, files, access_lines = scan_tree()

    # Refuse to ratchet a scan whose scope never got established, in EITHER
    # direction, before comparing or writing anything.
    broken = scope_reason(files, access_lines)
    if broken:
        verb = "--update" if args.update else "--check"
        print(f"refusing to {verb}: {broken}", file=sys.stderr)
        return 1

    if args.update:
        return _do_update(current, files)

    print(f"cite ratchet: scanned {files} file(s), {access_lines} MMIO access line(s).")
    return report(current, load_baseline())


if __name__ == "__main__":
    sys.exit(main())
