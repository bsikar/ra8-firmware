# widget_kit_demo

Concrete `ra8_widget` **leaf widgets** -- a text label and a push button --
composited through the `ra8_widget_panel` compositor on the live GLCDC panel.
The Phase-2 follow-up to `widget_compose_demo` for
[issue #145](https://github.com/bsikar/ra8-firmware/issues/145).

Where `widget_compose_demo` proved the panel compositor with anonymous tile
leaves, this app drops the **real, reusable leaf widgets** onto it:

- `ra8_widget_label_t` -- a background fill plus an aligned string.
- `ra8_widget_button_t` -- a bordered, filled face + label that **latches on a
  tap** and reports the press through its `on_input` callback.

Both draw through an injected `ra8_widget_paint_t` backend wired to `ra8_gfx`, so
the `ra8_widget` library itself stays graphics free (the same widgets run under
the host unit tests with a recording mock paint).

## The tree (dwm-style: every piece opt-in)

```
root (column panel)
|- title  (LABEL,  fixed)   centred heading
|- body   (ROW PANEL)       a nested panel ...
|   |- button A (BUTTON, flex)   "Prev"
|   |- button B (BUTTON, flex)   "Next"
|- footer (LABEL,  fixed)   a left-aligned hint line
```

One `ra8_widget_panel_compose()` call lays the children out, computes the minimal
damage rectangle + refresh hint, and renders only the dirty children. A dirty
nested panel repaints its whole subtree.

## What it proves (deterministic self-check)

- **Full compose** -- mark the whole tree dirty: 3 dirty root children, damage
  covers the full 512x512 frame, `quality` hint; the label and button leaves all
  paint (a stable composite CRC).
- **Press** -- a synthetic touch routed through the root panel lands on button A,
  which latches (`presses == 1`, `pressed == true`) and, via its `on_press`
  callback, invalidates only the body band.
- **Partial compose** -- exactly 1 dirty root child (the body), damage is just
  the `512x440` body rect with the `fast` hint, both buttons repaint, and the
  composite CRC changes (button A now shows its pressed face). This is the
  issue #145 partial-flush acceptance, driven by the concrete button widget.

It then prints a single banner so the app doubles as a `board_sim` gate:

```
widget-kit-demo: dirty0=3 crc0=<8hex> press=1 dirty1=1 crc1=<8hex> flush=512x440 hint=fast PASS
```

After the banner the loop alternately taps button A / button B and
partial-composes only the body band, so the panel visibly toggles a button live.

## Build / run

```
make widget_kit_demo                                    # from the repo root
make -C examples/ek_ra8d2/hw_pending/widget_kit_demo    # standalone

# headless render in the host emulator:
tools/ra8_emulator/build/ra8_emulator \
    examples/ek_ra8d2/hw_pending/widget_kit_demo/build/widget_kit_demo.elf \
    --ppm /tmp/widget_kit.ppm
```

The 512x512 RGB565 framebuffer lives in SRAM (the full 1024x600 panel needs
SDRAM, a separate task) and composites top-left over the GLCDC background plane.
No SDRAM / SD card -- the touches are synthesised in firmware, so it runs on a
stock EK-RA8D2 with no extra hardware.
