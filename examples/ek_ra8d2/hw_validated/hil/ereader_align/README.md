# ereader_align

Lays a baked chapter out through `ra8_reflow` with one paragraph each of
`text-align` right, center, justify and the default left, then folds an
FNV-1a-32 hash over every laid-out glyph's `(x, y)` (#108). The alignment
offsets and the justification slack live in those x positions, so drift in the
centre/right shift, the justify distribution or last-line handling changes the
hash. Headless -- no panel, SD or touch.

The bundled Ahem face has fixed glyph metrics, so the layout is deterministic
and the same hash appears on host, under the emulator and on silicon. That makes
the gate an emulator/silicon equivalence check as well as a layout regression
net.
