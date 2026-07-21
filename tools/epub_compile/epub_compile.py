#!/usr/bin/env python3
"""Compile an EPUB into a flat, execute-in-place .rabook blob.

The on-device reader (libs/ra8_book) never unzips or parses XHTML at runtime.
This host tool does it once: it unzips the EPUB, parses every spine document
into a faithful DOM (every tag, attribute and text run preserved), keeps each
stylesheet verbatim, transcodes raster images to the panel-native 4bpp
grayscale at source resolution (downscale is an opt-in --max-edge knob;
issue #210) and preserves SVG as
vector source, then serializes everything into the binary layout described by
libs/ra8_book/inc/ra8_book.h.

Fidelity is the rule: nothing in the markup is dropped to match what the
renderer understands today. The only content that changes form is raster
images, because the e-ink panel is physically 4bpp.

Usage:
    epub_compile.py INPUT.epub OUTPUT.rabook [--stats]

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from epub_pipeline import compile_epub
from epub_selftest import selftest
from rabook_blob import MAX_IMAGE_EDGE
from rabook_format import CONTAINER_CHUNK_BYTES, wrap_container


def main() -> int:
    """Parse the command line, compile, and write the container to disk.

    Two modes: `--selftest` runs the issue #196 fixed-layout self-check and
    ignores the positional arguments entirely, otherwise both input and output
    are required. The output written is the RBKC-wrapped container, not the raw
    blob -- `--chunk-bytes` must equal the reader's `ra8_vmem` frame size or the
    device cannot page the book.

    Errors are not caught here. A malformed EPUB surfaces as a traceback rather
    than a diagnostic; the exception type names the failing stage.

    Returns:
        0 on success. Non-zero exits arrive as SystemExit from argparse or the
        selftest, not through this return.
    """
    ap = argparse.ArgumentParser(description="Compile an EPUB into a .rabook blob.")
    ap.add_argument("input", nargs="?", help="source .epub")
    ap.add_argument("output", nargs="?", help="destination .rabook")
    ap.add_argument("--stats", action="store_true", help="print size/structure stats")
    ap.add_argument(
        "--max-edge",
        type=int,
        default=MAX_IMAGE_EDGE,
        help="opt-in: downscale raster image long edge to at most this many "
        "pixels (default 0 = preserve source resolution)",
    )
    ap.add_argument(
        "--no-images",
        action="store_true",
        help="drop all images (text-only); tiny blob for a baked fixture",
    )
    ap.add_argument(
        "--chunk-bytes",
        type=int,
        default=CONTAINER_CHUNK_BYTES,
        help="inflated bytes per independently-compressed container chunk "
        "(must equal the reader's ra8_vmem frame size)",
    )
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="compile the fixed-layout fixture and run the #196 self-check, then exit",
    )
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.input or not args.output:
        ap.error("input and output are required unless --selftest")

    blob, meta, bb = compile_epub(args.input, args.max_edge, args.no_images)
    container = wrap_container(blob, args.chunk_bytes)
    with Path(args.output).open("wb") as fh:
        fh.write(container)

    if args.stats:
        src = Path(args.input).stat().st_size
        out = len(container)
        print(f"{meta['title']} -- {meta['author']}")
        print(
            f"  chapters={len(bb.chapters)} nodes={len(bb.nodes)} "
            f"attrs={len(bb.attrs)} css={len(bb.stylesheets)} images={len(bb.images)}"
        )
        print(
            f"  epub={src // 1024} KB -> rabook={out // 1024} KB "
            f"({100 * out // max(src, 1)}%); inflated={len(blob) // 1024} KB"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
