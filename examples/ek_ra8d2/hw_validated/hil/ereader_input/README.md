# ereader_input

Exercises the e-reader interaction layer with synthetic taps, so it needs no
GT911 touch panel (#80, #118). It builds a representative target set -- a book
grid plus a toolbar button, each a rect bound to an action id -- injects taps at
every target's centre and at deliberate misses (the column gutter, off-screen),
and drives the `ra8_ui` screen stack through opening a book and going back,
asserting the root screen is never popped.

This catches what a render gate structurally cannot: a screen that draws
correctly but dispatches its taps to the wrong handler. It is pure logic with no
peripheral state, so the result is identical every boot and matches the host and
emulator runs.
