#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate the ra8_viewer malformed-input security corpus.

The viewer opens attacker-supplied JOF atlases and comic archives and sizes
caller-owned workspace regions from their metadata. This script emits a small
corpus that exercises both sides of that policy:

  * malicious fixtures that MUST be refused with a clean ra8_err_t (process exit
    1), never an OOM, an abort, or a hang; and
  * legitimate fixtures that MUST still decode (exit 0, a P6 PPM written).

Everything here is pure Python standard library so the corpus regenerates on any
CI runner with no third-party dependency (unlike make_fixture.py, which needs
Pillow). Integers on disk are little-endian unless a format dictates otherwise.
"""

from __future__ import annotations

import gzip
import io
import struct
import sys
import zipfile
import zlib
from pathlib import Path
from typing import NamedTuple

# --- shared sizing knobs (kept well clear of / above codec policy) -----------
# The viewer enforces a 64 MiB per-unit output cap and a 1024:1 ratio bound
# (ra8_decomp_limits_default). These constants push a crafted fixture safely
# past one of those bounds so the guard, not luck, decides the outcome.
MIB = 1024 * 1024
OVER_CAP_BYTES = 128 * MIB  # > 64 MiB output cap  -> k_ra8_err_decomp_output_cap
BOMB_UNCOMP_BYTES = 50 * MIB  # < cap but huge ratio -> k_ra8_err_decomp_ratio
UNWRAP_BOMB_BYTES = 160 * MIB  # > the 128 MiB gzip/xz unwrap arena
PAGE_NAME = "page01.jpg"  # Archive fixture entry name.
FILL_BYTE = 0x80  # decoded-pixel fill for generated atlases
EXPECTED_ARGC = 2  # argv is: script, out_dir

# --- JOF (RTA1 atlas) on-disk layout -----------------------------------------
JOF_MAGIC_HDR = b"JOF1"
JOF_MAGIC_FTR = b"JOFE"
JOF_HDR_BYTES = 32
JOF_FOOTER_BYTES = 16

# --- ustar (tar) header field offsets ----------------------------------------
TAR_SIZE_OFF = 124
TAR_CHKSUM_OFF = 148
TAR_MAGIC_OFF = 257
TAR_BLOCK = 512


class JofGeom(NamedTuple):
    """One JOF atlas geometry: image size, declared tile size, bytes per pixel."""

    width: int
    height: int
    tile_w: int
    tile_h: int
    bpp: int


def _ceil_div(a: int, b: int) -> int:
    """Return ceil(a / b) for positive integers (the JOF grid rule)."""
    return (a + b - 1) // b


def build_jof(geom: JofGeom, codec: int = 0) -> bytes:
    """Build a structurally-valid raw (codec 0) JOF atlas.

    The header carries the DECLARED tile_w/tile_h, but each tile stream holds
    only its edge-clamped payload (min(tile_w, width - x*tile_w) etc.), exactly
    as jof_produce writes it. That split is the point of the giant-tiles
    fixture: a 16x16 image can declare 65535x65535 tiles, so the file stays tiny
    while the viewer's band_bytes = tile_w*tile_h*bpp balloons to ~17 GiB.

    Args:
        geom: Atlas geometry. tile_w/tile_h may exceed the image size (the header
            does not clamp them); bpp is 1, 3 or 4.
        codec: Zero for raw tiles or one for raw-DEFLATE tiles.

    Returns:
        The complete atlas bytes (header + tiles + index + footer).
    """
    cols = _ceil_div(geom.width, geom.tile_w)
    rows = _ceil_div(geom.height, geom.tile_h)
    tile_count = cols * rows

    body = io.BytesIO()
    index: list[tuple[int, int]] = []
    offset = JOF_HDR_BYTES
    for ty in range(rows):
        for tx in range(cols):
            clamp_w = min(geom.tile_w, geom.width - tx * geom.tile_w)
            clamp_h = min(geom.tile_h, geom.height - ty * geom.tile_h)
            payload = bytes([FILL_BYTE]) * (clamp_w * clamp_h * geom.bpp)
            if codec == 1:
                compressor = zlib.compressobj(level=9, wbits=-15)
                stored = compressor.compress(payload) + compressor.flush()
            else:
                stored = payload
            body.write(stored)
            index.append((offset, len(stored)))
            offset += len(stored)

    index_off = offset
    header = struct.pack(
        "<4sHHHHBBHI12x",
        JOF_MAGIC_HDR,
        geom.width,
        geom.height,
        geom.tile_w,
        geom.tile_h,
        geom.bpp,
        codec,  # 0 = raw; 1 = raw DEFLATE
        0,  # reserved u16
        tile_count,
    )
    index_bytes = b"".join(struct.pack("<II", off, length) for off, length in index)
    total_size = index_off + len(index_bytes) + JOF_FOOTER_BYTES
    footer = struct.pack("<III4s", index_off, tile_count, total_size, JOF_MAGIC_FTR)
    return header + body.getvalue() + index_bytes + footer


def build_cbz(entry_name: str, data: bytes, forced_uncomp: int | None = None) -> bytes:
    """Build a one-entry ZIP, optionally forging its declared uncompressed size.

    zipfile writes the true sizes; when forced_uncomp is set, the central
    directory's uncompressed-size field is patched afterwards. miniz's
    mz_zip_reader_file_stat reads that field, so the comic backend sees
    the lie at open and ra8_decomp_check_declared refuses it before any inflate.

    Args:
        entry_name: Archive member name (must look like a page image).
        data: The real member bytes (kept tiny for a bomb).
        forced_uncomp: Value to write into the central-directory uncompressed
            size, or None to leave the honest size.

    Returns:
        The ZIP bytes.
    """
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(entry_name, data)
    raw = bytearray(buf.getvalue())
    if forced_uncomp is not None:
        sig = raw.find(b"PK\x01\x02")  # central directory file header
        if sig < 0:
            msg = "no central directory in generated zip"
            raise RuntimeError(msg)
        # CDH layout: sig(4) verMade(2) verNeed(2) flags(2) method(2) time(2)
        # date(2) crc(4) compSize(4) uncompSize(4) -> uncompressed at sig+24.
        struct.pack_into("<I", raw, sig + 24, forced_uncomp & 0xFFFFFFFF)
    return bytes(raw)


def build_cbt(entry_name: str, data: bytes, forced_size: int | None = None) -> bytes:
    """Build a one-member ustar tar, optionally forging the member size field.

    The CBT (tar) walker has no open-time declared-size guard, so a forged size
    is the viewer's own line of defence: viewer_read_page_bytes validates the
    member's declared size against the archive length before reserving a page
    buffer. The 512-byte header checksum is recomputed so the forged header is
    otherwise well-formed.

    Args:
        entry_name: Member name (a page image name).
        data: The real (tiny) member bytes.
        forced_size: Octal size to force into the header, or None for the honest
            size.

    Returns:
        The tar bytes (header + data + zero padding + two zero end blocks).
    """
    name = entry_name.encode("ascii")
    header = bytearray(TAR_BLOCK)
    header[0 : len(name)] = name
    header[100:108] = b"0000644\x00"  # mode
    header[108:116] = b"0000000\x00"  # uid
    header[116:124] = b"0000000\x00"  # gid
    real_size = len(data)
    size_field = forced_size if forced_size is not None else real_size
    header[TAR_SIZE_OFF : TAR_SIZE_OFF + 12] = f"{size_field:011o}\x00".encode("ascii")
    header[136:148] = b"00000000000\x00"  # mtime
    header[156:157] = b"0"  # typeflag: regular file
    header[TAR_MAGIC_OFF : TAR_MAGIC_OFF + 6] = b"ustar\x00"
    header[263:265] = b"00"
    header[TAR_CHKSUM_OFF : TAR_CHKSUM_OFF + 8] = b" " * 8  # spaces while summing
    chksum = sum(header) & 0o777777
    header[TAR_CHKSUM_OFF : TAR_CHKSUM_OFF + 8] = f"{chksum:06o}\x00 ".encode("ascii")

    pad = (-real_size) % TAR_BLOCK
    return bytes(header) + data + (b"\x00" * pad) + (b"\x00" * (2 * TAR_BLOCK))


def _write(out_dir: Path, name: str, blob: bytes) -> None:
    """Write blob to out_dir/name and report its size on stderr."""
    (out_dir / name).write_bytes(blob)
    sys.stderr.write(f"gen_corpus: {name:<22} {len(blob):8d} bytes\n")


def main() -> int:
    """Emit the whole corpus into the directory named by argv[1]."""
    if len(sys.argv) != EXPECTED_ARGC:
        sys.stderr.write("usage: gen_corpus.py <out_dir>\n")
        return 2
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)

    tiny = b"not a real image, refused before decode"

    # --- malicious: comic declared-size / ratio bombs ------------------------
    _write(out, "giant_decl.cbz", build_cbz(PAGE_NAME, tiny, forced_uncomp=OVER_CAP_BYTES))
    _write(out, "zip_bomb.cbz", build_cbz(PAGE_NAME, tiny, forced_uncomp=BOMB_UNCOMP_BYTES))
    _write(out, "giant_decl.cbt", build_cbt(PAGE_NAME, tiny, forced_size=OVER_CAP_BYTES))

    # --- malicious: truncated containers -------------------------------------
    full_cbz = build_cbz(PAGE_NAME, tiny)
    _write(out, "truncated.cbz", full_cbz[: len(full_cbz) // 3])
    full_cbt = build_cbt(PAGE_NAME, tiny)
    _write(out, "truncated.cbt", full_cbt[:200])
    _write(out, "garbage.cbr", b"Rar!\x1a\x07\x00" + b"\x00" * 64)  # not a decodable RAR

    # --- malicious: JOF absurd geometry / truncation -------------------------
    _write(out, "giant_tiles.jof", build_jof(JofGeom(16, 16, 65535, 65535, 4)))
    _write(out, "truncated.jof", build_jof(JofGeom(32, 32, 32, 32, 1))[:JOF_HDR_BYTES])

    # --- malicious: gzip-wrapped comic that overflows the unwrap arena -------
    _write(out, "unwrap_bomb.cbt.gz", gzip.compress(b"\x00" * UNWRAP_BOMB_BYTES, compresslevel=9))

    # --- legitimate: must still decode ---------------------------------------
    _write(out, "legit.jof", build_jof(JofGeom(32, 32, 32, 32, 1)))
    _write(out, "legit_deflate.jof", build_jof(JofGeom(32, 32, 32, 32, 1), codec=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
