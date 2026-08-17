# widget_chrome_demo

The concrete ereader **chrome widgets** -- extracted into the `ra8_widget`
library for issue #145 -- composited on the live GLCDC panel. Where
`widget_compose_demo` proves the `ra8_widget_panel` compositor primitive with
anonymous tiles, this app shows the reusable leaves the ereader chrome is now
built from: a status bar with a live clock, a toolbar carrying a search field
and a count chip, a two-column book grid, an overall progress bar, and a nav
strip, stacked as bands in one column panel.

Each leaf paints through the injected `ra8_widget_paint_t` backend -- bound here
to `ra8_gfx`, and to a recording mock in the host unit tests -- so a widget runs
byte-for-byte the same logic on the host and on the M85.

A single `ra8_widget_panel_compose()` lays the bands out, computes the minimal
damage rectangle and refresh hint, and renders only the dirty children. The
deterministic self-check pins both ends of that: a full compose dirties every
band and flushes the whole frame with the `quality` hint, while advancing the
clock and invalidating only the status bar dirties exactly one child and yields
just the status rect with the `fast` hint. That is the Phase-1 partial-flush
acceptance, now driven by a real chrome widget. After the banner the loop keeps
advancing the clock and partial-composing, so the panel updates only its top
band live.

The gate CRCs hash the software-rasterised framebuffer including the rendered
text, so any change to chrome-widget geometry *or* to the strings re-mints the
goldens in `hil.conf`.

This does not replace the `ereader_ui` chrome; that swap is a later phase and
stays byte-identical under `make ereader-golden` until then.
