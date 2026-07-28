# ereader_table

Headless **on-silicon HIL gate** for minimal `<table>` layout (`ra8_reflow`,
#107) -- the tokenize table/tr/td/th -> equal-column grid -> per-cell flow ->
row stacking (with row page-breaks) pipeline. No panel / SD / touch needed.

1. Lay out a baked chapter through `ra8_reflow` (bundled **Ahem** face): a
   heading, a 2-column table (a `<th>` header row + data rows), and a trailing
   paragraph.
2. Fold an **FNV-1a-32** hash over every laid-out glyph's `(x, y)` -- the column
   positions and row baselines are encoded there.
3. Print a banner on the SCI8 J-Link OB console:

   ```
   ereader-table-hil: glyphs=172 geom=E3181EE6
   ```

The gate (`hil.conf`, `uart_scrape`) asserts that line. Any drift in the column
sizing, cell flow, or row stacking changes the hash.

Ahem has fixed glyph metrics, so the grid is deterministic: the banner is
identical on host, `ra8_emulator`, and silicon -- an emulator/silicon equivalence check.

## Build + run

```
make ereader_table
scripts/hil/run_local.sh ereader_table      # flash + scrape the banner
```

## Result (validated 2026-06-18, ra8_emulator + host)

```
ereader-table-hil: boot
ereader-table-hil: glyphs=172 geom=E3181EE6
```

`scripts/emu/smoke.sh ereader_table` PASS; the identical host layout
produces the same `geom=E3181EE6` -- byte-for-byte agreement.

## Updating the baseline

After an **intentional** change to the baked table or the grid math, recompute
the hash (run under `ra8_emulator` or on the bench) and update `HIL_EXPECT` in
`hil.conf`. The on-device banner is the source of truth.
