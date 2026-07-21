# ereader_input

Headless **on-silicon HIL gate** for the e-reader interaction layer (`ra8_ui`
hit-test + screen navigation, #80/#118). The chrome render is golden-validated
(`ereader_chrome`); this app closes the *input-routing* gap with **synthetic**
input -- no GT911 touch needed.

1. Build a representative chrome target set (a 2x2 library book grid + a status
   bar toolbar button), each an `ra8_ui_target_t` rect bound to an action id.
2. Inject a sequence of taps at known coordinates: each target's centre (a hit
   on the matching action) plus off-target taps (the column gutter + off-screen,
   which must miss).
3. Drive the screen stack: open a book (push reading), back (pop to library),
   and verify the root is never popped.
4. Print the result on the SCI8 J-Link OB console:

   ```
   ui-hil: taps=7 hits=5 nav_ok=1 PASS
   ```

The gate (`hil.conf`, `uart_scrape`) asserts that line. A render-correct screen
whose taps dispatched to the wrong handler -- invisible to the chrome render
gate -- fails here.

Pure logic (no peripheral state), so the banner is identical every boot and
matches the host / board_sim run.

## Build + run

```
make ereader_input
scripts/hil/run_local.sh ereader_input      # flash + scrape the banner
```

## Result (validated 2026-06-18, board_sim + host)

```
ui-hil: boot
ui-hil: taps=7 hits=5 nav_ok=1 PASS
```

`scripts/sim/smoke.sh ereader_input` PASS.
