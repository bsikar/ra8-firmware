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
the real SG-stub addresses) and applies two rules.

STRUCTURAL, on every Secure ELF that placed an NSC region. Each veneer in the
link is found by its ``__acle_se_<name>`` companion symbol -- gcc emits one per
``cmse_nonsecure_entry`` function, so the pair (``name``, ``__acle_se_name``)
is the definitive marker of a veneer, independent of which ``ra8_nsc`` sources
the app happened to compile. Every one of those entry symbols must land inside
``[g_ra8_ls_sgstubs_start, g_ra8_ls_sgstubs_end)`` at an exact multiple of
SG_STUB_BYTES, and the veneers must fill the region one-per-slot: no two
veneers on one slot, no slot left empty, and the region sized exactly for the
veneer count. That is the invariant the ``linker_script.ld`` comment rests the
S/NS boundary on, and it holds for ANY app -- the 3-veneer ``tz_nsc_cgc_usb``
link and the 40-veneer ``ra8d2-ereader`` link alike.

PINNED, on the image EXPECTED_OFFSETS was derived from. That table records the
byte offsets of the ``tz_nsc_cgc_usb`` veneer set, which is the exact set that
app links (its CMakeLists passes ``NSC_SRCS ra8_nsc_cgc.c``). It is applied
when the ELF's veneer set equals the table's key set, so a drift in that app's
ABI still fails the build; an app with a different veneer set gets the
structural rule only, because its offsets are legitimately its own. Measured
2026-09-17 on ``ra8d2-ereader.elf``: 40 stubs in 320 bytes, with
``ra8_nsc_cgc_get_clock_hz`` at +96, not +0.

ld does NOT emit the stubs in ascending symbol-name order -- the 40-stub
e-reader layout starts ``acmphs_init``, ``pdm_init``, ``adc_init`` -- so only
the stride and the one-per-slot packing are dependable, never the order. A
reorder inside the region is still caught for the pinned image, and for every
image it is caught by the CMSE import library being re-derived from the same
link.

Only an ELF with no ``g_ra8_ls_sgstubs_start`` at all -- a link whose script
never placed ``.gnu.sgstubs`` -- is skipped.

Usage:
    check_sg_offsets.py <elf> [--nm <nm-binary>]

Exit codes:
    0  -- the layout holds, or the ELF has no NSC region at all
    1  -- a veneer slot drifted, a required veneer is missing from the link, or
          the SG region is not exactly one 8-byte slot per veneer
    2  -- usage / tool error, or a symbol table too small to trust (SYMBOL_FLOOR)
"""

import argparse
import contextlib
import shutil
import subprocess
import sys

# Expected byte offset of each SG veneer from g_ra8_ls_sgstubs_start, for the
# tz_nsc_cgc_usb link (NSC_SRCS ra8_nsc_cgc.c, so exactly these three). Derived
# from that link, NOT from a naming rule: ld's stub order is not ascending
# symbol-name order, as ra8d2-ereader.elf shows (40 stubs opening acmphs_init,
# pdm_init, adc_init). Applied only to an ELF whose veneer set equals these
# keys; see pinned_image().
EXPECTED_OFFSETS = {
    "ra8_nsc_cgc_get_clock_hz": 0,
    "ra8_nsc_cgc_usbfs_clock_enable": 8,
    "ra8_nsc_cgc_pll2_enable": 16,
}
BASE_SYMBOL = "g_ra8_ls_sgstubs_start"
END_SYMBOL = "g_ra8_ls_sgstubs_end"
THUMB_MASK = 0xFFFFFFFE

# gcc names the Secure body of a cmse_nonsecure_entry function
# __acle_se_<name> and gives <name> to the secure-gateway stub, so the presence
# of the companion symbol is what identifies a veneer in any linked image.
ACLE_PREFIX = "__acle_se_"

# One ARMv8-M secure gateway is SG + B.W: 4 + 4 bytes. ld packs the stubs back
# to back, so the region size is exactly SG_STUB_BYTES * veneer count.
SG_STUB_BYTES = 8

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


def veneer_symbols(syms: dict[str, int]) -> dict[str, int]:
    """Return {veneer: gateway address} for every veneer the link contains.

    A veneer is recognised by its ``__acle_se_<name>`` companion rather than by
    an ``ra8_nsc_`` name prefix, so this holds for any future veneer family and
    cannot be fooled by an ordinary function that merely shares the prefix.
    """
    return {
        name[len(ACLE_PREFIX) :]: syms[name[len(ACLE_PREFIX) :]]
        for name in syms
        if name.startswith(ACLE_PREFIX) and name[len(ACLE_PREFIX) :] in syms
    }


def slot_findings(syms: dict[str, int]) -> list[str]:
    """Return every way the SG region departs from one veneer per 8-byte slot.

    Checked against the region the linker script actually produced, so an app
    that links three veneers and one that links forty are both covered without
    a per-app table. Reported: a gateway outside the region, a gateway at a
    misaligned address, two veneers sharing a slot, an empty slot, and a region
    whose size is not the veneer count times the stride.
    """
    base = syms[BASE_SYMBOL]
    findings: list[str] = []
    veneers = veneer_symbols(syms)
    if not veneers:
        return [
            "  no veneers in the link, but the script placed an NSC region "
            f"({BASE_SYMBOL} is defined): the secure gateway is empty"
        ]
    if END_SYMBOL not in syms:
        return [f"  {END_SYMBOL} missing: cannot bound the SG region"]
    size = syms[END_SYMBOL] - base
    slots: dict[int, str] = {}
    for name, addr in sorted(veneers.items()):
        off = (addr & THUMB_MASK) - base
        if off < 0 or off >= size:
            findings.append(f"  {name}: gateway at sgstubs+{off}, outside the {size} B region")
            continue
        if off % SG_STUB_BYTES != 0:
            findings.append(f"  {name}: gateway at sgstubs+{off}, not a {SG_STUB_BYTES} B multiple")
            continue
        if off in slots:
            findings.append(f"  {name}: shares slot sgstubs+{off} with {slots[off]}")
            continue
        slots[off] = name
    want = len(veneers) * SG_STUB_BYTES
    if size != want:
        findings.append(
            f"  region is {size} B for {len(veneers)} veneer(s), expected {want} B: "
            "either a stub is missing a symbol or something else shares the section"
        )
    empty = [off for off in range(0, size, SG_STUB_BYTES) if off not in slots]
    if empty and not findings:
        findings.append(
            f"  {len(empty)} slot(s) with no veneer symbol, first at sgstubs+{empty[0]}"
        )
    return findings


def pinned_image(syms: dict[str, int]) -> bool:
    """Say whether this ELF is the image EXPECTED_OFFSETS was derived from.

    The table pins one app's whole veneer set, so it only means anything on a
    link with exactly that set. Any other app's offsets are its own and get the
    structural rule alone.
    """
    return set(veneer_symbols(syms)) == set(EXPECTED_OFFSETS)


def _fixture(names: tuple[str, ...], base: int = 0x1000) -> dict[str, int]:
    """Build a symbol table holding ``names`` as veneers, packed in order.

    Each veneer gets the Thumb bit set (as ld emits it) plus the
    ``__acle_se_`` companion, so the fixture exercises the same recognition
    path a real ELF does.
    """
    syms = {BASE_SYMBOL: base, END_SYMBOL: base + len(names) * SG_STUB_BYTES}
    for slot, name in enumerate(names):
        syms[name] = base + (slot * SG_STUB_BYTES) + 1
        syms[ACLE_PREFIX + name] = base + 0x400 + slot
    return syms


def selftest() -> int:
    """Prove a clean layout stays quiet while each defect fires, both rules."""
    base = 0x1000
    pinned = {
        BASE_SYMBOL: base,
        END_SYMBOL: base + len(EXPECTED_OFFSETS) * SG_STUB_BYTES,
        **{name: base + offset + 1 for name, offset in EXPECTED_OFFSETS.items()},
        **{ACLE_PREFIX + name: base + 0x400 + i for i, name in enumerate(EXPECTED_OFFSETS)},
    }
    bad = dict(pinned)
    missing = next(iter(EXPECTED_OFFSETS))
    bad.pop(missing)
    shifted = next(name for name in EXPECTED_OFFSETS if name != missing)
    bad[shifted] += SG_STUB_BYTES
    bad_findings = offset_drift(bad)
    expected_bad_findings = 2

    # A different app's veneer set: more veneers, its own offsets, and the
    # pinned table must not be applied to it. Mirrors ra8d2-ereader.elf, where
    # ra8_nsc_cgc_get_clock_hz sits at +96 rather than +0.
    wide = ("ra8_nsc_acmphs_init", "ra8_nsc_pdm_init", *EXPECTED_OFFSETS)
    wide_syms = _fixture(wide)

    misaligned = _fixture(wide)
    misaligned["ra8_nsc_pdm_init"] += 2

    outside = _fixture(wide)
    outside["ra8_nsc_pdm_init"] = outside[BASE_SYMBOL] + 0x900 + 1

    collided = _fixture(wide)
    collided["ra8_nsc_pdm_init"] = collided["ra8_nsc_acmphs_init"]

    oversized = _fixture(wide)
    oversized[END_SYMBOL] += SG_STUB_BYTES

    empty = {BASE_SYMBOL: base, END_SYMBOL: base + SG_STUB_BYTES}

    cases = (
        (not offset_drift(pinned), "exact Thumb-normalized veneer offsets stay quiet"),
        (
            len(bad_findings) == expected_bad_findings
            and any("MISSING" in item for item in bad_findings)
            and any("expected" in item for item in bad_findings),
            "a missing veneer and a displaced veneer both fire",
        ),
        (pinned_image(pinned), "the pinned veneer set is recognised as its own image"),
        (not pinned_image(wide_syms), "a wider veneer set is not held to the pinned table"),
        (not slot_findings(wide_syms), "a packed region of any veneer count stays quiet"),
        (
            any("multiple" in item for item in slot_findings(misaligned)),
            "a gateway off the 8-byte stride fires",
        ),
        (
            any("outside" in item for item in slot_findings(outside)),
            "a gateway outside the region fires",
        ),
        (
            any("shares slot" in item for item in slot_findings(collided)),
            "two veneers on one slot fire",
        ),
        (
            any("expected" in item for item in slot_findings(oversized)),
            "a region wider than its veneer count fires",
        ),
        (
            any("gateway is empty" in item for item in slot_findings(empty)),
            "an NSC region with no veneer at all fires",
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
    have drifted. Once that base symbol exists the structural rule always runs,
    on any app: the region must hold exactly one veneer per SG_STUB_BYTES slot.
    The pinned EXPECTED_OFFSETS table runs on top of that for the image it was
    derived from, where EVERY veneer in it is required, so a missing one there
    is a broken secure gateway rather than a build configuration.

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

    findings = slot_findings(syms)
    pinned = pinned_image(syms)
    if pinned:
        findings += offset_drift(syms)

    if findings:
        print("check_sg_offsets: FATAL -- NSC SG-veneer slot drift detected.", file=sys.stderr)
        print("\n".join(findings), file=sys.stderr)
        print(
            "Re-derive the offsets from the Secure link and update EXPECTED_OFFSETS here "
            "(pinned image), or fix the link so the SG region holds one veneer per "
            f"{SG_STUB_BYTES} B slot.",
            file=sys.stderr,
        )
        return 1

    count = len(veneer_symbols(syms))
    pin_note = ", pinned offsets 0/8/16 verified" if pinned else ""
    print(f"check_sg_offsets: NSC SG-veneer layout OK ({count} veneer(s){pin_note}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
