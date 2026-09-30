#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""matrix_ratchet.py -- ra8_emulator example-matrix ratchet (compare vs baseline).

`scripts/emu/matrix.sh` boots EVERY example under `examples/ek_ra8d2/` on the
board emulator and writes one `app<pad>VERDICT` row per app to
`build/ra8_emulator_matrix.txt`. That sweep measures #67's own headline success
criterion -- "every example runs in the emulator" -- and until #394 it was
invoked by nothing: not ci.sh, not a workflow, not the justfile. The repo's
dominant defect class (a gate wired to nothing) applied to the epic's own
definition of done.

This script turns that report into a one-way ratchet against a committed
baseline:

* NEW debt (an app in a failing state that the baseline does not record in
  that state) FAILS the gate.
* Shrinkage (debt burned down) PASSES with a notice to re-baseline via
  `--update`, which locks the progress in so it cannot quietly grow back.

WHY PER-APP BUCKETS AND NOT A BARE TOTAL. A single number is satisfied by a
swap: fix one app, break another, net zero, gate green. The bucket key is
`(app, verdict)`, so a swap trips -- the new pair is absent from the baseline.
It is also what makes the burn-down actionable: the diff names the app.

This is a RATCHET, not an allowlist. A baseline entry is recorded debt with an
end state of zero, it is never a permanent exemption, and nothing in this file
can mark an app "expected to fail forever".

VERDICT CLASSES. Debt is FAULT / TRUNCATED / UNKNOWN / BUILD_FAIL / NO_ELF.
OK and HALT reached their budget. SPECIAL (two-image TrustZone, needs a --ns
recipe) and SKIPPED (_unsupported tier, needs external hardware) are not run
at all, so they are neither credit nor debt.

TRUNCATED is debt on purpose. It means a wall-clock bound cut the run short
before the deterministic chunk budget -- the app produced NO verdict. Counting
a non-verdict as a pass is the #168 mislabel; counting it as a fault invents a
failure. It is its own bucket so the burn-down can see it.

SELF-ATTESTATION (#712). The ratchet reads the baseline's ROWS; until now
nothing asserted the FILE is one this tool wrote. A hand edit that kept the
rows parseable -- a whole-file `sort`, a typed row, an adjusted `# total:` --
went around the ratchet's own refusal and survived ten days. `--attest`
re-derives the canonical text from the committed rows and demands byte
identity, and `--check`/`--update` run it first. It needs no report and no
emulator, so it runs anywhere. The optional cause column is part of the
canonical form, not noise to normalise away.

USAGE
    python3 scripts/checks/matrix_ratchet.py --selftest    # assert it fires
    python3 scripts/checks/matrix_ratchet.py --attest      # baseline is machine-written
    python3 scripts/checks/matrix_ratchet.py --check       # the gate
    python3 scripts/checks/matrix_ratchet.py --update      # re-baseline

`--check` and `--update` read `build/ra8_emulator_matrix.txt`; produce it first:
    bash scripts/emu/matrix.sh

Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
REPORT_FILE = REPO_ROOT / "build" / "ra8_emulator_matrix.txt"
BASELINE_FILE = REPO_ROOT / ".github" / "emulator-matrix-baseline.txt"

MAX_DETAIL_LINES = 250
"""Cap on offending apps echoed before the report truncates.

Deliberately above the example count. The first run of this gate on a new
machine has an empty baseline, so EVERY failing app is "new" -- and that
listing is what the baseline is then built from. A cap that truncated it
would make the gate's own bootstrap output unusable.
"""

BASELINE_COLUMNS = 2
"""Column count of one baseline row: app, verdict."""

ATTESTATION_CASES = 5
"""Distinct hand-edit shapes `_selftest_attestation` proves are rejected."""

DEBT_VERDICTS = frozenset({"FAULT", "TRUNCATED", "UNKNOWN", "BUILD_FAIL", "NO_ELF"})
"""Verdicts that count as debt -- the set this gate ratchets downward."""

PASS_VERDICTS = frozenset({"OK", "HALT"})
"""Verdicts where the app reached its run budget."""

NOT_RUN_VERDICTS = frozenset({"SPECIAL", "SKIPPED"})
"""Verdicts for apps the matrix never boots (neither credit nor debt)."""

KNOWN_VERDICTS = DEBT_VERDICTS | PASS_VERDICTS | NOT_RUN_VERDICTS
"""Every verdict matrix.sh can emit. An unknown one is a hard error."""


def parse_report(text: str) -> dict[str, str]:
    """Return an {app: verdict} map for a matrix.sh report.

    Each row is `app` padded to a column then the verdict, so a plain split on
    whitespace recovers both. A row that does not yield exactly two fields is a
    malformed report and is fatal: silently skipping rows is how a gate ends up
    ratcheting against a fraction of the sweep and calling it clean.

    Args:
        text: The full contents of `build/ra8_emulator_matrix.txt`.

    Returns:
        A mapping of app name to its verdict string.
    """
    verdicts: dict[str, str] = {}
    for raw in text.splitlines():
        if not raw.strip():
            continue
        cols = raw.split()
        if len(cols) != BASELINE_COLUMNS:
            sys.stderr.write(f"matrix_ratchet.py: ERROR -- malformed report row: {raw!r}\n")
            sys.exit(1)
        app, verdict = cols
        if verdict not in KNOWN_VERDICTS:
            sys.stderr.write(
                f"matrix_ratchet.py: ERROR -- unknown verdict {verdict!r} for {app!r}.\n"
                f"       Known: {' '.join(sorted(KNOWN_VERDICTS))}\n"
                "       A new matrix.sh state must be classified here as debt or\n"
                "       not-debt; leaving it unclassified would let it slip the gate.\n"
            )
            sys.exit(1)
        verdicts[app] = verdict
    return verdicts


def debt_of(verdicts: dict[str, str]) -> dict[str, str]:
    """Return only the failing entries of a verdict map.

    Args:
        verdicts: An {app: verdict} map.

    Returns:
        The subset whose verdict is in `DEBT_VERDICTS`.
    """
    return {app: v for app, v in verdicts.items() if v in DEBT_VERDICTS}


def load_baseline(path: Path) -> dict[str, str]:
    """Parse the committed baseline into an {app: verdict} map.

    Args:
        path: The baseline file.

    Returns:
        The recorded debt, empty when the file does not exist.
    """
    return _parse_baseline(path)[0]


def load_baseline_causes(path: Path) -> dict[str, str]:
    """Return the {app: cause} notes recorded alongside the baseline verdicts.

    Args:
        path: The baseline file.

    Returns:
        The recorded causes; apps without one are absent.
    """
    return _parse_baseline(path)[1]


def _parse_baseline(path: Path) -> tuple[dict[str, str], dict[str, str]]:
    """Parse the baseline into ({app: verdict}, {app: cause}).

    A row is `app<TAB>verdict` or `app<TAB>verdict<TAB>cause`. The cause is
    optional so a machine-written baseline stays valid, but it is the whole
    reason the third column exists -- see `write_baseline`.

    Args:
        path: The baseline file.

    Returns:
        The recorded verdicts and causes, both empty when the file is absent.
    """
    if not path.exists():
        return {}, {}
    verdicts: dict[str, str] = {}
    causes: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        if not raw.strip() or raw.startswith("#"):
            continue
        cols = raw.split("\t")
        if len(cols) not in (BASELINE_COLUMNS, BASELINE_COLUMNS + 1):
            sys.stderr.write(f"matrix_ratchet.py: ERROR -- malformed baseline row: {raw!r}\n")
            sys.exit(1)
        verdicts[cols[0]] = cols[1]
        if len(cols) == BASELINE_COLUMNS + 1 and cols[2].strip():
            causes[cols[0]] = cols[2].strip()
    return verdicts, causes


def render_baseline(debt: dict[str, str], causes: dict[str, str] | None = None) -> str:
    """Return the canonical baseline text for a debt map. THE committed form.

    This is the single definition of what a machine-written baseline looks
    like: fixed header, `# total:` line, then one row per app in sorted order
    with the optional cause as a third TAB-separated column. `write_baseline`
    emits it and `attest_baseline` re-derives it, so the file on disk can be
    compared against what the tool would have produced.

    Args:
        debt: The {app: verdict} debt to record.
        causes: Optional {app: cause} notes to preserve.

    Returns:
        The full file text, newline-terminated.
    """
    causes = causes or {}
    lines = [
        "# ra8_emulator example-matrix baseline -- see scripts/checks/matrix_ratchet.py",
        "#",
        "# Recorded debt from `bash scripts/emu/matrix.sh`, one",
        "# `app<TAB>verdict<TAB>cause` row each. This is a RATCHET: growth fails,",
        "# shrinking is free, and the end state is an EMPTY file. It is not an",
        "# allowlist -- no row here is a permanent exemption, and every one is work",
        "# still owed. The cause column is not decoration: a number nobody can",
        "# explain is indistinguishable from one nobody has looked at.",
        "#",
        "# MEASURE THIS ON THE CI RUNNER, NEVER ON A DEVELOPER BOX -- see #400.",
        "#",
        "# Re-baseline after burning debt down (causes are carried forward):",
        "#   bash scripts/emu/matrix.sh; python3 scripts/checks/matrix_ratchet.py --update",
        f"# total: {len(debt)}",
    ]
    for app, verdict in sorted(debt.items()):
        cause = causes.get(app, "")
        lines.append(f"{app}\t{verdict}\t{cause}" if cause else f"{app}\t{verdict}")
    return "\n".join(lines) + "\n"


def write_baseline(path: Path, debt: dict[str, str], causes: dict[str, str] | None = None) -> None:
    """Rewrite the baseline from a measured debt map, keeping recorded causes.

    Each row carries an optional third column naming WHY the app is failing.
    Without it a future reader sees a bare count and cannot tell recorded debt
    from an unexamined allowlist -- which is the failure mode this whole gate
    was built against. `--update` therefore carries the existing cause forward
    for any app still in debt rather than regenerating a comment-free file.

    Args:
        path: The baseline file to write.
        debt: The {app: verdict} debt to record.
        causes: Optional {app: cause} notes to preserve.
    """
    path.write_text(render_baseline(debt, causes), encoding="utf-8")


def _first_difference(committed: str, rendered: str) -> str:
    """Return a one-line description of where two texts first diverge.

    Args:
        committed: The text read from disk.
        rendered: The text the tool would emit.

    Returns:
        A printable `line N: ...` description.
    """
    got = committed.splitlines()
    want = rendered.splitlines()
    for number, (a, b) in enumerate(zip(got, want), start=1):
        if a != b:
            return f"line {number}: committed {a!r}, tool emits {b!r}"
    if len(got) > len(want):
        return f"line {len(want) + 1}: committed has {len(got) - len(want)} extra line(s)"
    return f"line {len(got) + 1}: committed is missing {len(want) - len(got)} line(s)"


def attest_baseline(baseline_file: Path = BASELINE_FILE) -> list[str]:
    """Return the ways the committed baseline is not what this tool would emit.

    The ratchet reads ROWS; nothing until now asserted the FILE is one
    `write_baseline` produced. A hand edit that keeps the rows parseable --
    a whole-file `sort`, a hand-written row, an adjusted `# total:` -- passed
    silently, which is the ten-day bypass #712 records.

    An ABSENT baseline is not a violation: this gate's documented bootstrap on
    a new machine is an empty baseline whose first run lists every failing app.
    A row is, though, if its verdict is not debt: `--update` records only
    `DEBT_VERDICTS`, so an `OK` row could only have been typed by hand.

    Args:
        baseline_file: The committed baseline to attest.

    Returns:
        A list of problem descriptions, empty when the file is canonical.
    """
    if not baseline_file.exists():
        return []
    committed = baseline_file.read_text(encoding="utf-8")
    verdicts, causes = _parse_baseline(baseline_file)
    problems: list[str] = []
    for app, verdict in sorted(verdicts.items()):
        if verdict not in KNOWN_VERDICTS:
            problems.append(f"{app}: verdict {verdict!r} is not one matrix.sh emits")
        elif verdict not in DEBT_VERDICTS:
            problems.append(
                f"{app}: {verdict} is not debt, so --update would never record it "
                "-- the row was written by hand"
            )
    rendered = render_baseline(verdicts, causes)
    if committed != rendered:
        problems.append(_first_difference(committed, rendered))
    return problems


def report_attestation(problems: list[str], baseline_file: Path = BASELINE_FILE) -> None:
    """Print the failure report for a baseline the tool would not have written.

    Args:
        problems: The descriptions returned by `attest_baseline`.
        baseline_file: The file they describe.
    """
    try:
        shown = baseline_file.relative_to(REPO_ROOT)
    except ValueError:
        shown = baseline_file
    sys.stderr.write(
        f"\nmatrix_ratchet.py: FAIL -- {shown} is not what this tool emits.\n\n"
    )
    for line in problems[:MAX_DETAIL_LINES]:
        sys.stderr.write(f"  {line}\n")
    sys.stderr.write(
        "\n  A generated baseline is machine-written. Editing it by hand goes\n"
        "  around the ratchet's own refusal, and a parseable hand edit used to\n"
        "  survive indefinitely (#712). Regenerate it instead:\n"
        "      bash scripts/emu/matrix.sh\n"
        "      python3 scripts/checks/matrix_ratchet.py --update\n"
        "  Causes are carried forward, so re-baselining does not lose them.\n"
    )


def summarise(verdicts: dict[str, str]) -> str:
    """Return a one-line count of each verdict class, most-failing first.

    Args:
        verdicts: An {app: verdict} map.

    Returns:
        A printable summary such as `OK 155  FAULT 46  SPECIAL 3`.
    """
    counts: dict[str, int] = {}
    for verdict in verdicts.values():
        counts[verdict] = counts.get(verdict, 0) + 1
    return "  ".join(f"{v} {n}" for v, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])))


def report_growth(grown: dict[str, str], baseline: dict[str, str]) -> None:
    """Print the failure report for newly-appeared debt.

    Args:
        grown: The {app: verdict} debt absent from the baseline.
        baseline: The recorded baseline, used to explain a changed verdict.
    """
    sys.stderr.write(
        f"\nmatrix_ratchet.py: FAIL -- {len(grown)} example(s) newly failing in ra8_emulator.\n\n"
    )
    for app, verdict in sorted(grown.items())[:MAX_DETAIL_LINES]:
        was = baseline.get(app)
        prior = f"was {was}" if was else "not in the baseline"
        sys.stderr.write(f"  {app:<32} {verdict:<11} ({prior})\n")
    if len(grown) > MAX_DETAIL_LINES:
        sys.stderr.write(f"  ... and {len(grown) - MAX_DETAIL_LINES} more\n")
    sys.stderr.write(
        "\n  This gate ratchets the ra8_emulator example matrix DOWNWARD (#394): the\n"
        "  count may shrink freely and may never grow. Either fix the example /\n"
        "  the ra8_emulator model gap, or -- if this is a verdict CHANGE rather than\n"
        "  a regression -- explain it in the commit and re-baseline with:\n"
        "      bash scripts/emu/matrix.sh\n"
        "      python3 scripts/checks/matrix_ratchet.py --update\n"
    )


def check(report_file: Path = REPORT_FILE, baseline_file: Path = BASELINE_FILE) -> int:
    """Run the ratchet against the committed baseline.

    Args:
        report_file: The matrix.sh report to read.
        baseline_file: The committed baseline to compare against.

    Returns:
        A process exit status: 0 when debt did not grow, 1 when it did.
    """
    problems = attest_baseline(baseline_file)
    if problems:
        report_attestation(problems, baseline_file)
        return 1
    if not report_file.exists():
        sys.stderr.write(
            f"matrix_ratchet.py: FATAL -- no report at {report_file}.\n"
            "       Run `bash scripts/emu/matrix.sh` first. A missing report is a\n"
            "       hard failure, never a silent pass -- a gate that reports\n"
            "       nothing for work never done is the defect this closes.\n"
        )
        return 1
    verdicts = parse_report(report_file.read_text(encoding="utf-8"))
    if not verdicts:
        sys.stderr.write(
            "matrix_ratchet.py: FATAL -- the report is empty; the sweep did not run.\n"
        )
        return 1
    debt = debt_of(verdicts)
    baseline = load_baseline(baseline_file)

    # The count is printed on EVERY run, pass or fail, so the burn-down is
    # visible in the log rather than buried in a baseline diff.
    print(f"ra8_emulator matrix: {len(verdicts)} example(s) -- {summarise(verdicts)}")
    print(f"ra8_emulator matrix: failing {len(debt)}, baseline {len(baseline)}")

    grown = {app: v for app, v in debt.items() if baseline.get(app) != v}
    if grown:
        report_growth(grown, baseline)
        return 1
    fixed = {app: v for app, v in baseline.items() if debt.get(app) != v}
    if fixed:
        print(f"ra8_emulator matrix: {len(fixed)} example(s) improved since the baseline:")
        for app, verdict in sorted(fixed.items())[:MAX_DETAIL_LINES]:
            print(f"  {app:<32} {verdict} -> {verdicts.get(app, 'gone')}")
        print(
            "  Lock the progress in:  python3 scripts/checks/matrix_ratchet.py --update\n"
            "  (until then the gate still passes, but the debt can grow back to the\n"
            "  old, larger baseline without failing.)"
        )
    print("ra8_emulator matrix: OK -- the failing-example count did not grow.")
    return 0


def update(report_file: Path = REPORT_FILE, baseline_file: Path = BASELINE_FILE) -> int:
    """Rewrite the baseline from the current report.

    Args:
        report_file: The matrix.sh report to read.
        baseline_file: The baseline file to rewrite.

    Returns:
        A process exit status: 0 on success, 1 when the report is missing.
    """
    # Attest FIRST: --update carries the committed causes forward, so a
    # hand-edited file would launder its edits into the regenerated one.
    problems = attest_baseline(baseline_file)
    if problems:
        report_attestation(problems, baseline_file)
        return 1
    if not report_file.exists():
        sys.stderr.write(f"matrix_ratchet.py: FATAL -- no report at {report_file}.\n")
        return 1
    debt = debt_of(parse_report(report_file.read_text(encoding="utf-8")))
    # Carry every recorded cause forward, so re-baselining after a burn-down
    # cannot silently strip the explanations off the apps that remain.
    causes = load_baseline_causes(baseline_file)
    write_baseline(baseline_file, debt, causes)
    missing = sorted(app for app in debt if app not in causes)
    print(f"matrix_ratchet.py: baseline rewritten -- {len(debt)} failing example(s).")
    if missing:
        print(
            f"matrix_ratchet.py: {len(missing)} entr(y/ies) have no recorded cause: "
            f"{', '.join(missing)}\n"
            "  Add one as a third TAB-separated column. Debt nobody can explain\n"
            "  reads as an allowlist the first time someone else looks at it."
        )
    return 0


def _selftest_classification() -> list[str]:
    """Assert every verdict is classified, and classified the right way.

    Returns:
        A list of failure descriptions, empty when the classification holds.
    """
    failures: list[str] = []
    # An unclassified verdict is the silent-hole direction: it would be neither
    # debt nor pass, so an app in that state could never fail the gate.
    unclassified = KNOWN_VERDICTS - (DEBT_VERDICTS | PASS_VERDICTS | NOT_RUN_VERDICTS)
    if unclassified:
        failures.append(f"verdict(s) in no class: {sorted(unclassified)}")
    for overlap, names in (
        (DEBT_VERDICTS & PASS_VERDICTS, "debt/pass"),
        (DEBT_VERDICTS & NOT_RUN_VERDICTS, "debt/not-run"),
    ):
        if overlap:
            failures.append(f"{names} classes overlap on {sorted(overlap)}")
    # The specific mislabels this gate exists to prevent.
    if "TRUNCATED" not in DEBT_VERDICTS:
        failures.append("TRUNCATED is not debt -- a non-verdict would read as a pass (#168)")
    if "OK" in DEBT_VERDICTS:
        failures.append("OK is classified as debt")
    sample = parse_report("blink                      OK\nusb_x                      FAULT\n")
    if debt_of(sample) != {"usb_x": "FAULT"}:
        failures.append(f"debt_of() picked the wrong rows: {debt_of(sample)}")
    return failures


def _selftest_ratchet(tmp: Path) -> list[str]:
    """Assert the ratchet fires on growth and stays quiet on shrinkage.

    Both directions are asserted against real files. A ratchet that only ever
    returns 0 looks exactly like a clean tree, which is the failure mode this
    whole gate exists to close -- so "it must FAIL on a broken input" is the
    load-bearing half.

    Args:
        tmp: A scratch directory for the fixture report and baseline.

    Returns:
        A list of failure descriptions, empty when both directions hold.
    """
    failures: list[str] = []
    baseline = tmp / "baseline.txt"
    report = tmp / "report.txt"
    write_baseline(baseline, {"broken_app": "FAULT"})

    # Direction 1 -- MUST PASS: the report matches the baseline exactly.
    report.write_text("good_app                   OK\nbroken_app                 FAULT\n")
    if check(report, baseline) != 0:
        failures.append("a report matching the baseline did not pass")

    # Direction 2 -- MUST FAIL: a second app has started faulting.
    report.write_text(
        "good_app                   FAULT\nbroken_app                 FAULT\n",
    )
    if check(report, baseline) == 0:
        failures.append("a NEWLY-FAULTING example did not fail the ratchet")

    # Direction 2b -- MUST FAIL: a truncated run is a non-verdict, not a pass.
    report.write_text("good_app                   TRUNCATED\nbroken_app                 FAULT\n")
    if check(report, baseline) == 0:
        failures.append("a TRUNCATED example did not fail the ratchet")

    # Direction 3 -- MUST PASS: debt burned down (shrinking is free).
    report.write_text("good_app                   OK\nbroken_app                 OK\n")
    if check(report, baseline) != 0:
        failures.append("burning debt down did not pass")

    # Direction 4 -- MUST FAIL: a missing report is never a silent pass.
    if check(tmp / "absent.txt", baseline) == 0:
        failures.append("a MISSING report passed instead of failing loudly")

    # Causes must SURVIVE a re-baseline. If --update strips them, the file
    # decays into a bare list of app names on the next burn-down and stops
    # being distinguishable from an allowlist.
    write_baseline(baseline, {"broken_app": "FAULT"}, {"broken_app": "a recorded reason"})
    if load_baseline_causes(baseline).get("broken_app") != "a recorded reason":
        failures.append("write_baseline() did not persist the cause column")
    report.write_text("broken_app                 FAULT\nother_app                  FAULT\n")
    update(report, baseline)
    kept = load_baseline_causes(baseline)
    if kept.get("broken_app") != "a recorded reason":
        failures.append("--update dropped the recorded cause of a still-failing app")
    if load_baseline(baseline).get("other_app") != "FAULT":
        failures.append("--update did not record a newly-failing app")
    return failures


def _selftest_attestation(tmp: Path) -> list[str]:
    """Assert a hand-edited baseline is rejected and a written one accepted.

    Every case runs against a throwaway fixture, never the committed file: a
    selftest that mutated `.github/emulator-matrix-baseline.txt` would be the
    very hand edit this asserts against.

    Args:
        tmp: A scratch directory for the fixture baseline.

    Returns:
        A list of failure descriptions, empty when all cases hold.
    """
    failures: list[str] = []
    fixture = tmp / "attest.txt"
    debt = {"usb_x": "FAULT", "blink": "TRUNCATED"}
    causes = {"usb_x": "ra8_emulator has no USBHS model (#170)"}

    # Case 1 -- MUST PASS: a file this tool wrote, cause column and all.
    write_baseline(fixture, debt, causes)
    if attest_baseline(fixture):
        failures.append("a tool-written baseline did not attest clean")
    if load_baseline_causes(fixture).get("usb_x") != causes["usb_x"]:
        failures.append("the rendered cause column did not survive a round trip")

    canonical = fixture.read_text(encoding="utf-8")
    rows = canonical.splitlines()

    # Case 2 -- MUST FAIL: the whole-file `sort` that started #712.
    fixture.write_text("\n".join(sorted(rows)) + "\n", encoding="utf-8")
    if not attest_baseline(fixture):
        failures.append("a whole-file sort attested clean")

    # Case 3 -- MUST FAIL: a hand-written row left where the typist put it.
    fixture.write_text("\n".join([*rows[:-2], "zz_typed\tFAULT", *rows[-2:]]) + "\n")
    if not attest_baseline(fixture):
        failures.append("an out-of-order hand-written row attested clean")

    # Case 4 -- MUST FAIL: the `# total:` line edited to match a hand change.
    fixture.write_text(canonical.replace("# total: 2", "# total: 3"), encoding="utf-8")
    if not attest_baseline(fixture):
        failures.append("a wrong # total: line attested clean")

    # Case 5 -- MUST FAIL: a non-debt verdict, which --update never records.
    fixture.write_text(canonical.replace("blink\tTRUNCATED", "blink\tOK"), encoding="utf-8")
    if not attest_baseline(fixture):
        failures.append("an OK row -- impossible from --update -- attested clean")
    return failures


def selftest() -> int:
    """Assert the gate fires in both directions before it is trusted.

    Returns:
        A process exit status: 0 when every assertion holds, 1 otherwise.
    """
    import tempfile  # noqa: PLC0415  # selftest-only; not a runtime dependency

    failures = _selftest_classification()
    with tempfile.TemporaryDirectory() as td:
        failures.extend(_selftest_ratchet(Path(td)))
        failures.extend(_selftest_attestation(Path(td)))
    if failures:
        sys.stderr.write("matrix_ratchet.py --selftest: FAILED\n")
        for line in failures:
            sys.stderr.write(f"  {line}\n")
        return 1
    print(
        "matrix_ratchet.py --selftest: OK (fires on growth, quiet on shrinkage; "
        f"{ATTESTATION_CASES} attestation cases)"
    )
    return 0


def main() -> int:
    """Parse the mode argument and dispatch.

    Returns:
        A process exit status.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="gate against the baseline (default)")
    mode.add_argument("--update", action="store_true", help="rewrite the baseline")
    mode.add_argument("--selftest", action="store_true", help="assert the gate itself fires")
    mode.add_argument(
        "--attest", action="store_true", help="assert the baseline is machine-written"
    )
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if args.attest:
        problems = attest_baseline()
        if problems:
            report_attestation(problems)
            return 1
        print("matrix_ratchet.py: baseline attested -- byte-identical to what --update emits.")
        return 0
    if args.update:
        return update()
    return check()


if __name__ == "__main__":
    sys.exit(main())
