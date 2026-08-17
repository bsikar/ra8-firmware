# ereader_table

Lays a baked chapter out through `ra8_reflow` with a heading, a two-column table
(a `<th>` header row plus data rows) and a trailing paragraph, then folds an
FNV-1a-32 hash over every laid-out glyph's `(x, y)` (#107). The column positions
and row baselines live there, so drift in the column sizing, the per-cell flow
or the row stacking -- including row page-breaks -- changes the hash. Headless
-- no panel, SD or touch.

The bundled Ahem face has fixed glyph metrics, so the grid is deterministic and
the same hash appears on host, under the emulator and on silicon.
