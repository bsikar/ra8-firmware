# widget_kit_demo

Concrete `ra8_widget` **leaf widgets** -- a text label and a push button --
composited through the `ra8_widget_panel` compositor on the live GLCDC panel.
Where `widget_compose_demo` proved the panel compositor with anonymous tile
leaves, this app drops the real reusable leaves onto it:

- `ra8_widget_label_t` -- a background fill plus an aligned string.
- `ra8_widget_button_t` -- a bordered, filled face and label that **latches on a
  tap** and reports the press through its `on_input` callback.

Both draw through an injected `ra8_widget_paint_t` backend wired to `ra8_gfx`,
so the `ra8_widget` library itself stays graphics-free and the same widgets run
under the host unit tests against a recording mock paint.

```
root (column panel)
|- title  (LABEL,  fixed)   centred heading
|- body   (ROW PANEL)       a nested panel ...
|   |- button A (BUTTON, flex)   "Prev"
|   |- button B (BUTTON, flex)   "Next"
|- footer (LABEL,  fixed)   a left-aligned hint line
```

The deterministic self-check walks a full compose (every root child dirty, whole
frame flushed with the `quality` hint, all leaves painting to a stable CRC), then
a synthetic touch routed through the root panel onto button A, which latches and
-- from its `on_press` callback -- invalidates only the body band, then a partial
compose that dirties exactly that one child, damages just the body rect with the
`fast` hint, repaints both buttons, and changes the CRC because button A now
shows its pressed face. That is the issue #145 partial-flush acceptance driven by
a concrete button. After the banner the loop alternately taps A and B and
partial-composes only the body band, so the panel visibly toggles a button.

The RGB565 framebuffer lives in SRAM and composites top-left over the GLCDC
background plane; the full 1024x600 panel needs SDRAM, a separate task. The
touches are synthesised in firmware, so it runs on a stock EK-RA8D2 with no
extra hardware.
