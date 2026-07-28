# ereader_link

Headless **on-silicon HIL gate** for in-content hyperlink navigation
(`ra8_reflow` links + anchors, #110) -- the tokenize-`href`/`id` ->
layout-link-rects -> hit-test -> resolve -> anchor pipeline the e-reader uses to
follow `<a>` taps. No panel / SD / touch needed.

1. Lay out a baked chapter through `ra8_reflow` (bundled **Ahem** face): two
   `<a href>` links -- one cross-chapter (`ch2.xhtml`), one `#fragment`
   (`#foot`) -- and a `<p id="foot">` anchor.
2. **Synthesise a tap** at the centre of every laid-out link rectangle and
   resolve it with `ra8_reflow_hit_test_link()` + `ra8_reflow_href_split()`: one
   classifies as a cross-chapter target, one as a same-chapter fragment.
3. Resolve the `#foot` fragment to its page with `ra8_reflow_find_anchor()`.
4. Fold an **FNV-1a-32** hash over the laid-out link-rectangle geometry and
   print a banner on the SCI8 J-Link OB console:

   ```
   ereader-link-hil: links=2 cross=Y frag=Y apage=1 geom=5B90D1EE
   ```

The gate (`hil.conf`, `uart_scrape`) asserts that line. Any drift in the href
capture, the link-rect math, or the resolve logic changes it.

Ahem has fixed glyph metrics, so the layout + link geometry are deterministic:
the banner is identical on host, `ra8_emulator`, and silicon -- an emulator/silicon
equivalence check.

## Build + run

```
make ereader_link
scripts/hil/run_local.sh ereader_link      # flash + scrape the banner
```

## Result (validated 2026-06-18, ra8_emulator + host)

```
ereader-link-hil: boot
ereader-link-hil: links=2 cross=Y frag=Y apage=1 geom=5B90D1EE
```

`scripts/emu/smoke.sh ereader_link` runs the firmware ELF on the
emulated RA8D2 and scrapes the banner (PASS). The identical layout + nav run on
host produces the same `geom=5B90D1EE` -- byte-for-byte agreement.

## Updating the baseline

After an **intentional** change to the baked chapter or the link math, recompute
the hash (run under `ra8_emulator` or on the bench) and update `HIL_EXPECT` in
`hil.conf`. The on-device banner is the source of truth.
