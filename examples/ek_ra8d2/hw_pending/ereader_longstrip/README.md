# ereader_longstrip

The third e-reader reading mode (#289), beside reflowable EPUB text and paged
CBZ/manga: a chapter is one **continuous vertical strip** -- tall image slices
stacked seamlessly and read by scrolling, with no page boundaries. This app makes
the `apps/shared_libs/longstrip` scroll engine viewable by binding its band-composite
blit sink to the live 1024x600 GLCDC panel framebuffer, so an emulator window or
a captured frame shows the actual reader screen.

## What it shows

- **A tall strip that costs almost nothing to boot.** The strip is a JOF
  band-tile atlas, but only its header, index and footer are materialised: each
  band's pixels are painted procedurally on a tile-cache miss -- a vivid palette
  hue with a top-to-bottom gradient and a white seam stripe at every band edge,
  so scrolling is obviously visible. Boot is instant and no multi-megabyte pixel
  buffer is ever needed.
- **Bounded-memory streaming.** The `ra8_tile_cache` has fewer cells than the
  strip has bands, so the resident decoded-pixel set stays constant regardless of
  scroll distance; the LRU evicts and re-decodes as the viewport moves (#147).
- **Tap-zone navigation and chrome.** Bottom third pages down, top third pages
  up, centre toggles the chrome (a status bar with band, scroll percent and skip
  count, plus a right-edge scroll rail whose thumb tracks position). SW1/SW2 are
  a button fallback. Navigation is discrete-tap only -- the GT911 path reports
  contacts, not gestures -- so the app needs no emulator change to be driven.

After the first render the app folds an FNV-1a-32 over the whole panel
framebuffer and prints it. **The framebuffer bytes are written identically
whether or not a panel is attached**, so the hash is the same on the unit-test
host, in the emulator and on silicon. `hil.conf` pins the banner; the full
validation is the rendered panel itself.
