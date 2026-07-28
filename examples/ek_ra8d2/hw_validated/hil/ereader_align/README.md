# ereader_align

Headless **on-silicon HIL gate** for text alignment + justification
(`ra8_reflow`, #108) -- the `style="text-align:..."` -> per-line shift / justify
pipeline. No panel / SD / touch needed.

1. Lay out a baked chapter through `ra8_reflow` (bundled **Ahem** face) with one
   paragraph each of `text-align:right`, `:center`, `:justify`, and the default
   left.
2. Fold an **FNV-1a-32** hash over every laid-out glyph's `(x, y)` -- the
   alignment offsets and the justification slack are encoded in those x
   positions.
3. Print a banner on the SCI8 J-Link OB console:

   ```
   ereader-align-hil: glyphs=210 geom=D4C9657E
   ```

The gate (`hil.conf`, `uart_scrape`) asserts that line. Any drift in the
alignment math (centre/right shift, justify distribution, last-line handling)
changes the hash.

Ahem has fixed glyph metrics, so the layout is deterministic: the banner is
identical on host, `ra8_emulator`, and silicon -- an emulator/silicon equivalence check.

## Build + run

```
make ereader_align
scripts/hil/run_local.sh ereader_align      # flash + scrape the banner
```

## Result (validated 2026-06-18, ra8_emulator + host)

```
ereader-align-hil: boot
ereader-align-hil: glyphs=210 geom=D4C9657E
```

`scripts/emu/smoke.sh ereader_align` PASS; the identical host layout
produces the same `geom=D4C9657E` -- byte-for-byte agreement.

## Updating the baseline

After an **intentional** change to the baked chapter or the alignment math,
recompute the hash (run under `ra8_emulator` or on the bench) and update
`HIL_EXPECT` in `hil.conf`. The on-device banner is the source of truth.
