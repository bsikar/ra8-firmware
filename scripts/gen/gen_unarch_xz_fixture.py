#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""gen_unarch_xz_fixture.py -- regenerate tests/unarch_xz_fixture.h.

The XZ decoder tests (tests/test_ra8_unarch_xz.c) need real .xz streams
with controlled properties: integrity-check type (CRC32 / CRC64 / SHA-256),
LZMA2 dictionary size, and compression ratio. Those bytes cannot be built
portably at test runtime (the C tree has no XZ *encoder*), so this script
materialises them once with Python's lzma module and emits a committed
C header of byte arrays. Re-run it only when a fixture needs to change:

    python3 scripts/gen/gen_unarch_xz_fixture.py

Fixture inventory (all payloads reproducible in C, see the header):
  fx_xz_crc64_4k   4 KiB LCG payload, dict 64 KiB, CRC64 (the xz default)
                   -- stream > 512 B so the unwrap loop takes several
                   input-refill passes.
  fx_xz_crc32      328 B text payload, dict 64 KiB, CRC32.
  fx_xz_sha256     same payload, SHA-256 check -- xz-embedded is built
                   without SHA-256, so this must be REJECTED cleanly.
  fx_xz_bigdict    same payload, dict 8 MiB -- must be rejected by a
                   session whose scratch dictionary budget is smaller.
  fx_xz_bomb_1m    1 MiB of zeros in a 284 B stream (~3690:1) -- breaches
                   the DEFAULT decompression-limits ratio bound
                   (1024:1 + 64 KiB grace) and must be rejected mid-decode.
  fx_xz_cbt        an XZ-wrapped ustar comic (two .png page members with
                   the payloads "PAGE-ONE" / "PAGE-TWO") for the
                   ra8_comic_open_wrapped .tar.xz path.

Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import io
import lzma
import tarfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
OUT_PATH = REPO_ROOT / "tests" / "unarch_xz_fixture.h"

LCG_SEED = 0x12345678
"""Seed of the payload LCG; tests/test_ra8_unarch_xz.c re-derives the
payload with the same constants to verify decoded bytes."""

LCG_MUL = 1103515245
LCG_ADD = 12345


def lcg_bytes(n: int, seed: int = LCG_SEED) -> bytes:
    """The C-reproducible pseudo-random payload generator."""
    out = bytearray()
    s = seed
    for _ in range(n):
        s = (LCG_MUL * s + LCG_ADD) & 0xFFFFFFFF
        out.append((s >> 16) & 0xFF)
    return bytes(out)


def xz(payload: bytes, check: int, dict_size: int) -> bytes:
    """One .xz stream with an explicit check type and LZMA2 dictionary."""
    filters = [{"id": lzma.FILTER_LZMA2, "preset": 6, "dict_size": dict_size}]
    return lzma.compress(payload, format=lzma.FORMAT_XZ, check=check, filters=filters)


def c_array(name: str, data: bytes) -> str:
    """Emit one static C byte array (12 bytes per line, pure ASCII)."""
    lines = [f"/** @brief {name}: {len(data)} fixture bytes (see file header). */"]
    lines.append(f"static const uint8_t {name}[{len(data)}U] = {{")
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02X}U" for b in data[i : i + 12])
        lines.append(f"  {chunk},")
    lines.append("};")
    return "\n".join(lines)


def cbt_tar() -> bytes:
    """A deterministic two-page ustar comic (the .tar.xz fixture core)."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        for name, payload in (
            (b"page2.png", b"PAGE-TWO"),
            (b"page1.png", b"PAGE-ONE"),
        ):
            info = tarfile.TarInfo(name.decode("ascii"))
            info.size = len(payload)
            info.mtime = 0
            tf.addfile(info, io.BytesIO(payload))
    return buf.getvalue()


def main() -> int:
    """Emit the xz decoder fixtures covering each supported integrity check.

    Generates one stream per check type (CRC32, CRC64, SHA256) and per
    dictionary size, because the decoder's bounds handling differs by both --
    a fixture set covering only the default would leave the fail-closed paths
    unexercised.
    """
    small = b"hello xz stream, bounded and fail-closed\n" * 8
    dict_64k = 1 << 16
    dict_8m = 1 << 23

    arrays = [
        ("k_fx_xz_crc64_4k", xz(lcg_bytes(4096), lzma.CHECK_CRC64, dict_64k)),
        ("k_fx_xz_crc32", xz(small, lzma.CHECK_CRC32, dict_64k)),
        ("k_fx_xz_sha256", xz(small, lzma.CHECK_SHA256, dict_64k)),
        ("k_fx_xz_bigdict", xz(small, lzma.CHECK_CRC64, dict_8m)),
        ("k_fx_xz_bomb_1m", xz(b"\x00" * (1 << 20), lzma.CHECK_CRC64, dict_64k)),
        ("k_fx_xz_cbt", xz(cbt_tar(), lzma.CHECK_CRC64, dict_64k)),
    ]

    body = "\n\n".join(c_array(n, d) for n, d in arrays)
    header = f"""/**
 * @file unarch_xz_fixture.h
 * @brief Committed .xz byte fixtures for the XZ decoder tests.
 *
 * @details
 * GENERATED FILE -- regenerate with:
 *     python3 scripts/gen/gen_unarch_xz_fixture.py
 * See that script for the fixture inventory and rationale. The LCG payload
 * of `k_fx_xz_crc64_4k` is re-derivable in C: byte i is
 * `(state >> 16) & 0xFF` after `state = 1103515245 * state + 12345`
 * (uint32 wrap-around) from seed 0x12345678.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

{body}
"""
    OUT_PATH.write_text(header, encoding="ascii")
    print(f"wrote {OUT_PATH} ({OUT_PATH.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
