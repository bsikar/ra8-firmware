# libs/fonts/

Project-curated font assets for the rendering stack (`libs/ra_reflow`,
`libs/ra_gfx`).

## Layout

| Subdir / file               | What it is                                  |
|-----------------------------|---------------------------------------------|
| `<name>.otf` / `<name>.ttf` | Source vector font (TrueType / OpenType).   |

`ArnoPro-Regular.otf` is committed here as the reading-font asset. The
reflow engine (`libs/ra_reflow`) rasterises glyphs from it at runtime
through the vendored `stb_truetype` (which supports both TrueType and
CFF/OpenType outlines), so no offline bitmap-conversion step is needed --
apps hand the raw `.otf` bytes to `ra_reflow_init()`.

The host reflow tests load this file directly (see
`tests/test_ra_reflow.c`); on the target the bytes are embedded or read
from storage.
