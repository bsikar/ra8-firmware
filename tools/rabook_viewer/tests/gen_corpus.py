#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate the ra8_viewer malformed-input security corpus.

The viewer opens attacker-supplied JOF atlases and comic archives and sizes
caller-owned workspace regions from their metadata. This script emits a small
corpus that exercises both sides of that policy:

  * malicious fixtures that MUST be refused with a clean ra8_err_t (process exit
    1), never an OOM, an abort, or a hang;
  * legitimate fixtures that MUST still decode (exit 0, a P6 PPM written); and
  * recognised-but-unwired fixtures (#849) that MUST be refused with the honest
    reason for the refusal -- a wrapped comic, an EPUB, a RABOOK, and an
    unrecognised extension -- so "not wired yet" can never quietly become
    "accepted and rendered wrong".

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
RBKC_MAGIC = b"RBKC"  # .rabook container magic (ra8_rabook_container.c).
RBKC_HEADER_BYTES = 24  # RBKC fixed header: magic, chunk, total, count, rsvd.
FILL_BYTE = 0x80  # decoded-pixel fill for generated atlases
MIN_ARGC = 2  # argv is: script, out_dir
MAX_ARGC = 3  # argv may add: a real comic archive to repack as CBT

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


def build_tar_member(entry_name: str, data: bytes, forced_size: int | None = None) -> bytes:
    """Build one ustar member (header + data + padding), optionally forging size.

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
        The member bytes: one 512-byte header, the data, and its zero padding.
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
    return bytes(header) + data + (b"\x00" * pad)


def build_cbt(entry_name: str, data: bytes, forced_size: int | None = None) -> bytes:
    """Build a one-member ustar tar terminated by the two zero end blocks.

    Args:
        entry_name: Member name (a page image name).
        data: The real (tiny) member bytes.
        forced_size: Octal size to force into the header, or None for the honest
            size.

    Returns:
        The tar bytes (member + two zero end blocks).
    """
    return build_tar_member(entry_name, data, forced_size) + (b"\x00" * (2 * TAR_BLOCK))


def build_cbt_from_comic(archive: Path) -> bytes:
    """Repack every page of a real ZIP comic into an equivalent ustar CBT.

    The member bytes are copied verbatim, so the CBT holds exactly the encoded
    images the committed .cbz golden already renders. That is the point: the two
    containers must decode to the same pixels, and only the tar index path
    differs between them.

    Args:
        archive: Path to a real one-or-more page CBZ.

    Returns:
        The CBT bytes.

    Raises:
        RuntimeError: The archive holds no page-image member.
    """
    members: list[bytes] = []
    with zipfile.ZipFile(archive) as zf:
        for info in sorted(zf.infolist(), key=lambda i: i.filename):
            if info.is_dir():
                continue
            if not info.filename.lower().endswith((".jpg", ".jpeg", ".png")):
                continue
            members.append(build_tar_member(info.filename, zf.read(info.filename)))
    if not members:
        msg = f"no page image inside {archive}"
        raise RuntimeError(msg)
    return b"".join(members) + (b"\x00" * (2 * TAR_BLOCK))


def build_epub() -> bytes:
    """Build a structurally valid minimal EPUB 3 publication.

    Valid on purpose: the viewer must refuse this because its reflow engine is
    not wired (#849), never because the file is malformed. `mimetype` is the
    first member and is stored uncompressed, as OCF requires, so a real EPUB
    reader would open it.

    Returns:
        The EPUB (ZIP) bytes.
    """
    container = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<container version="1.0" '
        'xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
        "  <rootfiles>\n"
        '    <rootfile full-path="OEBPS/content.opf" '
        'media-type="application/oebps-package+xml"/>\n'
        "  </rootfiles>\n"
        "</container>\n"
    )
    opf = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" '
        'unique-identifier="pub-id">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        '    <dc:identifier id="pub-id">urn:uuid:ra8-viewer-corpus</dc:identifier>\n'
        "    <dc:title>ra8_viewer corpus</dc:title>\n"
        "    <dc:language>en</dc:language>\n"
        '    <meta property="dcterms:modified">2026-01-01T00:00:00Z</meta>\n'
        "  </metadata>\n"
        "  <manifest>\n"
        '    <item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml"/>\n'
        "  </manifest>\n"
        '  <spine>\n    <itemref idref="ch1"/>\n  </spine>\n'
        "</package>\n"
    )
    chapter = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>1</title></head>\n'
        "<body><h1>Chapter 1</h1><p>Reflowable text the viewer cannot lay out "
        "yet.</p></body></html>\n"
    )
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", zipfile.ZIP_STORED)
        zf.writestr("META-INF/container.xml", container, zipfile.ZIP_DEFLATED)
        zf.writestr("OEBPS/content.opf", opf, zipfile.ZIP_DEFLATED)
        zf.writestr("OEBPS/ch1.xhtml", chapter, zipfile.ZIP_DEFLATED)
    return buf.getvalue()


def build_rabook_stub() -> bytes:
    """Build a .rabook whose RBKC magic is real and whose body is deliberately not.

    The exporter that emits real RBKC containers is not wired into this corpus,
    and forging a chunk table here would assert a layout this script does not
    own. The viewer classifies `.rabook` by extension and refuses it before any
    byte is parsed, so the magic plus a zeroed fixed header is exactly enough to
    gate that refusal. Replace this fixture with exporter output when the reflow
    engine lands (#849).

    Returns:
        The stub container bytes.
    """
    return RBKC_MAGIC + (b"\x00" * (RBKC_HEADER_BYTES - len(RBKC_MAGIC)))


def _write(out_dir: Path, name: str, blob: bytes) -> None:
    """Write blob to out_dir/name and report its size on stderr."""
    (out_dir / name).write_bytes(blob)
    sys.stderr.write(f"gen_corpus: {name:<22} {len(blob):8d} bytes\n")


def main() -> int:
    """Emit the whole corpus into the directory named by argv[1].

    An optional argv[2] names a real CBZ whose pages are repacked as a
    legitimate CBT (and as a legitimate gzip-wrapped CBT), so the tar reader and
    the wrapper refusal are both covered by content that genuinely decodes.
    """
    if not (MIN_ARGC <= len(sys.argv) <= MAX_ARGC):
        sys.stderr.write("usage: gen_corpus.py <out_dir> [sample_comic.cbz]\n")
        return 2
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    sample = Path(sys.argv[2]) if len(sys.argv) == MAX_ARGC else None

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

    # --- recognised but unwired: must be refused with the honest reason (#849)
    _write(out, "sample.epub", build_epub())
    _write(out, "sample.rabook", build_rabook_stub())
    _write(out, "notes.pdf", b"%PDF-1.7\n% not a book format the viewer knows\n")
    if sample is not None:
        legit_cbt = build_cbt_from_comic(sample)
        _write(out, "legit.cbt", legit_cbt)
        _write(out, "legit.cbt.gz", gzip.compress(legit_cbt, compresslevel=6))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
