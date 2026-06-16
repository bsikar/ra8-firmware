#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# stage_slot_image.py -- wrap a raw application binary into a dfu_bootloader
# slot image: the body at the slot base, plus the 32-byte image header in the
# slot's LAST page (slot_base + 0x6FFE0). Emits an Intel HEX you can flash with
# J-Link (loadfile) to stage a slot WITHOUT a USB host, e.g. to bench-validate
# the bootloader's jump path. The real operator path is dfu-util: the bootloader
# itself writes the header on DFU commit, so for that path you only need the
# body binary (this tool's --body-hex), not the header.
#
# The header layout and CRC must match libs/ra_dfu (header-last):
#   magic(u32=0x52413844) seq(u32) img_len(u32, mult of 32)
#   img_crc32(u32 = CRC32 over [slot_base, slot_base+img_len)) entry(u32=slot_base)
#   reserved(12 bytes, 0). CRC32 is the standard reflected zlib polynomial.
#
# Usage:
#   stage_slot_image.py --payload app.bin --slot a [--seq 1] --out slotA.hex
#
# The application MUST be linked so its vector table sits at the slot base
# (ORIGIN = slot base); see this app's README "Per-slot payload builds".

import argparse
import struct
import sys
import zlib

# Bank map -- keep in lock-step with libs/ra_dfu/inc/ra_dfu.h.
SLOT_BASE = {"a": 0x02020000, "b": 0x02090000}
SLOT_SIZE = 0x00070000
HDR_OFFSET = 0x0006FFE0
HDR_MAGIC = 0x52413844
PAGE = 0x20


def ihex_records(data: bytes, base: int) -> list:
    """Emit Intel-HEX records (ELA + data) placing data at absolute base."""
    out = []
    hi = (base >> 16) & 0xFFFF
    rec = bytes([2, 0, 0, 4, (hi >> 8) & 0xFF, hi & 0xFF])
    out.append(":" + rec.hex().upper() + f"{((-sum(rec)) & 0xFF):02X}")
    off = base & 0xFFFF
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        addr = off + i
        rec = bytes([len(chunk), (addr >> 8) & 0xFF, addr & 0xFF, 0]) + chunk
        out.append(":" + rec.hex().upper() + f"{((-sum(rec)) & 0xFF):02X}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Stage a dfu_bootloader slot image.")
    ap.add_argument("--payload", required=True, help="raw app binary, linked at the slot base")
    ap.add_argument("--slot", required=True, choices=("a", "b"), help="target slot")
    ap.add_argument("--seq", type=int, default=1, help="monotonic sequence number")
    ap.add_argument("--out", required=True, help="output Intel HEX path")
    args = ap.parse_args()

    base = SLOT_BASE[args.slot]
    body = bytearray(open(args.payload, "rb").read())
    if len(body) % PAGE != 0:
        body += b"\xFF" * (PAGE - (len(body) % PAGE))
    if len(body) > HDR_OFFSET:
        sys.stderr.write(f"error: body {len(body)} bytes exceeds slot capacity {HDR_OFFSET}\n")
        return 2

    crc = zlib.crc32(bytes(body)) & 0xFFFFFFFF
    hdr = struct.pack("<5I", HDR_MAGIC, args.seq, len(body), crc, base) + b"\x00" * 12

    records = ihex_records(bytes(body), base)
    records += ihex_records(hdr, base + HDR_OFFSET)
    records.append(":00000001FF")
    with open(args.out, "w", encoding="ascii") as f:
        f.write("\n".join(records) + "\n")

    print(f"slot {args.slot.upper()} @0x{base:08X}: body={len(body)}B crc=0x{crc:08X} "
          f"hdr@0x{base + HDR_OFFSET:08X} seq={args.seq} -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
