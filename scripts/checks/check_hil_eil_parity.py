#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: EIL==HIL parity -- every HIL app is also exercised in the emulator.

The owner invariant this gate enforces: **every hardware-in-the-loop (HIL) app
MUST also be exercised in the emulator (EIL), automatically and dynamically**,
so that *adding* a HIL app cannot silently escape EIL coverage.

Two harnesses already exist:

  * ``scripts/hil/all.sh`` flashes the physical EK-RA8D2 and scrapes its UART /
    J-Link / RTT / wire for every app under
    ``examples/ek_ra8d2/hw_validated/hil/``.
  * ``scripts/emu/eil_all.sh`` boots the SAME ``.elf`` files in ``tools/ra8_emulator``
    headless and checks the SAME per-app ``hil.conf`` expectations with NO board
    attached.  The ``eil-integration`` CI job runs it enforcing, 0 skips.

Nothing, however, GUARANTEED that the two suites cover the same set of apps or
that a newly-added HIL app is EIL-visible.  A HIL app whose ``hil.conf`` is
missing, or whose ``HIL_MODE`` ``ra8_emulator`` cannot check, would quietly fall
out of EIL coverage -- exactly the drift this gate makes impossible.

It is a CHEAP, hardware-free structural gate: it does not build or run either
harness.  It re-derives each harness's app-discovery *from the harness scripts
themselves* (parsing ``HIL_DIR`` / ``EIL_RA8P1_DIR`` and the EIL-capable-mode
``case`` list out of ``eil_all.sh``, ``HIL_DIR`` out of ``hil_all.sh``) and then
globs the tree exactly as those scripts do.  Because the roots and the mode list
are parsed, not hardcoded, the gate tracks the harnesses as they evolve.

It FAILS (non-zero) if any of the following hold, with a precise message per
offender:

  1. an app under ``examples/ek_ra8d2/hw_validated/hil/`` has no ``hil.conf``
     (it is HIL-tiered but unspecified -- ``hil_all.sh`` fails loud on it, and
     ``ra8_emulator`` never sees it);
  2. the set of apps ``hil_all.sh`` would run is not covered by the set
     ``eil_all.sh`` would run -- drift between the two harnesses.  ``eil_all.sh``
     legitimately runs a SUPERSET (it also emulates the RA8P1/NPU foundation
     apps under ``examples/ra8p1_foundation/`` via ``--device ra8p1``, which the
     RA8D2 bench cannot flash), so the relation checked is HIL subset-of EIL:
     every HIL app must be in the EIL run set;
  3. any ``hw_validated/hil`` app declares a ``HIL_MODE`` that ``eil_all.sh``
     cannot check -- i.e. one that is not in its EIL-capable set
     (``uart_scrape`` / ``alive`` / ``jlink_memprobe`` / ``rtt_scrape`` /
     ``hil_eth_tcp``, parsed from ``eil_all.sh``).  There must be NO EIL skips.

Run::

    check_hil_eil_parity.py            # gate (exit 1 on any violation)
    check_hil_eil_parity.py --list     # enumerate the derived sets, no gate

Exit 0 if the EIL==HIL invariant holds, exit 1 otherwise, exit 2 if the harness
scripts could not be parsed (a structural change that must be reconciled -- the
gate refuses to silently no-op).
"""

# ---------------------------------------------------------------------------
# EIL==HIL is an ONGOING DISCIPLINE, not a one-time state.
#
# "EIL==HIL" means ra8_emulator (tools/ra8_emulator) must reproduce the ACTUAL
# silicon behaviour of every path a HIL app exercises -- bugs INCLUDED, not an
# idealised model.  When a new HIL app drives a peripheral or code path that
# ra8_emulator does not yet model, the correct response is NOT to skip it in EIL:
# ra8_emulator MUST be extended so the EIL suite stays complete.  This gate makes
# skipping impossible (a missing hil.conf, an un-modelled HIL_MODE, or an app
# invisible to eil_all.sh all fail the build), which is precisely what FORCES
# the emulator to keep up with the hardware.  Concretely, the upcoming
# archive / compression work may need new ra8_emulator SD / decode models before
# its HIL apps can pass EIL -- and that is by design: the emulator follows silicon.
# ---------------------------------------------------------------------------

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

HIL_ALL = REPO_ROOT / "scripts" / "hil" / "all.sh"
EIL_ALL = REPO_ROOT / "scripts" / "emu" / "eil_all.sh"

# hil_discover_apps() skips this file when walking a hil/ directory.
DISCOVERY_SKIP = "README.md"

# The eil_all.sh line whose case-labels are the EIL-capable modes lives right
# after this dispatch header (in run_one()).
EIL_MODE_CASE_HEADER = 'case "${HIL_MODE:-}" in'


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


def _parse_eil_modes(text: str) -> list[str] | None:
    """Parse the EIL-capable HIL_MODE set from eil_all.sh's run_one dispatch.

    The authoritative list is the ``case`` label right after
    ``case "${HIL_MODE:-}" in``::

        uart_scrape | alive | jlink_memprobe | rtt_scrape | hil_eth_tcp) : ;;

    Parsed (not hardcoded) so a mode added to the emulator is picked up here.
    Returns None if the dispatch or its label line cannot be found/parsed.
    """
    lines = text.splitlines()
    for idx, line in enumerate(lines):
        if EIL_MODE_CASE_HEADER not in line:
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
    eil_modes: list[str]
    hil_apps: list[str]
    ra8p1_apps: list[str]
    eil_apps: list[str]


def build_model() -> tuple[Model | None, list[str]]:
    """Derive the two harnesses' discovery, or a list of parse-error strings."""
    errors: list[str] = []
    hil_text = _read(HIL_ALL)
    eil_text = _read(EIL_ALL)
    if hil_text is None:
        errors.append(f"cannot read {_rel(HIL_ALL)}")
    if eil_text is None:
        errors.append(f"cannot read {_rel(EIL_ALL)}")
    if hil_text is None or eil_text is None:
        return None, errors

    hil_root_hilall = _parse_repo_root_dir(hil_text, "HIL_DIR")
    hil_root_silall = _parse_repo_root_dir(eil_text, "HIL_DIR")
    ra8p1_root = _parse_repo_root_dir(eil_text, "EIL_RA8P1_DIR")
    eil_modes = _parse_eil_modes(eil_text)
    if hil_root_hilall is None:
        errors.append(f'{_rel(HIL_ALL)}: no HIL_DIR="${{REPO_ROOT}}/..." assignment')
    if hil_root_silall is None:
        errors.append(f'{_rel(EIL_ALL)}: no HIL_DIR="${{REPO_ROOT}}/..." assignment')
    if ra8p1_root is None:
        errors.append(f'{_rel(EIL_ALL)}: no EIL_RA8P1_DIR="${{REPO_ROOT}}/..." assignment')
    if eil_modes is None:
        errors.append(
            f"{_rel(EIL_ALL)}: could not parse the EIL-capable mode set from the "
            f"'{EIL_MODE_CASE_HEADER}' dispatch"
        )
    if None in (hil_root_hilall, hil_root_silall, ra8p1_root, eil_modes):
        return None, errors

    hil_apps = _discover_apps(hil_root_hilall)
    eil = set(_discover_apps(hil_root_silall))
    ra8p1_apps = [
        name for name in _discover_apps(ra8p1_root) if (ra8p1_root / name / "hil.conf").is_file()
    ]
    eil.update(ra8p1_apps)
    model = Model(
        hil_root_hilall=hil_root_hilall,
        hil_root_silall=hil_root_silall,
        ra8p1_root=ra8p1_root,
        eil_modes=eil_modes,
        hil_apps=hil_apps,
        ra8p1_apps=ra8p1_apps,
        eil_apps=sorted(eil),
    )
    return model, []


# ---------------------------------------------------------------------------
# Per-offender message builders (kept out of the comprehensions below so the
# checks stay one readable expression each).
# ---------------------------------------------------------------------------
def _msg_no_conf(appdir_rel: str) -> str:
    return (
        f"{appdir_rel}: no hil.conf -- a hil/ app is HIL-tiered but declares no "
        "HIL mode (hil_all.sh fails loud, ra8_emulator never sees it). Add a "
        "hil.conf or move it to manual/."
    )


def _msg_escapes_eil(app: str) -> str:
    return (
        f"{app}: hil_all.sh would run it but eil_all.sh would not -- the HIL app "
        "escapes EIL coverage. Ensure eil_all.sh discovers it (same hil/ root) so "
        "it is exercised in ra8_emulator too."
    )


def _msg_root_drift(model: Model) -> str:
    return (
        "hil_all.sh and eil_all.sh discover DIFFERENT hil/ roots "
        f"({_rel(model.hil_root_hilall)} vs {_rel(model.hil_root_silall)}) -- the "
        "two harnesses have drifted; point both HIL_DIR at the same root."
    )


def _msg_bad_mode(app: str, mode: str, capable: set[str]) -> str:
    return (
        f"{app}: HIL_MODE='{mode}' is not EIL-checkable (eil_all.sh checks only: "
        f"{', '.join(sorted(capable))}). ra8_emulator would SKIP it -- there must be "
        "NO EIL skips. Model the mode in ra8_emulator + eil_all.sh, do not leave it "
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
    """Check 2: hil_all's run set must be covered by eil_all's run set."""
    offenders: list[str] = []
    if model.hil_root_hilall != model.hil_root_silall:
        offenders.append(_msg_root_drift(model))
    eil_set = set(model.eil_apps)
    offenders.extend(_msg_escapes_eil(app) for app in model.hil_apps if app not in eil_set)
    return offenders


def check_unsupported_mode(model: Model) -> list[str]:
    """Check 3: every hil/ app's HIL_MODE must be EIL-checkable (no skips)."""
    capable = set(model.eil_modes)
    offenders: list[str] = []
    for app in model.hil_apps:
        # A missing hil.conf / HIL_MODE is reported by check 1, not double-counted.
        mode = _hil_mode_of(model.hil_root_hilall / app / "hil.conf")
        if mode is not None and mode not in capable:
            offenders.append(_msg_bad_mode(app, mode, capable))
    return offenders


def _print_list(model: Model) -> None:
    print("check_hil_eil_parity.py --list")
    print("-------------------------------------------------------------------")
    print(f"hil_all.sh HIL_DIR      : {_rel(model.hil_root_hilall)}")
    print(f"eil_all.sh HIL_DIR      : {_rel(model.hil_root_silall)}")
    print(f"eil_all.sh EIL_RA8P1_DIR: {_rel(model.ra8p1_root)}")
    print(f"EIL-capable HIL_MODEs   : {', '.join(model.eil_modes)}")
    print(f"hil_all.sh run set      : {len(model.hil_apps)} app(s)")
    print(f"eil_all.sh run set      : {len(model.eil_apps)} app(s)")
    print(f"  EIL-only (RA8P1)      : {', '.join(model.ra8p1_apps) or '(none)'}")
    print("-------------------------------------------------------------------")
    print(f"{'APP':<34} {'MODE':<16} {'IN EIL':<7}")
    eil_set = set(model.eil_apps)
    for app in model.hil_apps:
        mode = _hil_mode_of(model.hil_root_hilall / app / "hil.conf") or "(no hil.conf)"
        in_eil = "yes" if app in eil_set else "NO"
        print(f"{app:<34} {mode:<16} {in_eil:<7}")


def _report_parse_errors(errors: list[str]) -> int:
    print("check_hil_eil_parity.py: could not parse the HIL/EIL harness scripts:", file=sys.stderr)
    for err in errors:
        print(f"  - {err}", file=sys.stderr)
    print(
        "\nThe harness scripts changed shape and the gate can no longer derive the "
        "discovery rules. Reconcile the parse -- do not bypass the gate.",
        file=sys.stderr,
    )
    return 2


def main(argv: list[str]) -> int:
    """Fail when the HIL app set and the EIL app set have drifted apart.

    A model-build failure short-circuits ahead of every parity check and is
    reported on its own: if the two sets could not be derived, any verdict
    about their agreement would be an artefact of the broken parse rather than
    a fact about the tree.

    ``--list`` prints the derived sets and modes and always exits 0. It is a
    debugging aid, explicitly NOT a gate -- CI must invoke this bare, or the
    step passes without ever comparing anything.

    Returns 0 when parity holds or under ``--list``, 1 on drift (an app
    missing a hil.conf, a set mismatch, or an unsupported mode) or on a
    model-build error.
    """
    parser = argparse.ArgumentParser(
        prog="check_hil_eil_parity.py",
        description="Gate: every HIL app is also exercised in the emulator (EIL==HIL).",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="enumerate the derived hil/eil sets + modes, then exit 0 (no gating).",
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
            f"check_hil_eil_parity.py: EIL==HIL holds -- {len(model.hil_apps)} HIL "
            "app(s), all with a hil.conf, all in an EIL-checkable mode "
            f"({', '.join(model.eil_modes)}), all covered by eil_all.sh "
            f"({len(model.eil_apps)} EIL app(s) incl. {len(model.ra8p1_apps)} RA8P1)."
        )
        return 0

    print(
        f"check_hil_eil_parity.py: {len(offenders)} EIL==HIL parity violation(s):\n",
        file=sys.stderr,
    )
    for offender in offenders:
        print(f"  - {offender}", file=sys.stderr)
    print(
        "\nEIL==HIL is an ongoing discipline: ra8_emulator must reproduce the actual\n"
        "silicon behaviour of every HIL path (bugs included). A HIL app may never\n"
        "be skipped in EIL -- if ra8_emulator cannot yet model a path, extend\n"
        "ra8_emulator + eil_all.sh so EIL stays complete. See the header comment.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
