# Fixed-layout / image-only EPUB3 fixture (issue #196)

An unzipped, redistributable, fully-synthetic fixed-layout EPUB3 -- a comic /
manga "CBZ-in-EPUB-clothing": three pre-paginated spine documents, each a
single full-page raster with no flowable text. It exercises the fixed-layout
content model end to end through `tools/epub_compile/epub_compile.py` and is
the fixture that compiler's built-in selftest consumes: the selftest zips this
directory in memory, compiles it, compiles an equivalent CBZ built from the
same page images, and asserts the two `.rabook` blobs share the one-image-per-page
shape and byte-identical 4bpp image payloads.

## Layout (what each part probes)

| Part | Probes |
|------|--------|
| `content.opf` `rendition:layout=pre-paginated` (package + per-`itemref`) | compiler must not choke on fixed-layout metadata |
| `content.opf` cover via `properties="cover-image"` **only** (no legacy `<meta name="cover">`) | the EPUB3-only cover form, which the desktop tool once missed while the on-device compiler resolved it |
| `text/page1.xhtml` | bare `<img>`, image-only page, cross-directory `src` |
| `text/page2.xhtml` | `<meta name="viewport">` pinned page |
| `text/page3.xhtml` | inline `<svg><image xlink:href=...>` wrapper page |
| `images/page*.png` | real rasters transcoded to panel-native 4bpp (page3 is odd-width, so the nibble parity stagger is real) |

`images/*.png` are deterministic grayscale (`pixel(x,y) = (x*ax + y*ay + seed)
% 256`), regenerable with Pillow; they are tiny on purpose. `.png` is declared
`binary` in `.gitattributes`.

## Known divergences this fixture pins

The fixed-layout container parses and compiles cleanly -- the one-image-per-page
`.rabook` shape falls out of the existing EPUB path, since each image-only spine
document becomes a chapter whose body DOM is a single `<img>` and the manifest
rasters become the image table. Two real gaps remain, and this fixture is where
they are visible:

1. **`<img src>` does not match the manifest image id.** In the emitted
   `.rabook`, the DOM `<img src="../images/page1.png">` is the verbatim author
   string (relative to the XHTML), while the image table is keyed by the
   manifest href `images/page1.png`. For any real publisher layout (`text/` and
   `images/` in separate directories) these strings differ, so a render-time
   loader resolving images by exact-string href match would miss. A CBZ
   sidesteps this -- its synthesized `src` equals the image id. Nothing hits it
   today because no production render path resolves book images by href (see
   below). The fix is to normalise `<img src>` against the manifest href at
   compile time, or to resolve by normalised path on device.

2. **The `ereader_shelf` reader is text-only.** It extracts plain chapter text,
   folds to ASCII, word-wraps and paginates by text lines; it renders no inline
   images. A fixed-layout book has almost no body text, so it reads as a correct
   **cover** followed by **blank** pages. `ra8_reflow` *can* lay an `<img>` out
   as a full-bleed block, but it is not wired into the shelf reader, and its
   image loader expects **encoded** bytes (PNG/JPEG/SVG, parsed by stb_image or
   the SVG rasteriser) whereas `.rabook` stores **already-decoded 4bpp** with no
   header -- so reflow cannot consume a `.rabook` raster as-is. Rendering
   fixed-layout books in the flow needs a book-backed image path: the
   `sh_image.c` 4bpp row-streaming decode, resolved by `<img src>` to an image
   index.

Memory fit is a non-issue: the RBKC container plus `ra8_vmem` demand paging
already carry books far larger than RAM, and `sh_image.c` decodes a full-page
raster one source row at a time from the page cache, so a single page image
never needs to be fully resident.
