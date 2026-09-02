# ereader_zoom

The other half of a decision already made. `.rabook` import deliberately does
**not** downscale images (#210-213, #476): the pixels are kept so the reader can
magnify into them. This app makes that magnifier viewable, and is the demo for
`apps/shared_libs/zoom` (#478).

It brings up the 1024x600 GLCDC panel, binds a 4096x3072 gray8 page -- an order
of magnitude past what this part can hold -- through an `ra8_tile_cache`, and
drives it with two viewports over the same engine: a full-content zoom viewport,
and a 320x320 loupe composited over it at a higher magnification.

## Why it looks like this

The page is procedural -- a triangle-wave gradient overlaid with 3 px "text"
rules broken into word-like runs -- rather than a baked fixture. That is not a
shortcut: the two things the viewer has to prove are best proven by content that
is generated, not stored.

* The **gradient** swings dozens of gray levels across roughly a screen. A
  16-level panel bands that visibly without the blue-noise dither (#477); with
  it, the grain is scattered and the ramp is smooth. Look at the render, not the
  hash.
* The **rules** are 3 px tall at source resolution. At fit-to-page they would be
  gone; at 1:1 they read as texture; at 4x they resolve into structure. That is
  what "no downscale" buys.
* Generating the page means the app carries no multi-megabyte fixture *and*
  exercises the identical `ra8_tile_cache` decode-on-miss, eviction and residency
  behaviour a real JOF atlas would, because the container lives behind the same
  seam.

## What the banner proves

The boot banner is not a printout, it is the golden -- `hil.conf` pins it and
`apps/board/stand_alone/ereader/tests/src/test_app_ereader_zoom.c` derives the identical numbers
from the identical
`ez_scene_selftest()` on the host. It carries a framebuffer hash for each of four
scripted viewport states, plus the tile-cache counters, and each is an assertion:

| Field | What it proves |
|---|---|
| the four state hashes | Each viewport state renders to a fixed framebuffer on host **and** emulator **and** silicon -- the whole path is integer. |
| miss count | Only the visible tiles are ever decoded; the whole four-state sequence touches a small fraction of the page. |
| evict count, which must be zero | No tile still on screen was thrown away. That is the thrash #338 describes, and `k_ez_cells` is sized to the viewport tile demand plus a pan margin so it cannot happen. |
| warm count | The pan read-ahead warmed the lead-edge column out of the cache's spare capacity (#341). |

The hashes are toolchain-independent **because the chrome carries no text**. A
framebuffer hash over antialiased glyphs is toolchain-bound -- the same board
prints one value from an image built by one compiler and another from the next --
so the status bar here is filled rectangles and a block zoom indicator.

## Memory

The framebuffer and the gray8 tile cache both live in SDRAM; the viewer's own
footprint is a few tens of KiB of SRAM scratch plus the tile-cache metadata. It
never holds the visible window -- 1024x600 gray8 could not be allocated at all
under NASA P10 Rule 3 -- so it composites in 16-row strips and the cost is
`O(dst.w * strip_rows)`, not `O(dst.w * dst.h)`. The large working set is the
tile cache, which is where a reader's image budget already lives.

## Interaction

Discrete tap zones (the GT911 path reports contacts, not drags, so there is no
swipe):

| Zone | Action |
|---|---|
| status bar | open / close the loupe |
| inside the loupe | cycle the loupe magnification |
| content, left / right / top / bottom band | pan the page by a viewport less a one-eighth overlap |
| content, centre | cycle the page magnification **about the tapped point** |

Zooming keeps the tapped point fixed under the finger. Panning is one
destination pixel of granularity at every magnification, because the viewport
anchor lives in the magnified image plane rather than in source pixels.

## e-ink behaviour

A pan moves every pixel in the viewport, so there is no smaller true dirty
region -- the viewport is flushed whole. What *is* exploited is the waveform: an
interactive burst flushes with the panel's bi-level A2 (far faster than GC16) and
`zoom_view_tick` promotes the view to a full 16-level GC16 repaint once the
gesture has been still. `k_zoom_policy_quality` opts out and pays full GC16
per step for photographic content.

The loupe is where partial update genuinely pays: cycling its magnification
changes only the lens box, so the flush covers the lens rather than the whole
content area.
