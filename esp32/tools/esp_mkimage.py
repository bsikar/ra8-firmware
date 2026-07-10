#!/usr/bin/env python3
"""First-cut Espressif app-image packer for the ESP32-C6 spike.

Wraps a flat firmware ``.bin`` in the Espressif application-image container so
the chip's ROM (or a future our-own 2nd-stage loader) can boot it, and so the
same artifact can later carry the OTA / USB-update payload.

Container layout emitted (all little-endian):

    +-------------------+  24-byte image header (magic 0xE9, ...)
    | image header      |
    +-------------------+  per loadable segment:
    | segment header    |    load_addr (u32), data_len (u32)
    | segment data      |    data_len bytes
    +-------------------+  ... repeated for each segment ...
    | zero padding      |  so the checksum byte lands on a 16-byte boundary
    | checksum (1 byte) |  XOR of every segment data byte, seeded with 0xEF
    +-------------------+
    | SHA-256 (32 byte) |  digest of everything above (header .. checksum)
    +-------------------+

WARNING: the exact header field layout, the ESP32-C6 chip-id, and the
flash-mode/size/frequency byte encodings below are taken from general knowledge
of the Espressif image format, NOT from an open ESP32-C6 TRM / esptool source
that was cross-checked here. Every such value is tagged ``# [CONFIRM]``. Verify
against the C6 TRM and esptool's ``esp_image_format`` before trusting on silicon.

For the spike the input is the flat RAM-app ``.bin``; it is packed as a single
load segment at the HP SRAM base with the entry point at the same address (the
linker places ``_start`` first). Emitting one segment per ELF loadable section
is the roadmap refinement (drive it from the ELF, not the flat ``.bin``).

Pure python3 standard library. Usage:

    python3 tools/esp_mkimage.py [input.bin] [output.app.bin]

Defaults (resolved relative to this script) are build/esp32c6-blink.bin and
build/esp32c6-blink.app.bin.
"""

import hashlib
import os
import struct
import sys

# --- Espressif image constants (see module WARNING; all [CONFIRM]) -----------
ESP_IMAGE_MAGIC = 0xE9        # header byte 0                       # [CONFIRM]
ESP_CHECKSUM_SEED = 0xEF      # XOR seed for the segment checksum   # [CONFIRM]
ESP_IMAGE_HEADER_LEN = 24     # bytes in the fixed image header     # [CONFIRM]
ESP_SEGMENT_ALIGN = 16        # checksum lands on this boundary     # [CONFIRM]
SHA256_LEN = 32               # appended digest length

CHIP_ID_ESP32C6 = 0x000D      # esp_chip_id_t for the C6            # [CONFIRM]
FLASH_MODE_DIO = 0x02         # header byte 2 (spi mode)            # [CONFIRM]
FLASH_FREQ_40M = 0x00         # header byte 3, low nibble           # [CONFIRM]
FLASH_SIZE_2MB = 0x10         # header byte 3, high nibble          # [CONFIRM]
HASH_APPENDED = 0x01          # header byte 23: SHA-256 is appended # [CONFIRM]
MIN_CHIP_REV_FULL = 0x0000    # accept any silicon revision         # [CONFIRM]
MAX_CHIP_REV_FULL = 0xFFFF    # accept any silicon revision         # [CONFIRM]

# --- Spike load geometry (matches boot/esp32c6.ld) ---------------------------
HP_SRAM_BASE = 0x40800000     # TRM Ch "System and Memory"          # [CONFIRM]


def build_image_header(segment_count, entry_addr):
    """Return the 24-byte Espressif image header for a C6 app image."""
    freq_size = (FLASH_SIZE_2MB & 0xF0) | (FLASH_FREQ_40M & 0x0F)
    header = bytearray()
    header.append(ESP_IMAGE_MAGIC)                       # 0x00 magic
    header.append(segment_count & 0xFF)                  # 0x01 segment count
    header.append(FLASH_MODE_DIO & 0xFF)                 # 0x02 spi mode
    header.append(freq_size & 0xFF)                      # 0x03 freq | size
    header += struct.pack("<I", entry_addr)              # 0x04 entry point
    header.append(0x00)                                  # 0x08 wp pin (disabled)
    header += bytes(3)                                   # 0x09 spi pin drive[3]
    header += struct.pack("<H", CHIP_ID_ESP32C6)         # 0x0C chip id
    header.append(0x00)                                  # 0x0E min rev (legacy)
    header += struct.pack("<H", MIN_CHIP_REV_FULL)       # 0x0F min rev full
    header += struct.pack("<H", MAX_CHIP_REV_FULL)       # 0x11 max rev full
    header += bytes(4)                                   # 0x13 reserved[4]
    header.append(HASH_APPENDED & 0xFF)                  # 0x17 hash appended
    if len(header) != ESP_IMAGE_HEADER_LEN:
        raise ValueError("image header is %d bytes, expected %d"
                         % (len(header), ESP_IMAGE_HEADER_LEN))
    return bytes(header)


def build_segment(load_addr, data):
    """Return one segment: 8-byte header (load_addr, len) followed by data."""
    return struct.pack("<II", load_addr, len(data)) + data


def segment_checksum(segments):
    """XOR every segment data byte, seeded with ESP_CHECKSUM_SEED."""
    checksum = ESP_CHECKSUM_SEED
    for _load_addr, data in segments:
        for byte in data:
            checksum ^= byte
    return checksum & 0xFF


def pack(payload, load_addr, entry_addr):
    """Pack a single-segment app image and return the container bytes."""
    segments = [(load_addr, payload)]
    body = bytearray(build_image_header(len(segments), entry_addr))
    for seg_load_addr, seg_data in segments:
        body += build_segment(seg_load_addr, seg_data)
    # Pad with zeros so the trailing 1-byte checksum ends a 16-byte block.
    pad = (ESP_SEGMENT_ALIGN - ((len(body) + 1) % ESP_SEGMENT_ALIGN)) % ESP_SEGMENT_ALIGN
    body += bytes(pad)
    body.append(segment_checksum(segments))
    body += hashlib.sha256(bytes(body)).digest()
    return bytes(body)


def default_paths():
    """Return (input, output) defaults relative to this script's esp32/ dir."""
    here = os.path.dirname(os.path.abspath(__file__))
    esp32_dir = os.path.dirname(here)
    build_dir = os.path.join(esp32_dir, "build")
    return (os.path.join(build_dir, "esp32c6-blink.bin"),
            os.path.join(build_dir, "esp32c6-blink.app.bin"))


def main(argv):
    """Read the input .bin, pack it, write the .app.bin, print a summary."""
    default_in, default_out = default_paths()
    input_path = argv[1] if len(argv) > 1 else default_in
    output_path = argv[2] if len(argv) > 2 else default_out

    if not os.path.isfile(input_path):
        sys.stderr.write("esp_mkimage: input not found: %s\n" % input_path)
        sys.stderr.write("esp_mkimage: build it first with 'make -C esp32'\n")
        return 1

    with open(input_path, "rb") as handle:
        payload = handle.read()

    image = pack(payload, HP_SRAM_BASE, HP_SRAM_BASE)
    with open(output_path, "wb") as handle:
        handle.write(image)

    print("esp_mkimage: %s (%d bytes) -> %s (%d bytes)"
          % (input_path, len(payload), output_path, len(image)))
    print("esp_mkimage: 1 segment @ 0x%08x, entry 0x%08x, magic 0x%02x, "
          "SHA-256 appended" % (HP_SRAM_BASE, HP_SRAM_BASE, ESP_IMAGE_MAGIC))
    print("esp_mkimage: NOTE header layout is [CONFIRM] vs the C6 TRM / esptool")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
