#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# gen_jlink_w4.py -- Emit a J-Link commander script that programs a flat
# binary into MRAM using w4 (direct DAP word writes) and starts the firmware
# by injecting PC/MSP/xPSR via CoreSight DCRSR/DCRDR without SYSRESETREQ.
#
# Why w4 instead of loadfile/loadbin:
#   loadfile/loadbin both trigger SYSRESETREQ before programming.  After that
#   reset the RA8D2 runs on its ~4 MHz internal oscillator; J-Link uploads
#   its RAMCode to SRAM and calls Prepare/Program/Finalize.  After ~13
#   consecutive operations the MRAM controller accumulates error state that
#   only a physical PORST (power-on reset) can clear, causing every subsequent
#   flash attempt to fail.
#
# Why not "r" to restart after w4 writes:
#   J-Link Commander's "r" command on RA8D2 with TrustZone invokes a
#   device-specific reset sequence that again runs RAMCode Prepare(), causing
#   the same accumulation.  Instead, the generated script injects the firmware
#   entry point directly into the CPU core registers via DCRSR/DCRDR while
#   the CPU is halted, then issues "g" (go).  No SYSRESETREQ is issued at any
#   point, so the MRAM controller state never accumulates.
#
# DCRSR/DCRDR register injection (ARMv8-M reference manual, C1.6):
#   DCRDR (0xE000EDF8): data register -- write the value here first
#   DCRSR (0xE000EDF4): selector register -- set bit 16 (write) + reg index
#   Register indices used:
#     15 (0x0F) = PC
#     17 (0x11) = MSP
#     16 (0x10) = xPSR  (set T bit, Thread mode: 0x01000000)
#     20 (0x14) = CFBP  (CONTROL | FAULTMASK | BASEPRI | PRIMASK, all zero)
#
# Usage:
#   python3 scripts/gen/gen_jlink_w4.py <binary> <base_addr_hex> \
#       [--device <device_name>]
#   python3 scripts/gen/gen_jlink_w4.py firmware.bin 0x02000000 > flash.jlink
#
# The binary must be a flat raw image whose first byte maps to base_addr.
# Use arm-none-eabi-objcopy -O binary to produce one from an ELF.
#
# Note: pass -SelectEmuBySN <SN> to JLinkExe on the command line, NOT via
# the generated script.  Placing SelectEmuBySN inside the commander script
# causes J-Link to fail subsequent halt commands on RA8D2 with SSD enabled.

import struct
import sys
from pathlib import Path

# Minimum positional argument count: <binary> <base_addr_hex>
MIN_ARGS = 3


def main() -> None:
    if len(sys.argv) < MIN_ARGS:
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

    with Path(bin_file).open("rb") as f:
        data: bytes = f.read()

    while len(data) % 4:
        data += b"\xff"

    initial_sp: int = struct.unpack_from("<I", data, 0)[0]
    reset_handler: int = struct.unpack_from("<I", data, 4)[0] & 0xFFFFFFFE

    print(f"device {device}")
    print("si SWD")
    print("speed 1000")
    print("connect")
    print("halt")
    for idx in range(0, len(data), 4):
        word: int = struct.unpack_from("<I", data, idx)[0]
        print(f"w4 0x{base + idx:08X} 0x{word:08X}")

    print(f"w4 0xE000EDF8 0x{initial_sp:08X}")
    print("w4 0xE000EDF4 0x00010011")
    print("w4 0xE000EDF8 0x01000000")
    print("w4 0xE000EDF4 0x00010010")
    print("w4 0xE000EDF8 0x00000000")
    print("w4 0xE000EDF4 0x00010014")
    print(f"w4 0xE000EDF8 0x{reset_handler:08X}")
    print("w4 0xE000EDF4 0x0001000F")
    print("g")
    print("q")


if __name__ == "__main__":
    main()
