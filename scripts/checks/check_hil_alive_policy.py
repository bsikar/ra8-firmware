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
  2  usage / unreachable repo root
"""

from __future__ import annotations

import pathlib
import re
import sys
from collections.abc import Iterable

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

    Returns 1 with one remediation-bearing message per offending hil.conf, 0
    when every conf under hw_validated/hil/ declares an asserting mode.
    """
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    violations: list[str] = []

    for conf in sorted(_iter_hil_confs(repo_root)):
        kv = _parse_kv(conf)
        mode = kv.get("HIL_MODE", "")
        fault_expected = kv.get("HIL_FAULT_EXPECTED", "0")
        app = conf.parent.name

        if mode in ALLOWED_MODES:
            continue
        if mode == "alive" and fault_expected == "1":
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

    print(
        f"check_hil_alive_policy.py: 0 findings across "
        f"{sum(1 for _ in _iter_hil_confs(repo_root))} hil.conf file(s)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
