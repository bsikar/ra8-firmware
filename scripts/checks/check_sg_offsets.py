#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Verify the NSC Secure-Gateway veneer slot offsets in a linked ELF.

The tz_nsc_cgc_usb Non-Secure image reaches each NSC CGC veneer by NAME, not by
address: ``ns_main.c`` declares the bare ``ra8_nsc_cgc_*`` prototypes and calls
them, and the CMSE import library the Secure link emits (``--cmse-implib
--out-implib``, on this image's link line) binds those names to the
Secure-Gateway stub addresses. That import library is derived FROM the Secure
ELF, so the byte offset of each veneer inside the ``.gnu.sgstubs`` region IS the
ABI contract between the two worlds: reorder the stubs and every bound
NS->Secure call lands on a different entry point, with no diagnostic.

This post-build check reads the FINAL Secure symbol table (where ``nm`` reports
the real SG-stub addresses) and fails the build if any veneer has drifted from
its pinned offset in ``EXPECTED_OFFSETS`` below. A drift means the Secure ELF no
longer matches the offsets this table records -- and therefore no longer matches
the import library the NS image was bound against; re-derive the offsets from
the link and update the table. (There is no ``k_sg_off_*`` enum to keep in step
any more: the NS side gave up hard-coded offsets for the import library.)

The veneer set is REQUIRED, not optional, once the ELF has an NSC region. Both
callers -- the ``tz_nsc_cgc_usb.elf`` POST_BUILD command and the ``sg-offsets``
gate -- pick that ELF precisely BECAUSE it binds all three ``ra8_nsc_cgc_*``
veneers (its CMakeLists passes ``NSC_SRCS ra8_nsc_cgc.c``, whose three
``RA8_NSC_VENEER`` definitions are the whole point of the app). So an absent
veneer there is a broken secure gateway, not a build configuration, and the
old ``any(...)`` guard turned exactly that defect into ``skipped.`` + exit 0.
Only an ELF with no ``g_ra8_ls_sgstubs_start`` at all -- a link whose script
never placed ``.gnu.sgstubs`` -- is still skipped.

Usage:
    check_sg_offsets.py <elf> [--nm <nm-binary>]

Exit codes:
    0  -- offsets match, or the ELF has no NSC region at all
    1  -- a veneer slot drifted, or a required veneer is missing from the link
    2  -- usage / tool error, or a symbol table too small to trust (SYMBOL_FLOOR)
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

# A linked firmware image cannot legitimately define a handful of symbols. If
# the parse returns less than this, something broke (the wrong file, a stripped
# ELF, an nm that printed a format this parser does not recognise) and every
# later lookup would miss -- which reads as "no SG veneers present" and exits 0
# having verified nothing. Measured 2026-07-28: 187 defined symbols in
# tz_nsc_cgc_usb.elf. Same trip-wire as check_ruff.py.
SYMBOL_FLOOR = 140


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


def offset_drift(syms: dict[str, int]) -> list[str]:
    """Return every missing or displaced required veneer from a symbol table."""
    base = syms[BASE_SYMBOL]
    drift: list[str] = []
    for sym, want in EXPECTED_OFFSETS.items():
        if sym not in syms:
            drift.append(f"  {sym}: MISSING from the link")
            continue
        got = (syms[sym] & THUMB_MASK) - base
        if got != want:
            drift.append(f"  {sym}: at sgstubs+{got} (expected sgstubs+{want})")
    return drift


def selftest() -> int:
    """Prove exact Thumb offsets pass while one drift and one absence fire."""
    base = 0x1000
    good = {
        BASE_SYMBOL: base,
        **{name: base + offset + 1 for name, offset in EXPECTED_OFFSETS.items()},
    }
    bad = dict(good)
    missing = next(iter(EXPECTED_OFFSETS))
    bad.pop(missing)
    shifted = next(name for name in EXPECTED_OFFSETS if name != missing)
    bad[shifted] += 8
    good_findings = offset_drift(good)
    bad_findings = offset_drift(bad)
    expected_bad_findings = 2
    cases = (
        (not good_findings, "exact Thumb-normalized veneer offsets stay quiet"),
        (
            len(bad_findings) == expected_bad_findings
            and any("MISSING" in item for item in bad_findings)
            and any("expected" in item for item in bad_findings),
            "a missing veneer and a displaced veneer both fire",
        ),
    )
    failed = [label for passed, label in cases if not passed]
    for passed, label in cases:
        print(f"  [{'ok' if passed else 'FAIL'}] {label}")
    if failed:
        print(f"check_sg_offsets.py --selftest: {len(failed)} failure(s)", file=sys.stderr)
        return 1
    print("check_sg_offsets.py --selftest: all cases pass (both directions).")
    return 0


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

    Only an ELF with no ``g_ra8_ls_sgstubs_start`` at all is skipped -- a link
    whose script never placed ``.gnu.sgstubs``, where there is no NSC region to
    have drifted. Once that base symbol exists, EVERY veneer in
    EXPECTED_OFFSETS is required: the callers hand this the one ELF that binds
    all three by construction, so a missing one is a broken secure gateway. The
    previous ``any(...)`` guard reported that defect as ``skipped.`` and
    exited 0.

    SYMBOL_FLOOR guards the layer beneath both: a parse that yields almost no
    symbols makes every lookup miss, which the skip branch would then read as
    "no veneers present".

    Returns 0 when every offset matches or the ELF has no NSC region, 1 on
    drift or a missing veneer, 2 when ``nm`` could not be run at all or its
    output fell below SYMBOL_FLOOR.
    """
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", nargs="?")
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        if args.elf is not None or args.nm != "arm-none-eabi-nm":
            ap.error("--selftest does not accept an ELF or --nm")
        return selftest()
    if args.elf is None:
        ap.error("the following arguments are required: elf")

    nm = shutil.which(args.nm) or args.nm
    try:
        syms = read_symbols(args.elf, nm)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"check_sg_offsets: cannot run nm on {args.elf}: {exc}", file=sys.stderr)
        return 2

    if len(syms) < SYMBOL_FLOOR:
        print(
            f"check_sg_offsets: FATAL -- only {len(syms)} defined symbol(s) read from "
            f"{args.elf}, floor is {SYMBOL_FLOOR}. A collapsed symbol table reports "
            "'no SG veneers present' because every lookup missed.",
            file=sys.stderr,
        )
        return 2

    if BASE_SYMBOL not in syms:
        # No .gnu.sgstubs placement at all: there is no NSC region to drift.
        # A PRESENT base with absent veneers is NOT this case -- it falls
        # through and every missing veneer is reported below.
        print("check_sg_offsets: no NSC region in this ELF -- skipped.")
        return 0

    drift = offset_drift(syms)

    if drift:
        print("check_sg_offsets: FATAL -- NSC SG-veneer slot drift detected.", file=sys.stderr)
        print("\n".join(drift), file=sys.stderr)
        print(
            "Re-derive the offsets from the Secure link and update EXPECTED_OFFSETS here.",
            file=sys.stderr,
        )
        return 1

    print("check_sg_offsets: NSC SG-veneer slot offsets OK (0/8/16).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
