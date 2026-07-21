#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Verify the NSC Secure-Gateway veneer slot offsets in a linked ELF.

The tz_nsc_cgc_usb Non-Secure image reaches each NSC CGC veneer by ADDRESS
(``g_ra8_ls_sgstubs_start + <slot offset>``) rather than by symbol, because
GNU ld rewrites every reference to a ``cmse_nonsecure_entry`` symbol inside
one secure image onto the secure body ``__acle_se_*`` -- so the bare veneer
symbol cannot be used, and a linker ``ASSERT`` sees the wrong value too.

This post-build check reads the FINAL symbol table (where ``nm`` reports the
real SG-stub addresses) and fails the build if any veneer has drifted from the
offset hard-coded in ``ns_main.c`` (the ``k_sg_off_*`` enum). If it fires,
update both the enum and this table together.

Usage:
    check_sg_offsets.py <elf> [--nm <nm-binary>]

Exit codes:
    0  -- offsets match, or the veneers are absent (non-TrustZone build)
    1  -- a veneer slot drifted
    2  -- usage / tool error
"""

import argparse
import contextlib
import shutil
import subprocess
import sys

# Expected byte offset of each SG veneer from g_ra8_ls_sgstubs_start. ld emits
# the 8-byte stubs in ascending symbol-name order, so this is deterministic.
EXPECTED_OFFSETS = {
    "ra8_nsc_cgc_get_clock_hz": 0,
    "ra8_nsc_cgc_usbfs_clock_enable": 8,
    "ra8_nsc_cgc_pll2_enable": 16,
}
BASE_SYMBOL = "g_ra8_ls_sgstubs_start"
THUMB_MASK = 0xFFFFFFFE

# nm output has 3 fields: address, type, name.
NM_FIELD_COUNT = 3


def read_symbols(elf: str, nm: str) -> dict[str, int]:
    """Return {symbol: address} for every defined symbol in ``elf``."""
    out = subprocess.run(  # noqa: S603  # trusted: fixed arm-none-eabi-nm argv
        [nm, elf], capture_output=True, text=True, check=True
    ).stdout
    syms: dict[str, int] = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == NM_FIELD_COUNT and parts[1] in ("T", "t", "R", "r", "D", "d", "B", "b"):
            with contextlib.suppress(ValueError):
                syms[parts[2]] = int(parts[0], 16)
    return syms


def main() -> int:
    """Verify the secure-gateway veneers sit at their pinned offsets in an ELF.

    The offsets are ABI: non-secure code reaches the secure world by branching
    into the SG region at a fixed distance from its base, so a veneer moving
    silently redirects a call to a different entry point. Only the offset from
    BASE_SYMBOL is compared, never absolute addresses, since the region as a
    whole is free to relocate between builds.

    Symbol values are masked with THUMB_MASK before subtracting -- every Thumb
    function symbol carries bit 0 set, and comparing raw values would make
    every offset off by one.

    An ELF with no veneers is skipped and exits 0, which is correct for a
    non-TrustZone build rather than a silent pass: absent veneers cannot drift.

    Returns 0 when every offset matches or no veneers are present, 1 on drift
    or a missing symbol, 2 when ``nm`` could not be run at all.
    """
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    args = ap.parse_args()

    nm = shutil.which(args.nm) or args.nm
    try:
        syms = read_symbols(args.elf, nm)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"check_sg_offsets: cannot run nm on {args.elf}: {exc}", file=sys.stderr)
        return 2

    if BASE_SYMBOL not in syms or not any(s in syms for s in EXPECTED_OFFSETS):
        # No veneers (TrustZone disabled / non-TZ build) -- nothing to verify.
        print("check_sg_offsets: no SG veneers present -- skipped.")
        return 0

    base = syms[BASE_SYMBOL]
    drift = []
    for sym, want in EXPECTED_OFFSETS.items():
        if sym not in syms:
            drift.append(f"  {sym}: MISSING from the link")
            continue
        got = (syms[sym] & THUMB_MASK) - base
        if got != want:
            drift.append(f"  {sym}: at sgstubs+{got} (expected sgstubs+{want})")

    if drift:
        print("check_sg_offsets: FATAL -- NSC SG-veneer slot drift detected.", file=sys.stderr)
        print("\n".join(drift), file=sys.stderr)
        print(
            "Update k_sg_off_* in ns_main.c and EXPECTED_OFFSETS here in step.",
            file=sys.stderr,
        )
        return 1

    print("check_sg_offsets: NSC SG-veneer slot offsets OK (0/8/16).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
