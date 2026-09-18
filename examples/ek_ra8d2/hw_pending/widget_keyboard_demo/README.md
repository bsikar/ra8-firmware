# widget_keyboard_demo

First app that links `ra8_widget_keyboard`.

`ra8_widget` publishes an on-screen-keyboard leaf widget: it draws a key grid
through the injected `ra8_widget_paint_t` backend and routes a tap through the
injected `ra8_widget_keyboard_ops_t` seam (`count` / `key_info` / `hit` /
`apply`). Until this app the widget was named only by its own header, its own
translation unit, and one host test whose seam is a recording mock, so both
sides of the pairing were fakes and nothing checked the widget against the real
`ra8_keyboard` engine (issue #1336). The one screen in the tree that shows a
keyboard, `ereader_ui`, drives `ra8_keyboard` directly and hand-rolls its own
key chrome, bypassing the widget entirely.

## What it proves

1. `ra8_widget_keyboard_init` installs the published vtable
   (`w.vt == ra8_widget_keyboard_vtable()`), the ctx and the visibility.
2. One `ra8_widget_panel_compose` lays the keyboard out inside the frame,
   reports a full-frame quality damage rect, and paints a background fill plus
   one face per key through the paint seam.
3. Taps on the `r` and `a` keys, routed by `ra8_widget_dispatch` through the
   root panel, type into the *engine's* buffer: `s_text.buf == "ra"`.
4. The one-shot SHIFT lands in the engine, not just in the widget: SHIFT then
   `b` appends `'B'` and clears SHIFT; backspace then removes it again.
5. A tap outside the frame is consumed by nobody and leaves the buffer as it
   was, so a miss cannot pass as a keystroke.
6. RETURN sets `committed`, fires `on_commit` exactly once, and the widget's
   self-invalidate makes the next compose report exactly the keyboard's own
   rect as damage.

## Why the paint backend records instead of drawing

The gap this app closes is the widget-to-engine pairing, not the pixels. A
recording `ra8_widget_paint_t` (it counts `fill_rect` / `draw_text` calls and
touches no framebuffer) keeps every leg deterministic and board-independent
while the widget and the key engine under test are both the real ones. The
pixel half is already exercised on-target by `widget_kit_demo`, which binds the
same seam to `ra8_gfx`.

## Run

Console is SCI8 over the J-Link OB VCOM at 115200.

```
just build widget_keyboard_demo
just emu widget_keyboard_demo
```

It lives under `hw_pending` because it has not been captured on the bench yet.
Nothing here needs a peripheral beyond the console, so `ra8_emulator` runs it as
is.
