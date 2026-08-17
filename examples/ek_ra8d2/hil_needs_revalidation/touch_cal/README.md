# touch_cal

Interactive N-point affine touch calibration (#262), wiring the weighted
least-squares solver in `libs/ra8_touch_cal` end to end over the **real** GoodIX
GT911 driver (`ra8_touch` on IIC_B) and the **real** GLCDC display path
(`ra8_display_pal` on `ra8_glcdc`): present targets, collect raw samples, solve
the transform, apply it and verify.

For each of five targets -- four inset corners and the panel centre -- it paints
a white cross-hair and blocks on one settled raw GT911 sample. `tc_draw_target`
and `tc_read_raw` are the two dependency-inversion seams `ra8_touch_cal` is
written against, and they also record the target/raw pair so the verify pass can
measure the residual. On a successful solve it serialises the matrix, loads it
back, re-applies it to every captured raw and paints a green corrected
cross-hair at each mapped pixel -- a live cross-hair landing on its target is
the visible proof. The largest residual is the fit error.

**The panel framebuffer is 512x512, not 1024x600.** The full panel buffer does
not fit in on-chip SRAM, so the app scans a 512x512 RGB565 framebuffer, the same
size `glcdc_render` uses. That is a hardware constraint, not a simplification.

## The bring-up sentinel is finger-free on purpose

The solve is only reachable once five distinct raw samples arrive, which on
silicon means a human tapping five cross-hairs. So the deterministic part of the
verdict is a separate ready line that prints once the panel and touch driver are
up, whether or not any tap arrived; a solver regression still trips the gate
through the explicit failure verdicts. With no finger the app reports a skipped
calibration and the ready line still holds, so the same gate is meaningful on a
staffed bench and on an unattended one.

On the bench the GT911 lives on the ereader carrier's I2C0 bus and the TFT on
the parallel RGB interface. Tap each cross-hair as it appears; the app solves
the transform, stores it ready for `ra8_flash` page storage, and paints the
corrected cross-hairs so you can see the correction land.
