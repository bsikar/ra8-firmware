# widget_app

Headless HIL gate proving the **`ra8_widget` compositor (#145) + `ra8_app`
framework (#146)** work end to end on the M85.

## What it does

1. **Two apps** (`library`, `reader`) register into an `ra8_app` registry
   (each `init` runs once).
2. **Each app is a widget tree**: a status-bar widget (fixed 16 px) over a
   content widget (flex), laid out by `ra8_widget_layout_stack` (delegating to
   `ra8_box`) and drawn by each widget's `render` callback through `ra8_gfx` into
   a 160x120 RGB565 framebuffer.
3. `ra8_app_launch(library)` -> `ra8_app_render` composites the library tree;
   FNV-1a hash -> `lib`.
4. `ra8_app_launch(reader)` fires `library.on_leave` + `reader.on_enter` (the
   focus lifecycle), then composites the reader tree -> `rdr`. The two CRCs
   differ (different content widget).
5. **Partial flush (#145):** invalidating only the status bar with the fast
   hint -> `ra8_widget_damage` returns just the status-bar rect (`160x16`) and
   the `fast` (A2) hint -- the minimal e-ink update.

On success:

```
widget-app-hil: apps=2 lib=D3FB85C5 rdr=E9E475C5 flush=160x16 hint=fast PASS
```

The banner asserts: both apps registered, the two app composites are distinct,
the focus lifecycle fired exactly once each, and the damage of a status-bar-only
change is exactly the status-bar rect with the fast hint. No panel / SDRAM /
touch / SD dependency.

## Why this matters

`ra8_widget` and `ra8_app` are the foundation issues #145/#146 ask for -- a
zero-heap (NASA Rule 3) composable widget layer and an app lifecycle/registry,
both built on the existing `ra8_box` (layout) + `ra8_ui` (hit-test/nav)
primitives. Their pure logic (layout, input routing, damage, registration,
lifecycle) is unit-tested on the host (`tests/test_ra8_widget.c`,
`tests/test_ra8_app.c`); this app runs the **whole composition + lifecycle** on
the target and CRC-gates the composited framebuffer, so any drift trips the gate.

## Scope note

This proves the foundation. Re-expressing the 2000-line `ereader_ui` monolith as
a `library` + `epub_reader` + `settings` app composition (the remaining
acceptance bullet of both issues) is the integration step that follows -- it is
golden-gated (`make ereader-golden` must stay byte-identical) and best done once
that golden baseline (currently in flux from the board_sim GUI work) settles.

## Validation

Run on `tools/ra8_emulator` (the firmware boots, registers + launches + composites
the apps, no fault):

```
[uart] SCI8: widget-app-hil: boot
[uart] SCI8: widget-app-hil: apps=2 lib=D3FB85C5 rdr=E9E475C5 flush=160x16 hint=fast PASS
```

## Build

```
make widget_app
make -C examples/ek_ra8d2/hw_validated/hil/widget_app flash
```
