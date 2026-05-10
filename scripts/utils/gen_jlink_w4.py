#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# gen_jlink_w4.py -- Emit a J-Link commander script that programs a flat
# binary into MRAM using w4 (direct DAP word writes).
#
# Why w4 instead of loadfile/loadbin:
#   On RA8D2, loadfile/loadbin trigger an implicit SYSRESETREQ before
#   programming.  After that reset the CPU runs on the ~4 MHz internal
#   oscillator.  If SWD speed exceeds ~1 MHz at that point, J-Link's RAMCode
#   at 0x320D0000 loses communication and times out.  w4 bypasses the flash
#   algorithm entirely by writing directly via DAP while the CPU is halted,
#   so SWD speed is no longer a constraint during programming.
#
# Usage:
#   python3 scripts/utils/gen_jlink_w4.py <binary> <base_addr_hex> \
#       [--device <device_name>]
#   python3 scripts/utils/gen_jlink_w4.py firmware.bin 0x02000000 > flash.jlink
#
# The binary must be a flat raw image whose first byte maps to base_addr.
# Use arm-none-eabi-objcopy -O binary to produce one from an ELF.
#
# Note: pass -SelectEmuBySN <SN> to JLinkExe on the command line, NOT via
# the generated script.  Placing SelectEmuBySN inside the commander script
# causes J-Link to fail subsequent halt commands on RA8D2 with SSD enabled.

import struct
import sys


def main() -> None:
    if len(sys.argv) < 3:
        print(
            f"Usage: {sys.argv[0]} <binary> <base_addr_hex> [--device DEV]",
            file=sys.stderr,
        )
        sys.exit(1)

    bin_file: str = sys.argv[1]
    base: int = int(sys.argv[2], 16)
    device: str = "R7KA8D2KF_CPU0"

    i: int = 3
    while i < len(sys.argv):
        if sys.argv[i] == "--device" and i + 1 < len(sys.argv):
            device = sys.argv[i + 1]
            i += 2
        else:
            print(f"Unknown arg: {sys.argv[i]}", file=sys.stderr)
            sys.exit(1)

    with open(bin_file, "rb") as f:
        data: bytes = f.read()

    while len(data) % 4:
        data += b"\xff"

    print(f"device {device}")
    print("si SWD")
    print("speed 1000")
    print("connect")
    print("halt")
    for idx in range(0, len(data), 4):
        word: int = struct.unpack_from("<I", data, idx)[0]
        print(f"w4 0x{base + idx:08X} 0x{word:08X}")
    print("r")
    print("g")
    print("q")


if __name__ == "__main__":
    main()
