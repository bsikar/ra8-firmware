#!/usr/bin/env python3
"""Downloader shim for the ESP32-C6 spike (roadmap).

The end state is our OWN implementation of the ESP32 ROM serial download
protocol (the SLIP-framed loader the ROM speaks at reset), so `make flash`
needs no esptool. Until that exists, this script documents the path and shims
`esptool` as the transport when it is available -- exactly as the RA8 tree used
vendor tools as a temporary transport while the our-own flow was built.

This is intentionally a stub: it does NOT yet drive a chip. It resolves the
image, reports what the real downloader will do, and -- if `esptool` is on PATH
and the caller passes --run -- prints the exact command it would delegate to.

Pure python3 standard library. Usage:

    python3 tools/esp_flash.py [image] [--port PORT] [--run]

Default image is build/esp32c6-blink.bin (resolved relative to this script).

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

import shutil
import sys
from pathlib import Path

DEFAULT_BAUD = 460800  # ROM download console baud for flashing # [CONFIRM]
DEFAULT_CHIP = "esp32c6"
HP_SRAM_BASE = 0x40800000  # RAM-app load address (see boot/esp32c6.ld)


def default_image():
    """Return the default .app.bin path relative to this script's esp32/ dir.

    NOTE: ``esptool load_ram`` parses the Espressif image container (the
    esp_mkimage.py output), not a flat objcopy .bin -- handing it the flat
    binary fails the 0xE9 magic check before any byte reaches the chip.
    """
    esp32_dir = Path(__file__).resolve().parent.parent
    return str(esp32_dir / "build" / "esp32c6-blink.app.bin")


def parse_args(argv):
    """Return (image, port, run) from a tiny hand-rolled argument scan."""
    image = None
    port = None
    run = False
    index = 1
    while index < len(argv):
        arg = argv[index]
        if arg == "--run":
            run = True
        elif arg == "--port":
            index += 1
            port = argv[index] if index < len(argv) else None
        elif image is None:
            image = arg
        index += 1
    if image is None:
        image = default_image()
    return (image, port, run)


def main(argv):
    """Report the roadmap flashing plan; optionally show the esptool command."""
    image, port, run = parse_args(argv)

    print("esp_flash: ROADMAP -- our-own ROM SLIP downloader is not implemented yet.")
    print(f"esp_flash: target chip {DEFAULT_CHIP}, RAM-app load 0x{HP_SRAM_BASE:08x}")
    print(f"esp_flash: image {image}")

    if not Path(image).is_file():
        print(
            "esp_flash: (image not built yet -- run 'make -C esp32' then"
            " 'python3 tools/esp_mkimage.py' first)"
        )

    esptool = shutil.which("esptool.py") or shutil.which("esptool")
    if esptool is None:
        print("esp_flash: esptool not found on PATH; install it for the interim transport.")
    else:
        port_arg = port if port is not None else "<PORT>"
        print(f"esp_flash: interim transport available: {esptool}")
        print("esp_flash: it would run ->")
        print(
            f"  {esptool} --chip {DEFAULT_CHIP} --port {port_arg} "
            f"--baud {DEFAULT_BAUD} load_ram {image}"
        )

    if run:
        print("esp_flash: --run is a no-op in the stub (no chip is driven yet).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
