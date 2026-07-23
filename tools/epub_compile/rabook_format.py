"""The .rabook on-disk constants, kept in lockstep with libs/ra8_book/inc/ra8_book.h.

Everything here is a contract with the FIRMWARE, not with this tool: a value
changed on one side and not the other produces a blob the device parses into
nonsense rather than a build error. Isolated in one small module so that
contract is one file to read next to the header it mirrors.

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

import struct
import zlib

# --- on-disk constants, kept in lockstep with libs/ra8_book/inc/ra8_book.h ------
MAGIC = b"RABOOK1\x00"
FORMAT_VERSION = 1
NIL = 0xFFFFFFFF
NODE_ELEMENT = 0
NODE_TEXT = 1
IMG_GRAY4 = 0
IMG_SVG = 1
# ra8_book_image_pixfmt_t: the raster depth stored in the image descriptor's
# former padding byte (ra8_book_image_t.pixel_format). 0 is the default that
# every pre-field .rabook already carried, so the firmware reads old blobs
# unchanged; a grayscale device emits gray4 (half the bytes), a deeper panel
# gray8 (lossless). Kept in lockstep with ra8_book.h.
PIXFMT_GRAY4 = 0
PIXFMT_GRAY8 = 1
# Header feature-flag bits (ra8_book_flag_t). The firmware validator rejects any
# bit outside its known mask, so only emit bits defined there.
FLAG_RTL = 0x00000001
# .rabook chunked container ("RBKC"; keep in sync with ra8_book_container_t in
# libs/ra8_book/inc/ra8_book.h):
#   "RBKC" + <I chunk_bytes + <Q inflated_total + <I chunk_count + <I reserved(0)
#   + <Q offset[chunk_count + 1] (payload-relative stream offsets)
#   + chunk_count concatenated zlib streams, one per chunk_bytes slice of the
#     flat blob (last slice short).
# Every chunk inflates independently, so the device can either inflate all of
# them into SDRAM (resident open) or inflate single chunks on demand into
# ra8_vmem cache frames (multi-GB books). chunk_bytes must equal the reader's
# cache frame size; 64 KiB is the current firmware default.
CONTAINER_MAGIC = b"RBKC"
CONTAINER_CHUNK_BYTES = 65536


def wrap_container(blob: bytes, chunk_bytes: int = CONTAINER_CHUNK_BYTES) -> bytes:
    """Wrap a flat RABOOK1 blob in the chunked RBKC container."""
    if not blob:
        msg = "empty blob"
        raise ValueError(msg)
    if chunk_bytes <= 0:
        msg = "chunk_bytes must be positive"
        raise ValueError(msg)
    count = (len(blob) + chunk_bytes - 1) // chunk_bytes
    streams = [
        zlib.compress(blob[i * chunk_bytes : (i + 1) * chunk_bytes], 9) for i in range(count)
    ]
    offsets = [0]
    for stream in streams:
        offsets.append(offsets[-1] + len(stream))
    header = CONTAINER_MAGIC + struct.pack("<IQII", chunk_bytes, len(blob), count, 0)
    table = b"".join(struct.pack("<Q", off) for off in offsets)
    return header + table + b"".join(streams)


# Downscale is OPT-IN (owner decision, issue #210): the default preserves the
# source resolution because any compile-time pixel loss is unrecoverable at
# zoom time (the planned press-and-hold loupe re-magnifies small manga text).
# 0 means no clamp. Pass --max-edge N to opt into a long-edge clamp where the
# smaller blob is worth it (e.g. TFT-class baked fixtures -- see
# scripts/builders/books.sh). FS dithering stays off because its high-frequency
