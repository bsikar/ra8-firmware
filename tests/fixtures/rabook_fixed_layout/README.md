# Fixed-layout / image-only EPUB3 fixture (issue #196)

An unzipped, redistributable, fully-synthetic fixed-layout EPUB3 -- a comic /
manga "CBZ-in-EPUB-clothing": three pre-paginated spine documents, each a
single full-page raster with no flowable text. It exercises the fixed-layout
content model end to end through `tools/epub_compile/epub_compile.py` and is the
fixture the compiler's built-in check consumes:

```
python3 tools/epub_compile/epub_compile.py --selftest
```

The selftest zips this directory in memory, compiles it, compiles an equivalent
CBZ built from the same page images, and asserts the two `.rabook` blobs share
the one-image-per-page shape and byte-identical 4bpp image payloads.

## Layout (what each part probes)

| Part | Probes |
|------|--------|
| `content.opf` `rendition:layout=pre-paginated` (package + per-`itemref`) | compiler must not choke on fixed-layout metadata |
| `content.opf` cover via `properties="cover-image"` **only** (no legacy `<meta name="cover">`) | the EPUB3-only cover the desktop tool used to miss |
| `text/page1.xhtml` | bare `<img>`, image-only page, cross-directory `src` |
| `text/page2.xhtml` | `<meta name="viewport">` pinned page |
| `text/page3.xhtml` | inline `<svg><image xlink:href=...>` wrapper page |
| `images/page*.png` | real rasters transcoded to panel-native 4bpp (page3 is odd-width, so the nibble parity stagger is real) |

`images/*.png` are deterministic grayscale (`pixel(x,y) = (x*ax + y*ay + seed)
% 256`), regenerable with Pillow; they are tiny on purpose. `.png` is declared
`binary` in `.gitattributes`.

## Findings (2026-07-15, issue #196 spike)

The fixed-layout container parses and compiles cleanly -- the one-image-per-page
`.rabook` shape falls out of the existing EPUB path (each image-only spine
document becomes a chapter whose body DOM is a single `<img>`, and the manifest
rasters become the image table). The spike did surface real gaps:

1. **EPUB3-only cover was dropped (FIXED here).** `epub_compile.py` only detected
   the legacy `<meta name="cover">`; a fixed-layout book that declares its cover
   the modern way (`properties="cover-image"`) compiled with **no cover**, while
   the on-device compiler (`ra8_epub_xml_shim.c`) already resolved it. The
   desktop tool now mirrors the on-device precedence (properties first, legacy
   meta fallback), closing a host-vs-device divergence. The `rabook_parity`
   golden is unchanged (its fixture declares both cover forms).

2. **`<img src>` does not match the manifest image id (latent).** In the emitted
   `.rabook`, the DOM `<img src="../images/page1.png">` is the verbatim author
   string (relative to the XHTML), while the image table is keyed by the manifest
   href `images/page1.png`. For any real publisher layout (`text/` and `images/`
   in separate directories) these strings differ, so a render-time loader that
   resolves images by exact-string href match would miss. A CBZ sidesteps this
   (its synthesized `src` equals the image id). Not hit today (no production
   render path resolves book images by href -- see #3). Follow-up: normalise the
   `<img src>` against the manifest href at compile time, or resolve by
   normalised path on device.

3. **The `ereader_shelf` reader is text-only.** It extracts plain chapter text,
   folds to ASCII, word-wraps and paginates by text lines; it renders no inline
   images. A fixed-layout book has ~no body text, so it currently reads as a
   correct **cover** plus **blank** reading pages. `ra8_reflow` *can* lay an
   `<img>` out as a full-bleed block, but (a) it is not wired into the shelf
   reader, and (b) its image loader expects **encoded** bytes (PNG/JPEG/SVG,
   parsed by stb_image / the SVG rasteriser), whereas `.rabook` stores
   **already-decoded 4bpp** with no header -- so reflow cannot consume a
   `.rabook` raster as-is. Rendering fixed-layout books in the flow needs a
   book-backed image path (the `sh_image.c` 4bpp row-streaming decode, resolved
   by `<img src>` -> image index). Follow-up issue material.

Memory fit is a non-issue: the RBKC container + `ra8_vmem` demand paging already
carry books far larger than RAM, and `sh_image.c` decodes a full-page raster one
source row at a time from the page cache, so a single page image never needs to
be fully resident.
