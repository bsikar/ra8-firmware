# ereader_comic

Headless gate for the comic-book-archive reader (`libs/ra8_comic`): open a baked
**CBZ** (ZIP of images) and a baked **CBR** (RAR of images), decode page 0 of
each through the zero-heap `ra8_img_decode_blit` pipeline into a 160x120 RGB565
framebuffer, and print a deterministic FNV-1a-32 of the framebuffer over the
SCI8 J-Link OB console.

```
ereader-comic-hil: cbz pages=<N> <W>x<H> crc=<8hex> cbr pages=<M> <W>x<H> crc=<8hex> ok
```

Both fixtures carry the **same** first page image, so the two CRCs are
identical -- proving the CBZ (streaming miniz ZIP) and CBR (first-party
clean-room RAR) backends extract byte-for-byte the same page and render
interchangeably.

The fixtures (`comic_hil_fixture.h`) are baked byte arrays: a two-page ZIP (one
STORE entry, one DEFLATE entry) and a two-page RAR5 STORE archive, each holding
tiny grayscale PNGs. No SD card, panel, or touch is required.

## Status

`hw_pending`: exercised on host + `board_sim` (the pinned CRC in `hil.conf`).
Not yet silicon-validated (no rig in this workflow).

## Build / run

```
make                       # cross-compile -> build/ereader_comic.elf
make -C ../../../.. sim-ereader_comic   # run through board_sim, print the banner
```

## Scope

This app is the end-to-end proof that `ra8_comic` opens, pages, extracts, and
renders both container families on the real cross-compiled target. Wiring comic
support into the full `ereader_shelf` UI (a full-page image reader screen, the
SD-scan `.CBZ`/`.CBR` extension detection, RTL page-turn) is tracked as a
follow-up. Full RAR compression (beyond STORE) is likewise a follow-up; a
RAR-compressed page is enumerated but reported unsupported.
