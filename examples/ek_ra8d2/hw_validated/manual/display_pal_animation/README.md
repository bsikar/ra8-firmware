# display_pal_animation

Reference example for `libs/ra8_display_pal`. Paints six horizontal RGB565
colour bars that scroll vertically once per frame, using only the PAL's
public API -- no direct call to `ra8_glcdc_*`.

## What this demo covers

- `display_init` -- bring up the bound backend (LCD here).
- `display_get_caps` -- query backend properties (refresh latency,
  continuous-vs-bistable panel, partial-update support).
- `display_get_framebuffer` -- get the buffer the backend is targeting.
- `display_clear` -- whole-screen pre-paint clear.
- `display_flush` -- commit framebuffer changes with a refresh hint.
- `display_full_rect` -- convenience for whole-screen flush rects.
- Caps-driven branching: picks `k_display_refresh_fast` on continuous-
  refresh panels (LCD) and `k_display_refresh_quality` on bistable ones
  (e-ink). The app is the same; only the choice depends on the bound
  backend.

The blue board LED toggles once per frame as a liveness signal; the red
LED comes on if any PAL call fails.

## Swap to the e-ink backend

Open `main.c` and change the `iface` line of `k_app_display_cfg`:

```c
static const display_cfg_t k_app_display_cfg = {
    .iface             = &k_display_backend_eink_it8951,   /* was: lcd_ra8_glcdc */
    .framebuffer       = s_framebuffer,
    .framebuffer_bytes = sizeof(s_framebuffer),
    .width_px          = (uint16_t)k_app_fb_w,
    .height_px         = (uint16_t)k_app_fb_h,
    .pixfmt            = k_display_pixfmt_rgb565,
};
```

That is the entire change. Today the e-ink backend is a stub:
`display_init` and `display_get_caps` succeed, but `display_flush` /
`display_get_framebuffer` return `k_ra8_err_not_supported`. The PAL will
report the not_supported back to `main` -- the red LED will come on --
which lets you exercise the link/wiring before the IT8951 driver lands.

## Build + flash

```
cd examples/ek_ra8d2/hw_validated/manual/display_pal_animation
make           # cross-compile build/display_pal_animation.elf / .hex
make flash     # JLinkExe via scripts/dev/flash.sh (HIL: bash scripts/hil/flash.sh display_pal_animation)
make ozone     # SEGGER Ozone GUI debug
```
