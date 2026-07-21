#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Wrap a raw application binary into a dfu_bootloader slot image.

Produces the body at the slot base plus the 32-byte image header in the slot's
LAST page (slot_base + 0x6FFE0), emitted as an Intel HEX that J-Link `loadfile`
can flash. That stages a slot WITHOUT a USB host -- the bench path for
validating the bootloader's copy-to-run behaviour.

This is NOT the operator path. Operators use dfu-util, and the bootloader
writes the header itself on DFU commit, so that path needs only the body
binary and never this tool.

Copy-to-run is why one image works in either slot: the bootloader copies the
slot body to the fixed SRAM run base (RUN_BASE) and launches it there, so the
IDENTICAL app.bin can be staged to slot a or slot b. The consequence is a hard
requirement on the input -- the application MUST be linked at the SRAM run base
(ORIGIN = RUN_BASE), NOT at a slot base. An image linked at a slot base stages
and flashes cleanly and then faults on launch. See this app's README, "One
image, either slot (copy-to-run)".

The header layout and CRC must stay in lock-step with libs/ra8_dfu
(header-last)::

    magic(u32 = 0x52413844) seq(u32) img_len(u32, multiple of 32)
    img_crc32(u32, CRC32 over [slot_base, slot_base + img_len))
    entry(u32 = RUN_BASE) reserved(12 bytes, zero)

CRC32 is the standard reflected zlib polynomial.

Usage:
    stage_slot_image.py --payload app.bin --slot a [--seq 1] --out slotA.hex
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path

# Bank map -- keep in lock-step with libs/ra8_dfu/inc/ra8_dfu.h.
SLOT_BASE = {"a": 0x02020000, "b": 0x02090000}
SLOT_SIZE = 0x00070000
HDR_OFFSET = 0x0006FFE0
HDR_MAGIC = 0x52413844
RUN_BASE = 0x22020000  # k_ra8_dfu_run_base: SRAM copy-to-run / payload link base
PAGE = 0x20


def ihex_records(data: bytes, base: int) -> list:
    """Emit Intel-HEX records (ELA + data) placing data at absolute base."""
    out = []
    hi = (base >> 16) & 0xFFFF
    rec = bytes([2, 0, 0, 4, (hi >> 8) & 0xFF, hi & 0xFF])
    out.append(":" + rec.hex().upper() + f"{((-sum(rec)) & 0xFF):02X}")
    off = base & 0xFFFF
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        addr = off + i
        rec = bytes([len(chunk), (addr >> 8) & 0xFF, addr & 0xFF, 0]) + chunk
        out.append(":" + rec.hex().upper() + f"{((-sum(rec)) & 0xFF):02X}")
    return out


def _build_parser() -> argparse.ArgumentParser:
    """Build the command-line parser for the slot stager."""
    ap = argparse.ArgumentParser(description="Stage a dfu_bootloader slot image.")
    ap.add_argument(
        "--payload", required=True, help="raw app binary, linked at the SRAM run base 0x22020000"
    )
    ap.add_argument("--slot", required=True, choices=("a", "b"), help="target slot")
    ap.add_argument("--seq", type=int, default=1, help="monotonic sequence number")
    ap.add_argument(
        "--signed",
        action="store_true",
        help="payload is a rot_sign.py-signed [body][ra8_rot_trailer_t] "
        "image; the header img_len + CRC cover the body only and "
        "the trailer is staged contiguously for the RoT launch gate",
    )
    ap.add_argument(
        "--trailer-size",
        type=int,
        default=116,
        help="ra8_rot_trailer_t size in bytes (only with --signed)",
    )
    ap.add_argument("--out", required=True, help="output Intel HEX path")
    return ap


def _build_slot_body(args: argparse.Namespace) -> tuple[int, int, bytes] | None:
    """Return ``(img_len, crc, slot_bytes)``, or None after reporting a fault.

    The two layouts differ in what the header covers. A plain payload is
    FF-padded to a page multiple and the CRC spans all of it. A signed slot is
    ``[body][ra8_rot_trailer_t]``: the header's img_len and CRC cover the BODY
    only, and the trailer is staged contiguously right after it so the RoT
    launch gate finds it at ``slot_base + img_len`` (see ra8_rot_trailer_after).
    The bootloader copies only img_len body bytes to the run base.
    """
    raw = bytearray(Path(args.payload).read_bytes())
    if not args.signed:
        if len(raw) % PAGE != 0:
            raw += b"\xff" * (PAGE - (len(raw) % PAGE))
        return len(raw), zlib.crc32(bytes(raw)) & 0xFFFFFFFF, bytes(raw)

    if len(raw) <= args.trailer_size:
        sys.stderr.write(f"error: signed image {len(raw)}B <= trailer {args.trailer_size}B\n")
        return None
    img_len = len(raw) - args.trailer_size
    if img_len % PAGE != 0:
        sys.stderr.write(
            f"error: signed body {img_len}B is not a multiple of {PAGE} "
            f"-- sign a page-aligned body\n"
        )
        return None
    return img_len, zlib.crc32(bytes(raw[:img_len])) & 0xFFFFFFFF, bytes(raw)


def main() -> int:
    """Build the slot image from the command line and write the Intel HEX.

    Pads the payload to a PAGE multiple, computes the CRC over the padded body,
    and places the header in the slot's last page. Body and header land at
    absolute addresses derived from `--slot`, so the output HEX is
    slot-specific even though the payload is not.

    `--seq` selects which slot the bootloader prefers at boot: the higher
    sequence number wins. Staging an image with a sequence at or below the other
    slot's produces a valid image the bootloader will simply not choose --
    which looks exactly like a flash that did not take.

    Returns:
        0 on success, non-zero on a usage or validation failure.
    """
    args = _build_parser().parse_args()

    base = SLOT_BASE[args.slot]
    built = _build_slot_body(args)
    if built is None:
        return 2
    img_len, crc, slot = built

    if len(slot) > HDR_OFFSET:
        sys.stderr.write(f"error: slot content {len(slot)} bytes exceeds capacity {HDR_OFFSET}\n")
        return 2

    # entry == RUN_BASE (copy-to-run): the bootloader copies img_len body bytes
    # there and launches it; the same image is valid in either slot.
    hdr = struct.pack("<5I", HDR_MAGIC, args.seq, img_len, crc, RUN_BASE) + b"\x00" * 12

    records = ihex_records(slot, base)
    records += ihex_records(hdr, base + HDR_OFFSET)
    records.append(":00000001FF")
    with Path(args.out).open("w", encoding="ascii") as f:
        f.write("\n".join(records) + "\n")

    kind = "signed" if args.signed else "plain"
    print(
        f"slot {args.slot.upper()} @0x{base:08X} ({kind}): img_len={img_len}B "
        f"slot={len(slot)}B crc=0x{crc:08X} hdr@0x{base + HDR_OFFSET:08X} "
        f"seq={args.seq} -> {args.out}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
