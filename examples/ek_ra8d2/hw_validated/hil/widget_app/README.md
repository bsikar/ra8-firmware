# widget_app

Headless gate proving the `ra8_widget` compositor (#145) and the `ra8_app`
framework (#146) work end to end on the M85.

Two apps register into an `ra8_app` registry. Each is a widget tree -- a
fixed-height status bar over a flex content widget -- laid out by
`ra8_widget_layout_stack` (delegating to `ra8_box`) and drawn by each widget's
`render` callback through `ra8_gfx` into a small off-screen RGB565 framebuffer.
Launching the second app fires the focus lifecycle (`on_leave` then `on_enter`)
before compositing it. The banner hashes both composites, so the gate asserts
that both apps registered, that the two composites are distinct, that the
lifecycle fired exactly once each, and -- the #145 partial-flush acceptance --
that invalidating only the status bar yields damage of exactly the status-bar
rect with the `fast` (A2) hint, the minimal e-ink update.

The pure logic (layout, input routing, damage, registration, lifecycle) is
unit-tested on the host; this app runs the whole composition and lifecycle on
the target and CRC-gates the result, so any drift trips the gate. No panel,
SDRAM, touch or SD dependency -- `widget_app_demo` is the panel-visible sibling.

Re-expressing the `ereader_ui` monolith as a composition of apps is the
follow-on integration step; it waits on the `just apps::emulator::golden` baseline
settling, since that must stay byte-identical.
