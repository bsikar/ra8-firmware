# widget_chrome_demo

The concrete ereader **chrome widgets** -- extracted into the `ra8_widget`
library for [issue #145](https://github.com/bsikar/ra8-firmware/issues/145)
Phase 2 -- composited on the live GLCDC panel.

Where `widget_compose_demo` proves the `ra8_widget_panel` compositor primitive
with generic tiles, this app shows the **reusable leaves** the ereader chrome is
now built from, each painting through the injected `ra8_widget_paint_t` backend
(bound here to `ra8_gfx`; the host unit tests bind the same seam to a recording
mock, so a widget runs byte-for-byte the same logic on the host and on the M85).

## The tree (dwm-style: every band is a reusable widget)

```
root (column panel)
|- status bar   (ra8_widget_status_bar_t,   fixed)  title + live clock + rule
|- toolbar      (ra8_widget_toolbar_t,       fixed)  search field + count chip
|- book grid    (ra8_widget_book_grid_t,     flex)   2-column cover / title / bar
|- progress bar (ra8_widget_progress_bar_t,  fixed)  overall read gauge
|- nav strip    (ra8_widget_nav_bar_t,       fixed)  Library / Store / Settings
```

One `ra8_widget_panel_compose()` call lays the bands out, computes the minimal
damage rectangle + refresh hint, and renders only the dirty children.

## What it proves (deterministic self-check)

- **Full compose** -- mark the whole tree dirty: all 5 chrome bands dirty,
  damage covers the full 512x512 frame, `quality` hint.
- **Partial compose** -- advance the status-bar clock and invalidate **only**
  the status bar: exactly 1 dirty child, damage is just the `512x44` status rect
  with the `fast` hint, and the composite CRC changes. This is the Phase-1
  partial-flush acceptance, now driven by a real chrome widget.

It then prints a single banner so the app doubles as a `board_sim` gate:

```
widget-chrome-demo: dirty0=5 crc0=5D5C1F5E dirty1=1 crc1=200D1857 flush=512x44 hint=fast PASS
```

After the banner the loop advances the clock and partial-composes the status bar
live, so the panel updates only its top band (the damage-tracked A2 path).

## Build / run

```
make widget_chrome_demo                              # cross-build the .elf
tools/ra8_emulator/build/ra8_emulator \
  examples/ek_ra8d2/hw_validated/hil/widget_chrome_demo/build/widget_chrome_demo.elf
```

The CRCs hash the software-rasterised framebuffer (including the rendered text),
so any change to the chrome widgets' geometry or strings re-mints the golden in
`hil.conf`.

## Scope

This is Phase 2: reusable widgets exercised on-panel. It does **not** replace the
`ereader_ui` chrome -- that is Phase 3, and stays byte-identical under
`make ereader-golden` until then.
