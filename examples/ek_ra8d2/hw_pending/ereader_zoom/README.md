# ereader_zoom -- tap-to-zoom image viewer (full-screen zoom + panel loupe)

The other half of a decision already made. `.rabook` import deliberately does
**not** downscale images (#210-213, #476): the pixels are kept so the reader can
magnify into them. This app makes that magnifier viewable, and is the demo for
`libs/ra8_zoom` (#478).

It brings up the 1024x600 GLCDC panel, binds a **4096x3072 gray8 page** -- 12 MiB,
an order of magnitude past what this part can hold -- through an `ra8_tile_cache`,
and drives it with two viewports over the same engine:

* a **full-content zoom viewport** (1024x552, the panel less the status bar), and
* a **320x320 loupe** composited over it, magnified further than the page around it.

## Why it looks like this

The page is procedural -- a triangle-wave gradient overlaid with 3 px "text"
rules broken into word-like runs -- rather than a baked fixture. That is not a
shortcut: the two things the viewer has to prove are best proven by content that
is generated, not stored.

* The **gradient** swings 64 gray levels across roughly a screen. A 16-level
  panel bands that visibly without the blue-noise dither (#477); with it, the
  grain is scattered and the ramp is smooth. Look at the render, not the hash.
* The **rules** are 3 px tall at source resolution. At fit-to-page they would be
  gone; at 1:1 they read as texture; at 4x they resolve into structure. That is
  what "no downscale" buys.
* Generating the page means the app carries no multi-megabyte fixture *and*
  exercises the identical `ra8_tile_cache` decode-on-miss, eviction and residency
  behaviour a real JOF atlas would (the container lives behind the same seam).

## What it demonstrates, measured rather than asserted

The boot banner is not a printout, it is the golden -- see `hil.conf` and
`tests/test_app_ereader_zoom.c`, which derives the identical numbers from the
identical `ez_scene_selftest()` on the host:

```
ereader-zoom: page 4096x3072 tile 256 cells 35 z1=EA5CD9B4 pan=D5BD08D3
  z2=6C68D8BF lens=8B3C5733 hit=6680 miss=27 evict=0 warm=3 ok
```

| Number | What it proves |
|---|---|
| `z1` / `pan` / `z2` / `lens` | The four scripted viewport states each render to a fixed framebuffer, on host **and** in `ra8_emulator` **and** on silicon -- the whole path is integer. |
| `miss=27` | 27 tile decodes covers the whole four-state sequence: 1.7 MiB of a 12 MiB page. Only the visible tiles are ever resident. |
| `evict=0` | No tile still on screen was thrown away. That is the thrash #338 describes, and the cache is sized (`k_ez_cells`) to the viewport tile demand plus a pan margin so it cannot happen. |
| `warm=3` | The pan read-ahead warmed the lead-edge column out of the cache's spare capacity (#341). |

The hashes are toolchain-independent **because the chrome carries no text**. A
framebuffer hash over antialiased glyphs is toolchain-bound -- the same board
prints one value from a 13.3-built image and another from a 14.3-built one -- so
the status bar here is filled rectangles and a block zoom indicator.

## Memory

| Where | What | Bytes |
|---|---|---|
| SDRAM | RGB565 framebuffer | 1 228 800 |
| SDRAM | tile cache: 35 gray8 256x256 cells | 2 293 760 |
| SRAM | `ra8_zoom` composite scratch (row + strip + packed) | 25 600 |
| SRAM | tile-cache metadata (keys / dims / links / buckets) | ~800 |

The viewer's own footprint is the 25 KiB. It never holds the visible window --
1024x600 gray8 would be 600 KiB and cannot be allocated (NASA P10 Rule 3) -- so
it composites in 16-row strips and the cost is `O(dst.w * strip_rows)`, not
`O(dst.w * dst.h)`. The large working set is the tile cache, which is where a
reader's image budget already lives.

## Interaction

Discrete tap zones (the GT911 path reports contacts, not drags, so there is no
swipe):

| Zone | Action |
|---|---|
| status bar | open / close the loupe |
| inside the loupe | cycle the loupe magnification (4x -> 8x -> 4x) |
| content, left / right / top / bottom band (176 px) | pan the page by a viewport less a one-eighth overlap |
| content, centre | cycle the page magnification (1:1 -> 2x -> 4x -> 1:1) **about the tapped point** |

Zooming keeps the tapped point fixed under the finger. Panning is one
destination pixel of granularity at every magnification, because the viewport
anchor lives in the magnified image plane rather than in source pixels.

## e-ink behaviour

A pan moves every pixel in the viewport, so there is no smaller true dirty
region -- the viewport is flushed whole. What *is* exploited is the waveform:
an interactive burst flushes with the panel's A2 (bi-level, ~10x faster than
GC16) and `ra8_zoom_view_tick` promotes the view to a full 16-level GC16 repaint
once the gesture has been still for 350 ms. `k_ra8_zoom_policy_quality` opts out
and pays full GC16 per step for photographic content.

The loupe is where partial update genuinely pays: cycling its magnification
changes only the lens box, so `ez_scene_present` asks for a flush of
320x320 = 102 400 pixels instead of the content area's 565 248.

## Run it

```sh
make emu-ereader_zoom                       # live macOS panel window
make ereader_zoom && make flash             # on the board

# headless, with the scripted taps (zoom, then pan right, then open the loupe):
tools/ra8_emulator/build/ra8_emulator \
  examples/ek_ra8d2/hw_pending/ereader_zoom/build/ereader_zoom.elf \
  --panel tools/ra8_emulator/panels/ek_ra8d2.toml \
  --touch-seq "512:300,950:300,10:10" --ppm /tmp/zoom.ppm
```

The banner is asserted by the `emulator-smoke` gate (the app is listed in
`scripts/emu/smoke_apps.sh`), and the same numbers are asserted on the host by
`tests/test_app_ereader_zoom.c`.

## Status

`hw_pending`: emulator-viewable and CI-asserted, **not** yet silicon-validated
(no rig in this workflow). It is deliberately not under
`examples/ek_ra8d2/hw_validated/hil/` -- that tier's `hil.conf` is enforced by
the EIL==HIL parity gate, and claiming the tier without a bench run is exactly
what that gate exists to stop. The `hil.conf` ships now, so promotion after a
bench run is a `git mv` with nothing to write.
