# ereader_chrome

Headless **on-silicon HIL gate** for the e-reader chrome render pipeline
(box model #76 + interaction core #80).

The chrome is golden-validated on the `board_sim` emulator (`make
ereader-golden`). This app closes the *real-hardware* gap with no panel / SDRAM
/ touch / SD dependency:

1. Build a representative chrome screen as an `ra8_box` tree -- a status bar over
   a 2-column grid of "book" cells.
2. `ra8_box_layout` it into a fixed `320x240` frame.
3. Render the boxes (fill + 1-px border) and labels (bundled
   `ra8_gfx_font_8x16`) into a static RGB565 framebuffer in internal SRAM.
4. Fold an **FNV-1a-32** hash over the whole framebuffer and print it on the
   SCI8 J-Link OB console:

   ```
   ereader-hil: chrome boxes=7 crc=0DCB740F
   ```

The gate (`hil.conf`, `uart_scrape`) asserts that line. Any drift in the
box-model math, the software rasteriser, or the toolchain output changes the
hash and trips the gate.

The render is deterministic (integer layout + a fixed bitmap font + a zeroed
static framebuffer), so the hash is the same every boot. It is also the *same*
hash the identical render produces on host -- so `crc=0DCB740F` doubles as a
sim/silicon equivalence check.

## Build + run

```
make ereader_chrome
scripts/hil/run_local.sh ereader_chrome      # flash + scrape the banner
```

## Result (validated 2026-06-18, EK-RA8D2 + J-Link OB)

```
ereader-hil: boot
ereader-hil: chrome boxes=7 crc=0DCB740F
[local PASS] ereader_chrome: saw 'ereader-hil: chrome boxes=7 crc=0DCB740F'
```

The same FNV-1a-32 computed on host over the identical `ra8_box` + `ra8_gfx`
render is `0DCB740F` -- byte-for-byte agreement between the host render and the
RA8D2 silicon.

## Updating the baseline

After an **intentional** change to the chrome tree or the render, recompute the
hash and update `HIL_EXPECT` in `hil.conf` (and the value above). The on-device
banner is the source of truth.
