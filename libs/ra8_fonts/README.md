# libs/ra8_fonts/

Project-curated font assets for the rendering stack (`libs/ra8_reflow`,
`libs/ra8_gfx`).

## Layout

| Subdir / file          | What it is                                       |
|------------------------|--------------------------------------------------|
| `Literata-Regular.ttf` | Source vector font: Literata Regular (SIL OFL).  |
| `literata_latin1.ttf`  | Latin-1 subset baked into flash (see below).     |
| `literata_latin1.h`    | `extern` decls for the baked subset array.       |
| `Literata-OFL.txt`     | SIL Open Font License 1.1 text (Literata).       |

`Literata-Regular.ttf` is committed here as the reading-font asset. Literata
is Google's open (SIL OFL 1.1) serif family, drawn specifically for long-form
on-screen reading in Play Books. The reflow engine (`libs/ra8_reflow`)
rasterises glyphs from it at runtime through the vendored `stb_truetype` (which
supports both TrueType and CFF/OpenType outlines), so no offline
bitmap-conversion step is needed -- apps hand the raw `.ttf` bytes to
`ra8_reflow_init()`.

`literata_latin1.ttf` is a ~37 KB Latin-1 + common-typographic subset produced
with `pyftsubset` (recipe in `scripts/gen/font_to_c.py`). The e-reader apps
bake it into `.rodata` at build time via `scripts/gen/font_to_c.py`, declared
by `literata_latin1.h`; the generated hex array is not committed.

The host reflow tests load these files directly (see `tests/test_ra8_reflow.c`
and `tests/test_ra8_reflow_render.c`); on the target the bytes are embedded or
read from storage.

Provenance and license are catalogued in the SBOM
(`docs/sbom/ra8-firmware.cdx.json`) and `THIRD_PARTY_LICENSES.md`.
