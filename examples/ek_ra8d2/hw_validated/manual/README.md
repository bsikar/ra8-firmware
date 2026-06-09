# examples/ek_ra8d2/hw_validated/manual/

Apps here are hardware-confirmed but cannot be tested automatically by HIL
CI because they require either:
- A physical interaction (button press) that CI cannot trigger, or
- A peripheral not present on the HIL bench (I2C controller, RTT viewer), or
- Visual confirmation on the 7-inch LCD that no programmatic check can
  substitute for.

HIL CI builds these but skips the run/verify step.  Manual sign-off is
required before promoting an app out of this directory.

To build: `make <appname>` from the repo root.

## Apps and blocking reason

| App | Why CI cannot auto-verify |
|-----|--------------------------|
| display_pal_animation | LCD render output -- requires human visual confirmation of animated frames |
| lcd_color_cycle | LCD render output -- requires human visual confirmation of color cycling |
| lcd_draw_x | LCD render output -- requires human visual confirmation of drawn 'X' |

All other manual-category apps were relocated to `hw_pending/` on 2026-05-19
because they had not yet been hardware-validated by the author. Their
HIL-ability assessments live in `hw_pending/<app>/README.md`.
