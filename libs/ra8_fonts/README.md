# libs/ra8_fonts/

The project's curated font assets for the rendering stack: Literata Regular,
its SIL OFL 1.1 licence text, and a subset of the face.

Literata is an open serif family drawn for long-form on-screen reading, which
is what the e-reader stack wants. The reflow engine rasterises its outlines at
runtime through the vendored `stb_truetype` (TrueType and CFF/OpenType alike),
so there is no offline bitmap-conversion step anywhere in the build -- an app
hands over raw `.ttf` bytes and gets glyphs back.

For targets that cannot spend flash on the whole face there is a Latin-1 plus
common-typographic subset, produced with `pyftsubset` and baked into `.rodata`
at build time by `scripts/gen/font_to_c.py`. The generated hex array is not
committed: only the `.ttf` it is generated from, and the header declaring the
symbols. Host reflow tests load these files straight off disk; on the target
the bytes are either embedded or read from storage.

The licence text ships beside the font because the OFL requires it. Provenance
is catalogued in the SBOM and `THIRD_PARTY_LICENSES.md`.
