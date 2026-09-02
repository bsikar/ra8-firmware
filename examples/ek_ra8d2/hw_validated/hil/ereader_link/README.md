# ereader_link

Exercises in-content hyperlink navigation end to end (#110). It lays a baked
chapter out through `reflow` carrying one cross-chapter `<a href>`, one
`#fragment` link and the matching `id` anchor; synthesises a tap at the centre
of every laid-out link rectangle and resolves it with
`reflow_hit_test_link()` plus `reflow_href_split()`, so one classifies as
a cross-chapter target and one as a same-chapter fragment; then resolves the
fragment to its page with `reflow_find_anchor()`. An FNV-1a-32 hash over the
link-rectangle geometry pins the layout half, so drift in the href capture, the
link-rect math or the resolve logic trips the gate. Headless -- no panel, SD or
touch.

The bundled Ahem face has fixed glyph metrics, so the layout and the link
geometry are deterministic and the result is identical on host, under the
emulator and on silicon.
