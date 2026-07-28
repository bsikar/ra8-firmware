# widget_compose_demo

Nested `ra8_widget` tree composited on the live GLCDC panel -- the focused
demonstration of the `ra8_widget_panel` compositor primitive added for
[issue #145](https://github.com/bsikar/ra8-firmware/issues/145).

Where `widget_app_demo` shows the full `ra8_widget` + `ra8_app` launcher, this
app isolates the new structural piece: **a container that is itself a widget**,
which is what turns the flat widget array into a *tree*.

## The tree (dwm-style: every piece opt-in)

```
root (column panel)
|- status   (leaf, fixed)   title + a live frame counter
|- body     (ROW PANEL)     a nested panel ...
|   |- left  tile (leaf, flex)
|   |- right tile (leaf, flex)
|- footer   (leaf, fixed)   a hint line
```

`body` is the new primitive in action: a `ra8_widget_panel` nested inside the
root `ra8_widget_panel`. One `ra8_widget_panel_compose()` call lays the children
out, computes the minimal damage rectangle + refresh hint, and renders only the
dirty children. A dirty nested panel repaints its whole subtree.

## What it proves (deterministic self-check)

- **Full compose** -- mark the whole tree dirty: 3 dirty root children, damage
  covers the full 512x512 frame, `quality` hint, and the nested `body` panel
  composites both tiles (per-tile render counters confirm it).
- **Partial compose** -- bump the frame counter and invalidate **only** the
  status bar: exactly 1 dirty child, damage is just the `512x44` status rect
  with the `fast` hint, the tiles are **not** re-rendered, and the composite CRC
  changes. This is the acceptance bullet -- a status-only change flushes only
  the status rect.

It then prints a single banner so the app doubles as a `ra8_emulator` gate:

```
widget-compose-demo: dirty0=3 crc0=<8hex> dirty1=1 crc1=<8hex> flush=512x44 hint=fast PASS
```

After the banner the loop advances the frame counter and partial-composes only
the status band, so the panel visibly updates its top band live.

## Build / run

```
make widget_compose_demo                       # from the repo root
make -C examples/ek_ra8d2/hw_pending/widget_compose_demo   # standalone

# headless render in the host emulator:
tools/ra8_emulator/build/ra8_emulator \
    examples/ek_ra8d2/hw_pending/widget_compose_demo/build/widget_compose_demo.elf \
    --ppm /tmp/widget_compose.ppm --record-secs 2
```

The 512x512 RGB565 framebuffer lives in SRAM (the full 1024x600 panel needs
SDRAM, a separate task) and composites top-left over the GLCDC background plane.
No SDRAM / touch / SD / buttons -- it is a pure compositor demo.
