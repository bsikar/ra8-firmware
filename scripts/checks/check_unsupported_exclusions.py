#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate the ``examples/_unsupported/`` tier's machine-readable exclusion reasons.

Every app parked there shall carry a current, specific reason a machine can read.

``examples/_unsupported/`` is the tier nothing in CI flashes. Until now its
exclusions were asserted by the directory name and by prose in each app's own
README, so a new app could be parked there silently and a stale justification
could survive a refactor unnoticed -- the emulator matrix printed one blanket
``needs external hardware`` line for the whole tier regardless of what each app
was actually waiting for, and for at least two of the six that line is simply
untrue (#401).

Each app therefore carries an ``UNSUPPORTED.toml`` marker naming its reason from
a bounded taxonomy, plus the file that evidences it. This gate proves the set of
markers and the set of app directories are the SAME set in both directions: an
app with no marker fails, and a marker with no app fails. A one-directional
check would pass forever the day someone adds an app and forgets the marker,
which is the exact failure mode this tier already had.

This gate does not decide whether an exclusion is still justified -- that is a
bench verdict, and re-tiering is deliberately out of scope here. It only proves
that a specific, readable reason exists for every excluded app.

Run::

    check_unsupported_exclusions.py             # scan the tier
    check_unsupported_exclusions.py --selftest  # prove both directions

Exit 0 when every app is marked and every marker is well formed, 1 on any
finding, 2 when the sweep collapses below APP_FLOOR (a read that saw nothing
must not report success).
"""

from __future__ import annotations

import argparse
import sys
import tempfile
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from selftest_assert import expect, report

REPO_ROOT = Path(__file__).resolve().parents[2]

TIER_DIR = "examples/_unsupported"
"""The excluded tier this gate covers."""

MARKER_NAME = "UNSUPPORTED.toml"
# The selftest fixture tier: named here so the discovery assertion counts the
# fixture rather than a number written twice.
FIXTURE_APPS = ("demo", "demo2", "demo3", "demo4")
"""Per-app marker filename."""

APP_FLOOR = 4
"""Fewest apps a healthy sweep may see. The tier held six when this gate landed;
a read that finds fewer than this has collapsed (wrong cwd, sparse checkout,
renamed tier) and is fatal rather than clean."""

REASONS = frozenset(
    {
        "external-hardware",
        "companion-radio",
        "host-side-rig",
        "onboard-routing-conflict",
        "removable-media",
        "human-observation",
    }
)
"""The bounded reason taxonomy. Adding a member is a deliberate edit here, so a
typo'd or invented reason fails instead of quietly becoming a seventh category."""

REQUIRED_KEYS = ("app", "reason", "also", "needs_extra_hardware", "evidence", "detail")
"""Keys every marker must carry."""

DETAIL_FLOOR = 40
"""Shortest useful ``detail``. "needs hardware" is the non-reason this gate exists
to reject."""


def app_dirs(root: Path) -> list[Path]:
    """Return every app directory in the tier, identified by its ``src/main.c``."""
    tier = root / TIER_DIR
    if not tier.is_dir():
        return []
    return sorted({p.parent.parent for p in tier.glob("*/src/main.c")})


def marker_files(root: Path) -> list[Path]:
    """Return every marker file in the tier, including orphans."""
    tier = root / TIER_DIR
    if not tier.is_dir():
        return []
    return sorted(tier.glob(f"*/{MARKER_NAME}"))


def _evidence_exists(marker: Path, evidence: str) -> bool:
    """True when ``evidence`` resolves from the repo root or from the tier's root."""
    tier_root = marker.parent.parent.parent
    return (REPO_ROOT / evidence).exists() or (tier_root / evidence).exists()


def check_marker(path: Path, app_name: str) -> list[str]:
    """Return the findings for one marker file (empty when it is well formed)."""
    findings: list[str] = []
    try:
        data = tomllib.loads(path.read_text())
    except (tomllib.TOMLDecodeError, UnicodeDecodeError) as exc:
        return [f"{path}: unparseable -- {exc}"]

    findings += [
        f"{path}: missing required key '{key}'" for key in REQUIRED_KEYS if key not in data
    ]
    if findings:
        return findings

    if data["app"] != app_name:
        findings.append(f"{path}: app = {data['app']!r} does not match directory {app_name!r}")
    if data["reason"] not in REASONS:
        findings.append(
            f"{path}: reason = {data['reason']!r} is not in the taxonomy "
            f"({' '.join(sorted(REASONS))})"
        )
    if not isinstance(data["also"], list) or any(r not in REASONS for r in data["also"]):
        findings.append(f"{path}: also = {data['also']!r} must be a list of taxonomy reasons")
    if data["reason"] in data["also"]:
        findings.append(f"{path}: reason {data['reason']!r} repeated in 'also'")
    if not isinstance(data["needs_extra_hardware"], bool):
        findings.append(f"{path}: needs_extra_hardware must be a boolean")
    detail = str(data["detail"]).strip()
    if len(detail) < DETAIL_FLOOR:
        findings.append(
            f"{path}: detail is {len(detail)} char(s); a reason shorter than "
            f"{DETAIL_FLOOR} is a label, not an explanation"
        )
    evidence = str(data["evidence"]).strip()
    if not evidence:
        findings.append(f"{path}: evidence is empty")
    elif not _evidence_exists(path, evidence):
        findings.append(f"{path}: evidence path {evidence!r} does not exist")
    return findings


def scan(root: Path) -> tuple[list[str], int]:
    """Return (findings, app_count) for the tier under ``root``."""
    apps = app_dirs(root)
    markers = marker_files(root)
    findings: list[str] = []

    marked = {p.parent.name for p in markers}
    findings += [
        f"{app.relative_to(root)}: no {MARKER_NAME} -- an app cannot be parked in "
        "the excluded tier without a stated reason"
        for app in apps
        if app.name not in marked
    ]
    for marker in markers:
        if marker.parent.name not in {a.name for a in apps}:
            findings.append(
                f"{marker.relative_to(root)}: orphan marker -- no src/main.c under this directory"
            )
            continue
        findings.extend(check_marker(marker, marker.parent.name))
    return findings, len(apps)


def selftest() -> int:
    """Prove the gate fires on a broken tier and stays quiet on a good one."""
    failures: list[str] = []
    good = (
        'app = "demo"\nreason = "external-hardware"\nalso = []\n'
        'needs_extra_hardware = true\nevidence = "README.md"\n'
        'detail = "Needs a gate-driver IC this project does not own, '
        'so nothing in CI can run it."\n'
    )
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        tier = root / TIER_DIR
        for name in FIXTURE_APPS:
            (tier / name / "src").mkdir(parents=True)
            (tier / name / "src" / "main.c").write_text("int main(void) { return 0; }\n")
            (tier / name / "README.md").write_text("x\n")
            (tier / name / MARKER_NAME).write_text(good.replace('"demo"', f'"{name}"'))
        findings, count = scan(root)
        expect(not findings, "a fully marked tier is quiet", failures)
        expect(count == len(FIXTURE_APPS), "every app directory is discovered", failures)

        (tier / "demo2" / MARKER_NAME).unlink()
        findings, _ = scan(root)
        expect(
            any("no UNSUPPORTED.toml" in f for f in findings), "a missing marker fires", failures
        )
        (tier / "demo2" / MARKER_NAME).write_text(good.replace('"demo"', '"demo2"'))

        # A marker that parses but omits a required key. Nothing else in this
        # selftest exercised REQUIRED_KEYS, so the missing-key direction could
        # be disabled outright and every other assertion would still hold.
        for key in REQUIRED_KEYS:
            lines = good.replace('"demo"', '"demo2"').splitlines(keepends=True)
            (tier / "demo2" / MARKER_NAME).write_text(
                "".join(line for line in lines if not line.startswith(f"{key} "))
            )
            findings, _ = scan(root)
            expect(
                any(f"missing required key '{key}'" in f for f in findings),
                f"a marker missing '{key}' fires",
                failures,
            )
        (tier / "demo2" / MARKER_NAME).write_text(good.replace('"demo"', '"demo2"'))

        (tier / "demo3" / MARKER_NAME).write_text(
            good.replace('"demo"', '"demo3"').replace("external-hardware", "because-reasons")
        )
        findings, _ = scan(root)
        expect(
            any("not in the taxonomy" in f for f in findings), "an invented reason fires", failures
        )

        (tier / "demo3" / MARKER_NAME).write_text(
            good.replace('"demo"', '"demo3"').replace(
                "Needs a gate-driver IC this project does not own, so nothing in CI can run it.",
                "needs hardware",
            )
        )
        findings, _ = scan(root)
        expect(any("not an explanation" in f for f in findings), "a stub detail fires", failures)
        (tier / "demo3" / MARKER_NAME).write_text(good.replace('"demo"', '"demo3"'))

        (tier / "demo4" / MARKER_NAME).write_text(good.replace('"demo"', '"wrong_name"'))
        findings, _ = scan(root)
        expect(
            any("does not match directory" in f for f in findings),
            "a marker naming another app fires",
            failures,
        )
        (tier / "demo4" / MARKER_NAME).write_text(good.replace('"demo"', '"demo4"'))

        (tier / "orphan").mkdir()
        (tier / "orphan" / MARKER_NAME).write_text(good.replace('"demo"', '"orphan"'))
        findings, _ = scan(root)
        expect(any("orphan marker" in f for f in findings), "a marker with no app fires", failures)
    return report(failures)


def main(argv: list[str]) -> int:
    """Prove every excluded app states a current, specific reason.

    Returns 0 when the tier is fully and validly marked, 1 on findings, 2 when
    the sweep collapsed below ``APP_FLOOR``.
    """
    ap = argparse.ArgumentParser(description="Validate examples/_unsupported exclusion markers")
    ap.add_argument("--selftest", action="store_true", help="assert both directions")
    args = ap.parse_args(argv[1:])
    if args.selftest:
        return selftest()

    findings, count = scan(REPO_ROOT)
    if count < APP_FLOOR:
        print(
            f"check_unsupported_exclusions.py: FATAL -- only {count} app(s) discovered under "
            f"{TIER_DIR}, floor is {APP_FLOOR}. A collapsed read reports success because it "
            "saw nothing.",
            file=sys.stderr,
        )
        return 2
    if findings:
        print(
            f"\n{len(findings)} exclusion-marker finding(s) under {TIER_DIR}:\n",
            file=sys.stderr,
        )
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print(f"check_unsupported_exclusions.py: {count} excluded app(s), every reason stated.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
