#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce the HIL "honest contract" gate.

Every hil.conf under examples/ek_ra8d2/hw_validated/hil/ must declare
HIL_MODE to one of:
  - uart_scrape       (UART success-banner assertion)
  - usb_cdc           (Pi-side USB host probe)
  - usb_hid           (Pi-side USB HID host-binding probe)
  - usb_msc           (Pi-side USB Mass Storage SCSI-attach probe)
  - jlink_memprobe    (named-symbol counter advance via SWD)
  - hil_eth_tcp       (Pi-as-peer TCP/UDP/HTTP probe)
  - rtt_scrape        (SEGGER RTT banner assertion via JLink mem dump)
  - alive             ONLY if HIL_FAULT_EXPECTED=1 is also set
                      (legitimate fault-recovery test path -- the
                      handler is required to latch CFSR != 0 and the
                      app must emit a positive UART banner that is
                      not in hil_check_alive.sh's negative regex)

Plain HIL_MODE=alive without HIL_FAULT_EXPECTED is the historical
"PC happens to be in MRAM" loose check; it lets silently-broken apps
pass CI and is forbidden under hw_validated/hil/. Apps that genuinely
have no observable signal yet (panic-halt at init, no
instrumentation) must live in hw_pending/ until they are fixed AND
instrumented.

Exit:
  0  every hil.conf in hw_validated/hil/ satisfies the policy
  1  one or more hil.confs violate
  2  usage / unreachable repo root, or a collapsed scan (see HIL_CONF_FLOOR)
"""

from __future__ import annotations

import pathlib
import re
import sys
import tempfile
from collections.abc import Iterable

# A hw_validated HIL suite this size cannot legitimately collapse to a handful
# of apps. If the glob returns less than this, something broke (a bad repo
# root, a renamed hil/ directory) and reporting "0 findings" would be a lie:
# every app would be trivially compliant because none was read. Measured
# 2026-07-28: 114 hil.conf files. Same trip-wire as check_ruff.py.
HIL_CONF_FLOOR = 90

ALLOWED_MODES: frozenset[str] = frozenset(
    {
        "uart_scrape",
        "usb_cdc",
        "usb_hid",
        "usb_msc",
        "jlink_memprobe",
        "hil_eth_tcp",
        "rtt_scrape",
    }
)


def _iter_hil_confs(repo_root: pathlib.Path) -> Iterable[pathlib.Path]:
    """Yield every hil.conf under examples/ek_ra8d2/hw_validated/hil/."""
    hil_dir = repo_root / "examples" / "ek_ra8d2" / "hw_validated" / "hil"
    if not hil_dir.is_dir():
        return ()
    return hil_dir.glob("*/hil.conf")


def _parse_kv(conf: pathlib.Path) -> dict[str, str]:
    """Extract simple KEY=VALUE lines (no shell expansion) from a hil.conf."""
    out: dict[str, str] = {}
    pat = re.compile(r"^\s*([A-Z_][A-Z_0-9]*)\s*=\s*(.*?)\s*$")
    for raw in conf.read_text().splitlines():
        line = raw.split("#", 1)[0].rstrip()
        m = pat.match(line)
        if not m:
            continue
        key, val = m.group(1), m.group(2)
        if val.startswith(('"', "'")) and val.endswith(val[0]) and len(val) >= 2:  # noqa: PLR2004  # min quoted-string length
            val = val[1:-1]
        out[key] = val
    return out


def _policy_violations(repo_root: pathlib.Path, confs: Iterable[pathlib.Path]) -> list[str]:
    """Return one honest-contract violation for each non-asserting config."""
    violations: list[str] = []
    for conf in confs:
        kv = _parse_kv(conf)
        mode = kv.get("HIL_MODE", "")
        fault_expected = kv.get("HIL_FAULT_EXPECTED", "0")
        app = conf.parent.name

        if mode in ALLOWED_MODES or (mode == "alive" and fault_expected == "1"):
            continue
        if mode == "alive":
            violations.append(
                f"{conf.relative_to(repo_root)}: HIL_MODE=alive without "
                f"HIL_FAULT_EXPECTED=1 is forbidden under "
                f"hw_validated/hil/. Either:\n"
                f"    - instrument the app (g_<x>_match symbol + "
                f"HIL_MODE=jlink_memprobe), OR\n"
                f"    - add a UART success banner + HIL_MODE=uart_scrape, OR\n"
                f"    - move the app to examples/ek_ra8d2/hw_pending/{app}/."
            )
        elif mode == "":
            violations.append(f"{conf.relative_to(repo_root)}: missing HIL_MODE")
        else:
            violations.append(
                f"{conf.relative_to(repo_root)}: HIL_MODE={mode!r} is not "
                f"in the allowed set "
                f"({[*sorted(ALLOWED_MODES), 'alive (with HIL_FAULT_EXPECTED=1)']})"
            )
    return violations


def selftest() -> int:
    """Prove asserting modes pass and every loose/unknown form is rejected."""
    with tempfile.TemporaryDirectory(prefix="hil-alive-policy-selftest-") as raw:
        root = pathlib.Path(raw)

        def conf(name: str, text: str) -> pathlib.Path:
            path = root / name / "hil.conf"
            path.parent.mkdir()
            path.write_text(text, encoding="ascii")
            return path

        good = [
            conf("uart", "HIL_MODE=uart_scrape\n"),
            conf("fault", "HIL_MODE=alive\nHIL_FAULT_EXPECTED=1\n"),
        ]
        bad = [
            conf("loose", "HIL_MODE=alive\n"),
            conf("missing", "HIL_FAULT_EXPECTED=1\n"),
            conf("unknown", "HIL_MODE=not_a_probe\n"),
        ]
        good_findings = _policy_violations(root, good)
        bad_findings = _policy_violations(root, bad)
    cases = (
        (not good_findings, "asserting and fault-expected modes stay quiet"),
        (len(bad_findings) == len(bad), "loose, missing, and unknown modes fire"),
    )
    failed = [label for passed, label in cases if not passed]
    for passed, label in cases:
        print(f"  [{'ok' if passed else 'FAIL'}] {label}")
    if failed:
        print(f"check_hil_alive_policy.py --selftest: {len(failed)} failure(s)", file=sys.stderr)
        return 1
    print("check_hil_alive_policy.py --selftest: all cases pass (both directions).")
    return 0


def main() -> int:
    """Fail any hw_validated HIL app whose hil.conf does not assert a real outcome.

    The rule being enforced is that ``HIL_MODE=alive`` proves only that the
    board did not hang -- it cannot distinguish a working app from one that
    booted and did nothing. It is therefore allowed under hw_validated/ only
    when ``HIL_FAULT_EXPECTED=1``, i.e. when not faulting IS the assertion.
    Any other app claiming validation must scrape a banner or probe a symbol,
    or move to hw_pending/.

    A missing HIL_MODE is treated as a violation rather than a default, since
    a silently defaulted mode is how an unasserted app would slip in.

    Enforces HIL_CONF_FLOOR before reading anything and exits 2 below it. An
    empty glob would otherwise report "0 findings" -- indistinguishable from a
    fully compliant suite, and produced by the scan having read nothing.

    Returns 1 with one remediation-bearing message per offending hil.conf, 0
    when every conf under hw_validated/hil/ declares an asserting mode, 2 when
    the enumeration is too small to trust.
    """
    args = sys.argv[1:]
    if args == ["--selftest"]:
        return selftest()
    if args:
        print("usage: check_hil_alive_policy.py [--selftest]", file=sys.stderr)
        return 2
    repo_root = pathlib.Path(__file__).resolve().parents[2]

    confs = sorted(_iter_hil_confs(repo_root))
    if len(confs) < HIL_CONF_FLOOR:
        sys.stderr.write(
            f"check_hil_alive_policy.py: FATAL -- only {len(confs)} hil.conf file(s) in "
            f"scope, floor is {HIL_CONF_FLOOR}.\n"
            "  A collapsed scope reports a compliant suite because it checked nothing.\n"
        )
        return 2

    violations = _policy_violations(repo_root, confs)

    if violations:
        sys.stderr.write(
            f"check_hil_alive_policy.py: {len(violations)} violation(s) in hw_validated/hil/:\n"
        )
        for v in violations:
            sys.stderr.write(f"  - {v}\n")
        sys.stderr.write(
            "\nThe HIL 'honest contract' rule: every app under "
            "hw_validated/hil/ must prove its feature works end-to-end on "
            "real hardware, not just that the chip booted.\n"
        )
        return 1

    print(f"check_hil_alive_policy.py: 0 findings across {len(confs)} hil.conf file(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
