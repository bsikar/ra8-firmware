# reflow_content

Headless **on-silicon HIL gate** for the `ra8_reflow` book-content render path
(#115) -- multi-page pagination, per-page render correctness, and a font-size
re-flow. No panel / SD / touch needed.

1. Lay out a baked multi-paragraph chapter through `ra8_reflow` (bundled **Ahem**
   face) into a 160x192 RGB565 framebuffer.
2. Render **every page** and fold an **FNV-1a-32** over the framebuffer output.
3. Call `ra8_reflow_set_font_size()` to re-flow the cached chapter at a larger
   size (16 -> 24 px), then render every page again.
4. Print a banner on the SCI8 J-Link OB console:

   ```
   reflow-content-hil: pages=14 crc=D211DBC5 rpages=33 crc=62C68DC5
   ```

The gate (`hil.conf`, `uart_scrape`) asserts that line. The larger re-flow
paginates to more pages (`rpages` > `pages`), exercising the re-flow path; any
drift in the layout, pagination, or glyph render changes a hash.

Ahem has fixed glyph metrics, so pagination + render are deterministic: the
banner is identical on host, `ra8_emulator`, and silicon, and stable across fresh
resets -- an emulator/silicon equivalence check.

## Build + run

```
make reflow_content
scripts/hil/run_local.sh reflow_content      # flash + scrape the banner
```

## Result (validated 2026-06-18, ra8_emulator + host)

```
reflow-content-hil: boot
reflow-content-hil: pages=14 crc=D211DBC5 rpages=33 crc=62C68DC5
```

`scripts/emu/smoke.sh reflow_content` PASS; the identical host run
produces the same hashes -- byte-for-byte agreement.

## Updating the baseline

After an **intentional** change to the baked chapter or the layout/render math,
recompute the banner (run under `ra8_emulator` or on the bench) and update
`HIL_EXPECT` in `hil.conf`. The on-device banner is the source of truth.
