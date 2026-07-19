# ereader_longstrip -- viewable continuous vertical-scroll reader (#289)

The third e-reader reading mode, beside reflowable EPUB text (`ra8_reflow`) and
paged CBZ/manga (`ra8_comic`): a chapter is one **continuous vertical strip** --
tall image slices stacked seamlessly and read by scrolling, with **no page
boundaries**. This app makes the scroll engine (`libs/ra8_longstrip`) **viewable**: the
engine's band-composite blit sink is bound to the live 1024x600 GLCDC panel
framebuffer, so board_sim's `--view` window (or a `--ppm` capture) shows the
actual reader screen.

## What it shows

1. **Live panel bring-up.** Boot clocks/MSTP/SysTick/console/LEDs, then external
   SDRAM + the 1024x600 RGB565 GLCDC panel through `ra8_display_pal`
   (`k_display_backend_lcd_ra8_glcdc`); `ra8_gfx` is bound to the panel FB.
2. **A tall colourful strip.** 16 bands of 1024 x 280 (a 1024 x 4480 canvas). The
   strip is a JOF band-tile atlas (`ra8_tileatlas`, one full-width band
   column), but only its header + index + footer are materialised (~176 bytes):
   each band's pixels are painted **procedurally** on a tile-cache miss -- a
   vivid palette hue with a top->bottom brightness gradient and a white seam
   stripe at every band edge, so scrolling is obviously visible. This keeps boot
   instant and needs no multi-megabyte pixel buffer.
3. **Bounded-memory streaming.** Bands page decode-on-demand through a 6-cell
   `ra8_tile_cache` (fewer cells than the 16 bands), so the resident
   decoded-pixel set stays constant regardless of scroll distance; the LRU
   evicts and re-decodes as the viewport moves (#147). `ra8_longstrip_render`
   composites the visible band range into the panel FB via `ra8_gfx_blit`.
4. **Tap-zone navigation + chrome.** Poll the GT911 (`ra8_touch`) and hit-test
   the tap: **bottom third** pages down, **top third** pages up, **centre third**
   toggles the chrome (a top status bar showing `band N/16`, scroll `pos %`, and
   `skip` count, plus a right-edge scroll rail whose thumb tracks the position).
   SW1/SW2 page down/up as a button fallback. Navigation is **discrete-tap only**
   -- there is no swipe/drag/pinch gesture (the board_sim GT911 model has none),
   so this app needs zero board_sim change.

## Boot banner (the headless golden)

```
ereader-longstrip: bands=16 view=1024x600 scroll=0 crc=795D27E6
```

After the first render the app folds an FNV-1a-32 over the whole 1024x600 panel
framebuffer and prints it. The FB bytes are written identically whether or not a
panel is attached, so the hash is the same on the unit-test host, in board_sim,
and on silicon (SIM == HIL). `hil.conf` pins the banner; the board-sim smoke gate
(`scripts/board_sim_smoke.sh`) asserts it in CI. The full validation is the
rendered panel itself.

## View / drive

```sh
make                                  # cross-compile -> build/ereader_longstrip.elf
make sim-ereader_longstrip            # live window: click to page, centre to toggle chrome

# Headless snapshots at three scroll positions (a discrete tap = one page):
SIM=../../../../tools/board_sim/build/board_sim
$SIM build/ereader_longstrip.elf --panel panels/ek_ra8d2.toml --ppm top.ppm
$SIM build/ereader_longstrip.elf --panel panels/ek_ra8d2.toml \
    --touch-seq "512:500,512:500,512:500,512:500" --ppm mid.ppm
$SIM build/ereader_longstrip.elf --panel panels/ek_ra8d2.toml \
    --touch-seq "512:500,512:500,512:500,512:500,512:500,512:500,512:500,512:500" \
    --ppm bottom.ppm
```

Each `--touch-seq` entry is served as one GT911 frame and pages once, so four
bottom-zone taps land near the middle and eight scroll to the end.

## Status

`hw_pending`: **sim-viewable** (board_sim panel render + deterministic banner).
The on-chip render is bench-runnable on a stock EK-RA8D2 (panel + SCI8 / J-Link
OB VCOM only, no external hardware), but has not yet been run on the rig. Promote
to `hw_validated/hil/` after a bench run confirms the same banner + a live panel.
