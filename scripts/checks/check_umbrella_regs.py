#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: the RA8D2 register umbrella shall re-export every register header.

``libs/ra8_hal/inc/ra8_regs.h`` publishes itself as the one include that hands
a consumer every peripheral register block on the RA8D2.  Nothing in the tree
had to include it for that claim to be believed, and nothing did, so the claim
was never tested by a build: by #1389 the umbrella re-exported 30 of the 60
eligible headers and half the chip had fallen out of it unnoticed.

This gate makes the claim checkable.  Three rules, all reported per header:

``missing-re-export``
    A ``ra8_*_regs.h`` in ``libs/ra8_hal/inc/`` that the umbrella does not
    include and that is not one of the documented exclusions below.  This is
    the rule that would have caught #1389.

``stale-re-export``
    The umbrella includes a header that no longer exists in that directory,
    which breaks every consumer of the umbrella at once.

``excluded-and-included``
    A documented exclusion that the umbrella re-exports anyway.  ``ra8_npu_regs.h``
    is the sharp one: it ``#error``s out on a part without an NPU, so including
    it does not merely widen the umbrella, it breaks every RA8D2 build that
    pulls the umbrella in.

WHAT THIS DOES NOT CHECK
------------------------
It does not ask whether the umbrella COMPILES: that is the job of the unit
test that includes it (``tests/hal/src/test_ra8_regs_umbrella.c``), because
only a compiler can answer it.  It does not ask which drivers include the
umbrella, because they deliberately should not: a driver includes the narrow
header for the block it drives.

EXCLUSIONS
----------
Each entry carries the reason it is not an RA8D2 peripheral block.  There is
deliberately no in-file waiver marker: widening the umbrella's scope is a
decision that belongs in this table, in review, not in a comment.

Run::

    check_umbrella_regs.py             # check the tree
    check_umbrella_regs.py --selftest  # prove every rule fires and stays quiet

Exit 0 when the umbrella is complete, exit 1 (with a table) otherwise.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# The umbrella, and the directory whose register headers it must re-export.
REGS_DIR_REL = "libs/ra8_hal/inc"
UMBRELLA_NAME = "ra8_regs.h"

# Headers that live beside the umbrella but are out of its stated scope.
# name -> why it is not a peripheral register block of the RA8D2.
EXCLUSIONS: dict[str, str] = {
    "ra8_npu_regs.h": (
        "Ethos-U55 NPU window on the RA8P1, not the RA8D2; the header #errors "
        "out on a part without an NPU, so the umbrella cannot carry it"
    ),
    "ra8_touch_gt911_regs.h": (
        "off-chip GoodIX GT911 touch controller reached over I2C, not a "
        "register block of this MCU"
    ),
}

# A whole-tree pass below this measured population has lost scope and must fail.
MIN_REGS_HEADERS = 40

_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)


def _regs_headers(regs_dir: Path) -> list[str]:
    """Every ``ra8_*_regs.h`` beside the umbrella, the umbrella excepted."""
    return sorted(
        path.name
        for path in regs_dir.glob("ra8_*_regs.h")
        if path.name != UMBRELLA_NAME
    )


def _re_exported(umbrella: Path) -> list[str]:
    """Header names the umbrella includes, in file order."""
    text = umbrella.read_text(encoding="utf-8", errors="ignore")
    return _INCLUDE_RE.findall(text)


def _audit(regs_dir: Path) -> tuple[int, list[tuple[str, str]]]:
    """Return the header population and every (rule, header) finding."""
    umbrella = regs_dir / UMBRELLA_NAME
    present = _regs_headers(regs_dir)
    exported = _re_exported(umbrella)
    exported_set = set(exported)

    findings: list[tuple[str, str]] = []
    for name in present:
        if name in EXCLUSIONS:
            if name in exported_set:
                findings.append(("excluded-and-included", name))
            continue
        if name not in exported_set:
            findings.append(("missing-re-export", name))
    for name in exported:
        if not (regs_dir / name).is_file():
            findings.append(("stale-re-export", name))
    return len(present), sorted(set(findings))


def _report(findings: list[tuple[str, str]]) -> None:
    """Print one line per finding, widest rule name first for alignment."""
    width = max(len(rule) for rule, _ in findings)
    print(f"{REGS_DIR_REL}/{UMBRELLA_NAME}: umbrella is not complete", file=sys.stderr)
    for rule, name in findings:
        detail = EXCLUSIONS.get(name, "")
        suffix = f"  ({detail})" if rule == "excluded-and-included" and detail else ""
        print(f"  [{rule.ljust(width)}] {name}{suffix}", file=sys.stderr)
    print(
        "  fix: add the header to the matching domain group in "
        f"{REGS_DIR_REL}/{UMBRELLA_NAME}, or give it an entry in EXCLUSIONS "
        "with the reason it is not an RA8D2 block.",
        file=sys.stderr,
    )


def selftest() -> int:
    """Prove each rule fires on a planted fault and stays quiet when clean."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-umbrella-regs-") as temp:
        regs_dir = Path(temp)
        for name in ("ra8_cgc_regs.h", "ra8_spi_regs.h", *EXCLUSIONS):
            (regs_dir / name).write_text("#pragma once\n", encoding="ascii")
        umbrella = regs_dir / UMBRELLA_NAME

        umbrella.write_text(
            '#pragma once\n#include "ra8_cgc_regs.h"\n#include "ra8_spi_regs.h"\n',
            encoding="ascii",
        )
        population, findings = _audit(regs_dir)
        if findings:
            failures.append(f"complete umbrella did not stay quiet: {findings!r}")
        if population != 2 + len(EXCLUSIONS):
            failures.append(f"population miscounted: {population}")

        umbrella.write_text('#pragma once\n#include "ra8_cgc_regs.h"\n', encoding="ascii")
        _, findings = _audit(regs_dir)
        if findings != [("missing-re-export", "ra8_spi_regs.h")]:
            failures.append(f"missing-re-export did not fire alone: {findings!r}")

        umbrella.write_text(
            '#pragma once\n#include "ra8_cgc_regs.h"\n#include "ra8_spi_regs.h"\n'
            '#include "ra8_deleted_regs.h"\n',
            encoding="ascii",
        )
        _, findings = _audit(regs_dir)
        if findings != [("stale-re-export", "ra8_deleted_regs.h")]:
            failures.append(f"stale-re-export did not fire alone: {findings!r}")

        excluded = next(iter(EXCLUSIONS))
        umbrella.write_text(
            '#pragma once\n#include "ra8_cgc_regs.h"\n#include "ra8_spi_regs.h"\n'
            f'#include "{excluded}"\n',
            encoding="ascii",
        )
        _, findings = _audit(regs_dir)
        if findings != [("excluded-and-included", excluded)]:
            failures.append(f"excluded-and-included did not fire alone: {findings!r}")

    if failures:
        for failure in failures:
            print(f"  [FAIL] {failure}", file=sys.stderr)
        return 1
    print("check_umbrella_regs.py --selftest: PASS (quiet, missing, stale, excluded)")
    return 0


def _parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the CLI so a misspelled option fails instead of being ignored."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true")
    return parser.parse_args(argv[1:])


def main(argv: list[str]) -> int:
    """Fail when the umbrella does not re-export every eligible register header."""
    args = _parse_args(argv)
    if args.selftest:
        return selftest()

    regs_dir = REPO_ROOT / REGS_DIR_REL
    umbrella = regs_dir / UMBRELLA_NAME
    if not umbrella.is_file():
        print(f"{REGS_DIR_REL}/{UMBRELLA_NAME}: missing", file=sys.stderr)
        return 1

    population, findings = _audit(regs_dir)
    if population < MIN_REGS_HEADERS:
        print(
            f"{REGS_DIR_REL}: scanned {population} register headers, below the "
            f"{MIN_REGS_HEADERS} floor; the scan has lost its scope",
            file=sys.stderr,
        )
        return 1
    if findings:
        _report(findings)
        return 1

    covered = population - len(EXCLUSIONS)
    print(
        f"check_umbrella_regs.py: PASS ({covered} register headers re-exported, "
        f"{len(EXCLUSIONS)} documented exclusions)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
