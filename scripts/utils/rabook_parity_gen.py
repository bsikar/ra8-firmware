#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate the ra_rabook_compile byte-identity parity fixture header.

The on-device compiler (ra_rabook_compile_from_epub) must emit a RABOOK1 flat
blob byte-identical to the desktop reference tools/epub_compile/epub_compile.py
for the slices the pipeline fully supports (text-only / SVG / sub-1600px raster;
LANCZOS-vs-bilinear downscale can never match, so the fixture is pinned to a
well-formed text-only EPUB). This script bakes both sides of that comparison
into a C header the host test embeds:

  * s_parity_epub[]    -- the fixture .epub bytes (written to a RAM FAT volume
                          and opened with ra_epub, the test's compiler input).
  * s_parity_golden[]  -- the golden RABOOK1 flat blob: epub_compile.py run on
                          the same .epub, with its RBKC chunked container
                          stripped (header + chunk table) and inflated.

Run it via `make rabook-golden-update` after any change to the format, the
emitter, or the fixture; the committed header is then the frozen acceptance
target the parity test diffs against. The desktop tool needs Pillow installed.

Usage:
    rabook_parity_gen.py SRC_DIR OUT_HEADER [EXAMPLE_HEADER]
    rabook_parity_gen.py --realbook SRC_DIR OUT_HEADER

SRC_DIR holds the fixture's EPUB component files (META-INF/, OEBPS/); the
`mimetype` entry is synthesized as the fixed EPUB constant. Extend the fixture
(add CSS / a sub-1600px image) by editing SRC_DIR, then rerun.

The `--realbook` form bakes a single header (s_realbook_epub +
s_realbook_golden_noimg) from a real-book fixture (verbatim Standard Ebooks
chapters) using the desktop tool's --no-images path -- the text/CSS-only golden
the on-device skip-images compile must match byte-for-byte (#151).
"""

from __future__ import annotations

import io
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

# RBKC chunked container (keep in sync with ra_book_container_t in
# libs/ra_book/inc/ra_book.h): b"RBKC" + <I chunk_bytes + <Q total + <I count
# + <I reserved(0), a (count + 1)-entry <Q offset table, then count
# concatenated zlib streams.
_RBKC_MAGIC = b"RBKC"
_RBKC_HEADER_LEN = 24
_BYTES_PER_ROW = 12
# Fixed ZIP member timestamp so the built .epub is byte-deterministic.
_ZIP_EPOCH = (1980, 1, 1, 0, 0, 0)
_MIMETYPE = b"application/epub+zip"


def _build_epub(src_dir: Path) -> bytes:
    """Pack SRC_DIR into a byte-deterministic .epub (mimetype first, all STORED).

    Every member is stored uncompressed with a fixed timestamp so the embedded
    bytes never drift across zlib versions or run times; both the desktop tool
    and ra_epub read the members by name, so STORED is read identically.
    """
    members = sorted(p for p in src_dir.rglob("*") if p.is_file())
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        mt = zipfile.ZipInfo("mimetype", _ZIP_EPOCH)
        mt.compress_type = zipfile.ZIP_STORED
        zf.writestr(mt, _MIMETYPE)
        for path in members:
            rel = path.relative_to(src_dir).as_posix()
            info = zipfile.ZipInfo(rel, _ZIP_EPOCH)
            info.compress_type = zipfile.ZIP_STORED
            zf.writestr(info, path.read_bytes())
    return buf.getvalue()


def _compile_desktop(epub_bytes: bytes, *, no_images: bool = False) -> bytes:
    """Run epub_compile.py on the .epub bytes; return the inflated flat blob.

    When `no_images` is true the desktop tool is invoked with `--no-images`, so
    the emitted blob drops the image table + cover index (text/CSS-only). That
    is the golden the on-device skip-images path (scratch `skip_images = true`)
    must match byte-for-byte.
    """
    repo = Path(__file__).resolve().parents[2]
    tool = repo / "tools" / "epub_compile" / "epub_compile.py"
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "fixture.epub"
        out = Path(td) / "golden.rabook"
        src.write_bytes(epub_bytes)
        argv = [sys.executable, str(tool), str(src), str(out)]
        if no_images:
            argv.append("--no-images")
        subprocess.run(  # noqa: S603 -- fixed argv, trusted in-repo tool + local fixture
            argv,
            check=True,
            capture_output=True,
        )
        container = out.read_bytes()
    if container[: len(_RBKC_MAGIC)] != _RBKC_MAGIC:
        msg = "desktop output is not an RBKC container"
        raise ValueError(msg)
    chunk_bytes, want, count, reserved = struct.unpack_from("<IQII", container, 4)
    if reserved != 0 or chunk_bytes == 0 or count != (want + chunk_bytes - 1) // chunk_bytes:
        msg = "malformed RBKC header"
        raise ValueError(msg)
    offsets = struct.unpack_from(f"<{count + 1}Q", container, _RBKC_HEADER_LEN)
    payload = _RBKC_HEADER_LEN + 8 * (count + 1)
    blob = b"".join(
        zlib.decompress(container[payload + offsets[i] : payload + offsets[i + 1]])
        for i in range(count)
    )
    if len(blob) != want:
        msg = f"inflated size {len(blob)} != header {want}"
        raise ValueError(msg)
    return blob


def _emit_array(name: str, data: bytes) -> str:
    rows = []
    for i in range(0, len(data), _BYTES_PER_ROW):
        chunk = data[i : i + _BYTES_PER_ROW]
        rows.append("  " + "".join(f"0x{b:02X}, " for b in chunk).rstrip())
    body = "\n".join(rows)
    return f"static const uint8_t {name}[] = {{\n{body}\n}};\n"


def _render(epub_bytes: bytes, golden: bytes, golden_noimg: bytes) -> str:
    return (
        "/**\n"
        " * @file rabook_parity_fixture.h\n"
        " * @brief Byte-identity parity fixture for ra_rabook_compile (#151).\n"
        " *\n"
        " * @generated by scripts/utils/rabook_parity_gen.py "
        "(make rabook-golden-update);\n"
        " *            do not hand-edit. The generator owns this file's length:\n"
        " *            it bakes the fixture .epub plus the golden RABOOK1 flat\n"
        " *            blobs that tools/epub_compile/epub_compile.py emits for it,\n"
        " *            one with images (the default) and one with --no-images (the\n"
        " *            skip-images path: text/CSS-only, no image table or cover).\n"
        " *\n"
        " * @copyright Copyright (c) 2026 Brighton Sikarskie\n"
        " * SPDX-License-Identifier: MIT\n"
        " * @since 0.1.0\n"
        " */\n"
        "#pragma once\n"
        "\n"
        "#include <stdint.h>\n"
        "\n"
        "/** @brief Fixture .epub bytes (text-only, well-formed XHTML). */\n"
        f"{_emit_array('s_parity_epub', epub_bytes)}"
        "\n"
        "/** @brief Golden RABOOK1 flat blob (desktop epub_compile.py, "
        "RBKC-stripped). */\n"
        f"{_emit_array('s_parity_golden', golden)}"
        "\n"
        "/** @brief Golden RABOOK1 flat blob for --no-images "
        "(skip-images path). */\n"
        f"{_emit_array('s_parity_golden_noimg', golden_noimg)}"
        "\n"
        "/** @brief Sizes of the embedded fixture and golden blobs (bytes). */\n"
        "enum : uint32_t {\n"
        f"  k_parity_epub_len = {len(epub_bytes)}U,\n"
        f"  k_parity_golden_len = {len(golden)}U,\n"
        f"  k_parity_golden_noimg_len = {len(golden_noimg)}U,\n"
        "};\n"
    )


def _render_realbook(epub_bytes: bytes, golden_noimg: bytes) -> str:
    return (
        "/**\n"
        " * @file rabook_realbook_fixture.h\n"
        " * @brief Real-book byte-identity fixture for ra_rabook_compile (#151).\n"
        " *\n"
        " * @generated by scripts/utils/rabook_parity_gen.py "
        "(make rabook-golden-update);\n"
        " *            do not hand-edit. Built from real Standard Ebooks Walden\n"
        " *            chapters (tests/fixtures/rabook_realbook/) -- including the\n"
        " *            ones carrying the significant `</abbr> <abbr>` inter-element\n"
        " *            whitespace -- this bakes the fixture .epub plus the golden\n"
        " *            RABOOK1 flat blob that tools/epub_compile/epub_compile.py\n"
        " *            emits with --no-images (text/CSS-only). The on-device\n"
        " *            skip-images compile must equal it byte-for-byte, proving the\n"
        " *            chapter DOM (and its preserved inline whitespace) round-trips\n"
        " *            against the desktop reference on real content.\n"
        " *\n"
        " * @copyright Copyright (c) 2026 Brighton Sikarskie\n"
        " * SPDX-License-Identifier: MIT\n"
        " * @since 0.1.0\n"
        " */\n"
        "#pragma once\n"
        "\n"
        "#include <stdint.h>\n"
        "\n"
        "/** @brief Real-book fixture .epub bytes (verbatim Walden chapters). */\n"
        f"{_emit_array('s_realbook_epub', epub_bytes)}"
        "\n"
        "/** @brief Golden RABOOK1 flat blob for --no-images "
        "(skip-images path). */\n"
        f"{_emit_array('s_realbook_golden_noimg', golden_noimg)}"
        "\n"
        "/** @brief Sizes of the embedded fixture and golden blob (bytes). */\n"
        "enum : uint32_t {\n"
        f"  k_realbook_epub_len = {len(epub_bytes)}U,\n"
        f"  k_realbook_golden_noimg_len = {len(golden_noimg)}U,\n"
        "};\n"
    )


def _render_example(epub_bytes: bytes, golden: bytes) -> str:
    return (
        "/**\n"
        " * @file parity_fixture.h\n"
        " * @brief Baked parity .epub + golden blob for the M33 compile (#149).\n"
        " *\n"
        " * @generated by scripts/utils/rabook_parity_gen.py "
        "(make rabook-golden-update);\n"
        " *            do not hand-edit. The compile_on_m33 CPU1 image compiles\n"
        " *            s_m33_parity_epub on the Cortex-M33 and the M85 compares the\n"
        " *            emitted blob to s_m33_parity_golden (byte-identity on the\n"
        " *            secondary core). Same fixture + golden as the M85 parity gate.\n"
        " *\n"
        " * @copyright Copyright (c) 2026 Brighton Sikarskie\n"
        " * SPDX-License-Identifier: MIT\n"
        " * @since 0.1.0\n"
        " */\n"
        "#pragma once\n"
        "\n"
        "#include <stdint.h>\n"
        "\n"
        "/** @brief Fixture .epub bytes compiled on the M33 (text/CSS/SVG). */\n"
        f"{_emit_array('s_m33_parity_epub', epub_bytes)}"
        "\n"
        "/** @brief Golden RABOOK1 blob the M33 output must equal byte-for-byte. */\n"
        f"{_emit_array('s_m33_parity_golden', golden)}"
        "\n"
        "/** @brief Sizes of the embedded fixture and golden blobs (bytes). */\n"
        "enum : uint32_t {\n"
        f"  k_m33_parity_epub_len = {len(epub_bytes)}U,\n"
        f"  k_m33_parity_golden_len = {len(golden)}U,\n"
        "};\n"
    )


def _main_realbook(argv: list[str]) -> int:
    if len(argv) != 4:  # noqa: PLR2004 -- --realbook SRC_DIR OUT_HEADER
        sys.stderr.write("usage: rabook_parity_gen.py --realbook SRC_DIR OUT_HEADER\n")
        return 2
    src_dir = Path(argv[2])
    out = Path(argv[3])
    epub_bytes = _build_epub(src_dir)
    golden_noimg = _compile_desktop(epub_bytes, no_images=True)
    out.write_text(_render_realbook(epub_bytes, golden_noimg), encoding="ascii")
    sys.stdout.write(
        f"{out}: {len(epub_bytes)} B epub -> {len(golden_noimg)} B --no-images golden blob\n"
    )
    return 0


def main(argv: list[str]) -> int:
    if len(argv) >= 2 and argv[1] == "--realbook":  # noqa: PLR2004 -- mode flag
        return _main_realbook(argv)
    if len(argv) not in (3, 4):
        sys.stderr.write(
            "usage: rabook_parity_gen.py SRC_DIR OUT_HEADER [EXAMPLE_HEADER]\n"
            "       rabook_parity_gen.py --realbook SRC_DIR OUT_HEADER\n"
        )
        return 2
    src_dir = Path(argv[1])
    out = Path(argv[2])
    epub_bytes = _build_epub(src_dir)
    golden = _compile_desktop(epub_bytes)
    golden_noimg = _compile_desktop(epub_bytes, no_images=True)
    out.write_text(_render(epub_bytes, golden, golden_noimg), encoding="ascii")
    sys.stdout.write(
        f"{out}: {len(epub_bytes)} B epub -> {len(golden)} B golden blob "
        f"(+{len(golden_noimg)} B --no-images golden)\n"
    )
    if len(argv) == 4:  # noqa: PLR2004 -- the optional example-header arg
        example = Path(argv[3])
        example.write_text(_render_example(epub_bytes, golden), encoding="ascii")
        sys.stdout.write(f"{example}: example-side header written\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
