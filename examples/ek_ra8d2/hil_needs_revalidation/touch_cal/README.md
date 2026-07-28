# touch_cal

Interactive **N-point affine touch calibration** demo + **HIL gate**
(`ra8_touch_cal`, #262). Wires the weighted-least-squares affine solver in
`libs/ra8_touch_cal` end-to-end over the **real** GoodIX GT911 touch driver
(`ra8_touch` -> IIC_B) and the **real** GLCDC layer-1 display path
(`ra8_display_pal` -> `ra8_glcdc`) -- present targets, collect raw touch
samples, solve the transform, apply + verify.

## What it does

1. Brings up clocks / MSTP / the SCI8 J-Link OB console.
2. Brings up the GLCDC panel through the display PAL, scanning a `512x512`
   RGB565 SRAM framebuffer (the same size `glcdc_render` uses -- the full
   1024x600 panel buffer does not fit on-chip). A failure prints
   `touchcal: FAIL glcdc`.
3. Brings up the GT911 over IIC_B channel 0, exactly as `touch_demo`. A failure
   prints `touchcal: FAIL open`.
4. Drives `ra8_touch_cal_run`: for each of the five built-in targets (four
   inset corners + the panel centre) it paints a white cross-hair and blocks on
   one settled raw GT911 sample. The two shims are the SOLID-D seams
   `ra8_touch_cal` inverts onto -- `tc_draw_target` renders into the
   framebuffer, `tc_read_raw` polls the real `ra8_touch_read`. Each shim also
   records the target / raw pair so the verify pass can measure the residual.
5. On a successful solve, serialises the matrix with `ra8_touch_cal_save`,
   round-trips it back with `ra8_touch_cal_load`, re-applies it to every
   captured raw with `ra8_touch_cal_apply`, and paints a **green corrected
   cross-hair** at each mapped pixel -- the live cross-hair proving corrected
   coordinates (it lands on its target). The largest residual is the fit error:

   ```
   touchcal: cal=OK verify=OK maxerr=0
   touchcal: ready dim=512x512
   ```

The `touchcal: ready` line is the deterministic, **finger-free** bring-up
sentinel -- it prints once the panel + touch driver are up, whether or not any
tap arrived. `hil.conf` asserts that substring and lists `verify=FAIL` /
`cal=FAIL` in its negative set, so a solver regression still trips the gate.

## Build + run

```
make touch_cal
scripts/hil/run_local.sh touch_cal      # flash + scrape the bring-up sentinel
```

## EIL==HIL

The calibration solve is only reachable once **five distinct raw samples**
arrive. On silicon a human taps the five cross-hairs. `ra8_emulator` reproduces
that by feeding five synthetic raw points through the modelled GT911
(`--touch-seq "x0:y0,..."`, added for this example), which return through the
genuine `ra8_touch_read` decode -- so under EIL the banner carries a **real**
solved + verified result with no board attached:

```
$ ra8_emulator touch_cal.elf --touch-seq 420:520,3868:520,3868:3968,420:3968,2148:2248
[uart] SCI8: touchcal: boot
[uart] SCI8: touchcal: target 40,40
[uart] SCI8: touchcal: target 471,40
[uart] SCI8: touchcal: target 471,471
[uart] SCI8: touchcal: target 40,471
[uart] SCI8: touchcal: target 256,256
[uart] SCI8: touchcal: cal=OK verify=OK maxerr=0
[uart] SCI8: touchcal: ready dim=512x512
  I3C/I2C GT911 : 5 touch frame(s) drained via ra8_touch -> I3C
```

The five raw points are the five targets pushed through a synthetic panel
transform (`raw = screen*8 + offset`), so the fit recovers the inverse exactly
and every corrected coordinate lands on its target (`maxerr=0`). `hil.conf`
declares that `--touch-seq` as `HIL_EMU_ARGS`, so `scripts/emu/eil_all.sh` runs the
whole calibration headless with **0 skips** and `check_hil_eil_parity.py` keeps
the app SIM-visible. On a bare automated bench with no finger the read shim
times out and the app reports `cal=SKIP got=0`, but `touchcal: ready` still
holds -- so the gate passes in every environment.

`tests/test_app_touch_cal.c` covers the app's decision logic (bounds guard,
usable-sample gate, pass verdict) with MC/DC and runs the same end-to-end solve
over the exact `--touch-seq` points on the host, asserting a zero residual.

## On real silicon

On the bench the GT911 lives on the ereader carrier's I2C0 bus and the TFT on
the parallel RGB interface. Tap each of the five cross-hairs as they appear;
the app solves the transform, stores it (ready for `ra8_flash` page storage),
and paints the green corrected cross-hairs so you can see the correction land.
Without a finger the bring-up sentinel (`touchcal: ready`) still passes.
