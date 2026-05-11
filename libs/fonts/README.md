# libs/fonts/

Project-curated font assets for GUIX (and any future rendering
engine that consumes bitmap glyphs).

## Layout

| Subdir / file              | What it is                                              |
|----------------------------|---------------------------------------------------------|
| `<name>.otf` / `<name>.ttf` | Source vector font (TrueType / OpenType).               |
| `<name>_<size>bpp.c`       | Generated GUIX `GX_FONT` table (bitmap glyph arrays).   |

Source fonts are kept here so the conversion can be re-run when GUIX
rendering parameters change.  The generated C tables are committed
alongside them so apps don't need a working converter at build time.

## Why bitmaps instead of using the OTF at runtime

GUIX consumes a `GX_FONT` struct that points at pre-rasterized
glyph data (1bpp / 4bpp / 8bpp).  It does **not** parse OTF / TTF
files at runtime.  The vendor pipeline expects either:

1. **GUIX Studio** (Renesas, Windows-only GUI) -- imports an OTF /
   TTF, lets you pick a point size + character set, and exports a
   `<font>.c` file with a `GX_CONST GX_FONT my_font = {...}` table.
2. **FreeType + a custom emitter** -- link FreeType into a host
   build, render glyph bitmaps at a chosen DPI, then write out a
   GUIX-format C table by hand.  This is what the Renesas FSP
   examples do for embedded targets without GUIX Studio.

Until either is wired up, the apps in this tree use the three
GUIX-bundled system fonts that ship with the library itself
(`libs/third_party/guix/common/src/gx_system_font_*.c`):

| Symbol                 | Style              | Approximate size | Notes                        |
|------------------------|--------------------|------------------|------------------------------|
| `_gx_system_font_mono` | Monospaced, 1 bpp  | ~8x13 px         | Fastest to render            |
| `_gx_system_font_4bpp` | Anti-aliased, 4 bpp | ~8x13 px        | Smoother but heavier         |
| `_gx_system_font_8bpp` | Anti-aliased, 8 bpp | ~8x13 px        | Best quality, largest data   |

## Adding `ArnoPro-Regular.otf` for real

The file `ArnoPro-Regular.otf` is committed here as the source asset.
To make it usable from a GUIX app, run one of the conversion paths
above and commit the resulting `arnopro_<size>bpp.c` alongside.

`scripts/utils/` is the eventual home for a FreeType-based converter
(`fonts_emit_guix.py`) -- not yet implemented.
