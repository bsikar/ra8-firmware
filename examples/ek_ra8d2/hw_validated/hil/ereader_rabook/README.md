# ereader_rabook

Loads a compiled `.rabook` and renders it -- the on-silicon proof of the
`book` pipeline. It validates the flat blob (magic, table bounds, CRC-32),
walks each chapter's pre-parsed DOM back out as XHTML so the existing
`reflow` engine can consume it unchanged, paginates, renders every page into
an RGB565 framebuffer and hashes the output. The demo book's first chapter is
short and its second much longer, so one run covers small-to-large pagination.
Headless -- no panel, SD or touch.

A second one-image book in the same gate is the #476 proof: a raster retained at
full source resolution in continuous-tone gray8 and blitted 1:1. More than
sixteen distinct tones is impossible for a 4bpp store, so that check cannot pass
on a quantised copy.

The fixture is baked already-inflated, so the gate needs no decompressor. A real
device instead reads the compressed `.rabook` off SD and inflates it through
`book_open()` with a miniz-backed inflate adapter, sizes the scratch buffer
to `k_book_library_max_inflated` from the generated `book_library.h`,
and places both the scratch and the framebuffer in SDRAM rather than SRAM.
