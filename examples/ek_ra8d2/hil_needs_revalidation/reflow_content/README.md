# reflow_content

Headless check of the `ra8_reflow` book-content render path (#115): lay out a
baked multi-paragraph chapter into a small RGB565 framebuffer, render every page
and fold a hash over the framebuffer output, then re-flow the cached chapter at
a larger font size and render every page again. No panel, no card, no touch.

The re-flow half is the interesting assertion: a larger size must paginate to
more pages, so a broken re-flow shows up as a page count that did not move, and
any drift in layout, pagination or glyph rendering changes a hash.

It uses the bundled **Ahem** face, whose glyph metrics are fixed by design. That
makes pagination and rendering fully deterministic, so the same hashes come out
on the host, in an emulator and on silicon, and they are stable across fresh
resets -- which turns this into an equivalence check between those three
environments rather than a rendering demo.

After an *intentional* change to the baked chapter or to the layout and render
math, the expectation in `hil.conf` has to be recomputed from a real run. The
on-device output is the source of truth for that baseline, never a hand-edited
value.
