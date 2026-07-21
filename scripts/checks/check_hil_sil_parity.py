#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: SIM==HIL parity -- every HIL app is also exercised in the simulator.

The owner invariant this gate enforces: **every hardware-in-the-loop (HIL) app
MUST also be exercised in the simulator (SIL), automatically and dynamically**,
so that *adding* a HIL app cannot silently escape SIM coverage.

Two harnesses already exist:

  * ``scripts/hil/all.sh`` flashes the physical EK-RA8D2 and scrapes its UART /
    J-Link / RTT / wire for every app under
    ``examples/ek_ra8d2/hw_validated/hil/``.
  * ``scripts/sim/sil_all.sh`` boots the SAME ``.elf`` files in ``tools/board_sim``
    headless and checks the SAME per-app ``hil.conf`` expectations with NO board
    attached.  The ``sil-integration`` CI job runs it enforcing, 0 skips.

Nothing, however, GUARANTEED that the two suites cover the same set of apps or
that a newly-added HIL app is SIL-visible.  A HIL app whose ``hil.conf`` is
missing, or whose ``HIL_MODE`` ``board_sim`` cannot check, would quietly fall
out of SIL coverage -- exactly the drift this gate makes impossible.

It is a CHEAP, hardware-free structural gate: it does not build or run either
harness.  It re-derives each harness's app-discovery *from the harness scripts
themselves* (parsing ``HIL_DIR`` / ``SIL_RA8P1_DIR`` and the SIL-capable-mode
``case`` list out of ``sil_all.sh``, ``HIL_DIR`` out of ``hil_all.sh``) and then
globs the tree exactly as those scripts do.  Because the roots and the mode list
are parsed, not hardcoded, the gate tracks the harnesses as they evolve.

It FAILS (non-zero) if any of the following hold, with a precise message per
offender:

  1. an app under ``examples/ek_ra8d2/hw_validated/hil/`` has no ``hil.conf``
     (it is HIL-tiered but unspecified -- ``hil_all.sh`` fails loud on it, and
     ``board_sim`` never sees it);
  2. the set of apps ``hil_all.sh`` would run is not covered by the set
     ``sil_all.sh`` would run -- drift between the two harnesses.  ``sil_all.sh``
     legitimately runs a SUPERSET (it also emulates the RA8P1/NPU foundation
     apps under ``examples/ra8p1_foundation/`` via ``--device ra8p1``, which the
     RA8D2 bench cannot flash), so the relation checked is HIL subset-of SIL:
     every HIL app must be in the SIL run set;
  3. any ``hw_validated/hil`` app declares a ``HIL_MODE`` that ``sil_all.sh``
     cannot check -- i.e. one that is not in its SIL-capable set
     (``uart_scrape`` / ``alive`` / ``jlink_memprobe`` / ``rtt_scrape`` /
     ``hil_eth_tcp``, parsed from ``sil_all.sh``).  There must be NO SIL skips.

Run::

    check_hil_sil_parity.py            # gate (exit 1 on any violation)
    check_hil_sil_parity.py --list     # enumerate the derived sets, no gate

Exit 0 if the SIM==HIL invariant holds, exit 1 otherwise, exit 2 if the harness
scripts could not be parsed (a structural change that must be reconciled -- the
gate refuses to silently no-op).
"""

# ---------------------------------------------------------------------------
# SIM==HIL is an ONGOING DISCIPLINE, not a one-time state.
#
# "SIM==HIL" means board_sim (tools/board_sim) must reproduce the ACTUAL
# silicon behaviour of every path a HIL app exercises -- bugs INCLUDED, not an
# idealised model.  When a new HIL app drives a peripheral or code path that
# board_sim does not yet model, the correct response is NOT to skip it in SIL:
# board_sim MUST be extended so the SIL suite stays complete.  This gate makes
# skipping impossible (a missing hil.conf, an un-modelled HIL_MODE, or an app
# invisible to sil_all.sh all fail the build), which is precisely what FORCES
# the simulator to keep up with the hardware.  Concretely, the upcoming
# archive / compression work may need new board_sim SD / decode models before
# its HIL apps can pass SIL -- and that is by design: the sim follows silicon.
# ---------------------------------------------------------------------------

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

HIL_ALL = REPO_ROOT / "scripts" / "hil" / "all.sh"
SIL_ALL = REPO_ROOT / "scripts" / "sim" / "sil_all.sh"

# hil_discover_apps() skips this file when walking a hil/ directory.
DISCOVERY_SKIP = "README.md"

# The sil_all.sh line whose case-labels are the SIL-capable modes lives right
# after this dispatch header (in run_one()).
SIL_MODE_CASE_HEADER = 'case "${HIL_MODE:-}" in'


# ---------------------------------------------------------------------------
# Harness-script parsing (all failures are collected as strings, never raised,
# so a structural change reports a precise reconcile message and exit 2 rather
# than an unhandled traceback).
# ---------------------------------------------------------------------------
def _read(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8")
    except OSError:
        return None


def _rel(path: Path) -> str:
    if path.is_relative_to(REPO_ROOT):
        return str(path.relative_to(REPO_ROOT))
    return str(path)


def _parse_repo_root_dir(text: str, var: str) -> Path | None:
    """Parse ``VAR="${REPO_ROOT}/<path>"`` and return REPO_ROOT / <path>.

    This is how both harnesses anchor their discovery roots, so parsing it (per
    harness) keeps the gate tracking a root the scripts may relocate.
    """
    pattern = re.compile(r"^\s*" + re.escape(var) + r'="\$\{REPO_ROOT\}/([^"]+)"', re.MULTILINE)
    match = pattern.search(text)
    if match is None:
        return None
    return REPO_ROOT / match.group(1)


def _parse_sil_modes(text: str) -> list[str] | None:
    """Parse the SIL-capable HIL_MODE set from sil_all.sh's run_one dispatch.

    The authoritative list is the ``case`` label right after
    ``case "${HIL_MODE:-}" in``::

        uart_scrape | alive | jlink_memprobe | rtt_scrape | hil_eth_tcp) : ;;

    Parsed (not hardcoded) so a mode added to the simulator is picked up here.
    Returns None if the dispatch or its label line cannot be found/parsed.
    """
    lines = text.splitlines()
    for idx, line in enumerate(lines):
        if SIL_MODE_CASE_HEADER not in line:
            continue
        for follow in lines[idx + 1 :]:
            stripped = follow.strip()
            if not stripped or stripped.startswith("#"):
                continue
            match = re.match(r"^([a-z0-9_ |]+)\)\s*:\s*;;\s*$", stripped)
            if match is None:
                return None
            return [tok.strip() for tok in match.group(1).split("|") if tok.strip()]
    return None


def _discover_apps(root: Path) -> list[str]:
    """App names directly under a hil/ root -- mirrors hil_discover_apps().

    An app is an immediate child DIRECTORY; the README.md file is skipped.
    Returned sorted, matching the harness's ``| sort``.
    """
    if not root.is_dir():
        return []
    names = [c.name for c in root.iterdir() if c.is_dir() and c.name != DISCOVERY_SKIP]
    return sorted(names)


def _hil_mode_of(conf: Path) -> str | None:
    """Parse ``HIL_MODE=<mode>`` from a hil.conf (quotes stripped)."""
    if not conf.is_file():
        return None
    text = _read(conf)
    if text is None:
        return None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("HIL_MODE="):
            return line[len("HIL_MODE=") :].strip().strip('"').strip("'")
    return None


@dataclass
class Model:
    """Everything the checks need, derived once from the two harness scripts."""

    hil_root_hilall: Path
    hil_root_silall: Path
    ra8p1_root: Path
    sil_modes: list[str]
    hil_apps: list[str]
    ra8p1_apps: list[str]
    sil_apps: list[str]


def build_model() -> tuple[Model | None, list[str]]:
    """Derive the two harnesses' discovery, or a list of parse-error strings."""
    errors: list[str] = []
    hil_text = _read(HIL_ALL)
    sil_text = _read(SIL_ALL)
    if hil_text is None:
        errors.append(f"cannot read {_rel(HIL_ALL)}")
    if sil_text is None:
        errors.append(f"cannot read {_rel(SIL_ALL)}")
    if hil_text is None or sil_text is None:
        return None, errors

    hil_root_hilall = _parse_repo_root_dir(hil_text, "HIL_DIR")
    hil_root_silall = _parse_repo_root_dir(sil_text, "HIL_DIR")
    ra8p1_root = _parse_repo_root_dir(sil_text, "SIL_RA8P1_DIR")
    sil_modes = _parse_sil_modes(sil_text)
    if hil_root_hilall is None:
        errors.append(f'{_rel(HIL_ALL)}: no HIL_DIR="${{REPO_ROOT}}/..." assignment')
    if hil_root_silall is None:
        errors.append(f'{_rel(SIL_ALL)}: no HIL_DIR="${{REPO_ROOT}}/..." assignment')
    if ra8p1_root is None:
        errors.append(f'{_rel(SIL_ALL)}: no SIL_RA8P1_DIR="${{REPO_ROOT}}/..." assignment')
    if sil_modes is None:
        errors.append(
            f"{_rel(SIL_ALL)}: could not parse the SIL-capable mode set from the "
            f"'{SIL_MODE_CASE_HEADER}' dispatch"
        )
    if None in (hil_root_hilall, hil_root_silall, ra8p1_root, sil_modes):
        return None, errors

    hil_apps = _discover_apps(hil_root_hilall)
    sil = set(_discover_apps(hil_root_silall))
    ra8p1_apps = [
        name for name in _discover_apps(ra8p1_root) if (ra8p1_root / name / "hil.conf").is_file()
    ]
    sil.update(ra8p1_apps)
    model = Model(
        hil_root_hilall=hil_root_hilall,
        hil_root_silall=hil_root_silall,
        ra8p1_root=ra8p1_root,
        sil_modes=sil_modes,
        hil_apps=hil_apps,
        ra8p1_apps=ra8p1_apps,
        sil_apps=sorted(sil),
    )
    return model, []


# ---------------------------------------------------------------------------
# Per-offender message builders (kept out of the comprehensions below so the
# checks stay one readable expression each).
# ---------------------------------------------------------------------------
def _msg_no_conf(appdir_rel: str) -> str:
    return (
        f"{appdir_rel}: no hil.conf -- a hil/ app is HIL-tiered but declares no "
        "HIL mode (hil_all.sh fails loud, board_sim never sees it). Add a "
        "hil.conf or move it to manual/."
    )


def _msg_escapes_sim(app: str) -> str:
    return (
        f"{app}: hil_all.sh would run it but sil_all.sh would not -- the HIL app "
        "escapes SIM coverage. Ensure sil_all.sh discovers it (same hil/ root) so "
        "it is exercised in board_sim too."
    )


def _msg_root_drift(model: Model) -> str:
    return (
        "hil_all.sh and sil_all.sh discover DIFFERENT hil/ roots "
        f"({_rel(model.hil_root_hilall)} vs {_rel(model.hil_root_silall)}) -- the "
        "two harnesses have drifted; point both HIL_DIR at the same root."
    )


def _msg_bad_mode(app: str, mode: str, capable: set[str]) -> str:
    return (
        f"{app}: HIL_MODE='{mode}' is not SIL-checkable (sil_all.sh checks only: "
        f"{', '.join(sorted(capable))}). board_sim would SKIP it -- there must be "
        "NO SIL skips. Model the mode in board_sim + sil_all.sh, do not leave it "
        "hardware-only."
    )


def check_missing_conf(model: Model) -> list[str]:
    """Check 1: every hil/ app must declare a hil.conf."""
    return [
        _msg_no_conf(_rel(model.hil_root_hilall / app))
        for app in model.hil_apps
        if not (model.hil_root_hilall / app / "hil.conf").is_file()
    ]


def check_set_drift(model: Model) -> list[str]:
    """Check 2: hil_all's run set must be covered by sil_all's run set."""
    offenders: list[str] = []
    if model.hil_root_hilall != model.hil_root_silall:
        offenders.append(_msg_root_drift(model))
    sil_set = set(model.sil_apps)
    offenders.extend(_msg_escapes_sim(app) for app in model.hil_apps if app not in sil_set)
    return offenders


def check_unsupported_mode(model: Model) -> list[str]:
    """Check 3: every hil/ app's HIL_MODE must be SIL-checkable (no skips)."""
    capable = set(model.sil_modes)
    offenders: list[str] = []
    for app in model.hil_apps:
        # A missing hil.conf / HIL_MODE is reported by check 1, not double-counted.
        mode = _hil_mode_of(model.hil_root_hilall / app / "hil.conf")
        if mode is not None and mode not in capable:
            offenders.append(_msg_bad_mode(app, mode, capable))
    return offenders


def _print_list(model: Model) -> None:
    print("check_hil_sil_parity.py --list")
    print("-------------------------------------------------------------------")
    print(f"hil_all.sh HIL_DIR      : {_rel(model.hil_root_hilall)}")
    print(f"sil_all.sh HIL_DIR      : {_rel(model.hil_root_silall)}")
    print(f"sil_all.sh SIL_RA8P1_DIR: {_rel(model.ra8p1_root)}")
    print(f"SIL-capable HIL_MODEs   : {', '.join(model.sil_modes)}")
    print(f"hil_all.sh run set      : {len(model.hil_apps)} app(s)")
    print(f"sil_all.sh run set      : {len(model.sil_apps)} app(s)")
    print(f"  SIL-only (RA8P1)      : {', '.join(model.ra8p1_apps) or '(none)'}")
    print("-------------------------------------------------------------------")
    print(f"{'APP':<34} {'MODE':<16} {'IN SIL':<7}")
    sil_set = set(model.sil_apps)
    for app in model.hil_apps:
        mode = _hil_mode_of(model.hil_root_hilall / app / "hil.conf") or "(no hil.conf)"
        in_sil = "yes" if app in sil_set else "NO"
        print(f"{app:<34} {mode:<16} {in_sil:<7}")


def _report_parse_errors(errors: list[str]) -> int:
    print("check_hil_sil_parity.py: could not parse the HIL/SIL harness scripts:", file=sys.stderr)
    for err in errors:
        print(f"  - {err}", file=sys.stderr)
    print(
        "\nThe harness scripts changed shape and the gate can no longer derive the "
        "discovery rules. Reconcile the parse -- do not bypass the gate.",
        file=sys.stderr,
    )
    return 2


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="check_hil_sil_parity.py",
        description="Gate: every HIL app is also exercised in the simulator (SIM==HIL).",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="enumerate the derived hil/sil sets + modes, then exit 0 (no gating).",
    )
    args = parser.parse_args(argv[1:])

    model, errors = build_model()
    if model is None:
        return _report_parse_errors(errors)

    if args.list:
        _print_list(model)
        return 0

    offenders = check_missing_conf(model) + check_set_drift(model) + check_unsupported_mode(model)
    if not offenders:
        print(
            f"check_hil_sil_parity.py: SIM==HIL holds -- {len(model.hil_apps)} HIL "
            "app(s), all with a hil.conf, all in a SIL-checkable mode "
            f"({', '.join(model.sil_modes)}), all covered by sil_all.sh "
            f"({len(model.sil_apps)} SIL app(s) incl. {len(model.ra8p1_apps)} RA8P1)."
        )
        return 0

    print(
        f"check_hil_sil_parity.py: {len(offenders)} SIM==HIL parity violation(s):\n",
        file=sys.stderr,
    )
    for offender in offenders:
        print(f"  - {offender}", file=sys.stderr)
    print(
        "\nSIM==HIL is an ongoing discipline: board_sim must reproduce the actual\n"
        "silicon behaviour of every HIL path (bugs included). A HIL app may never\n"
        "be skipped in SIL -- if board_sim cannot yet model a path, extend\n"
        "board_sim + sil_all.sh so SIL stays complete. See the header comment.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
