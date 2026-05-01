# threadx_guix_demo

ThreadX + GUIX hello-world demo for the EK-RA8D2.

## What this app does

1. Configures LED1 as a "ThreadX is alive" heartbeat indicator.
2. Allocates a 256 x 128 RGB565 framebuffer in on-chip SRAM.
3. Drops into the ThreadX scheduler, which spawns two threads:
   - `guix` (priority 5) -- runs `gx_system_initialize`, builds a tiny
     widget tree (root window + label + button), and enters
     `gx_system_start`.
   - `app`  (priority 6) -- every 750 ms toggles the GUIX `TEXT`
     colour resource and triggers a canvas refresh, animating the
     label between blue and orange.
4. The label reads `Hello from GUIX on RA8D2`; the button shows the
     default raised border with no text (no string-table resource is
     loaded by this minimal demo).

## Build

```
cd examples/threadx_guix_demo
make
```

This wraps the per-app `CMakeLists.txt` with `-DRA_USE_THREADX=ON
-DRA_USE_GUIX=ON` forced on. The resulting `build/threadx_guix_demo.elf`
links the vendored Eclipse ThreadX kernel and the vendored Eclipse
GUIX UI framework.

## Notes

- GLCDC bring-up is intentionally skipped here -- the EK-RA8D2 v1
  LCD-connector pin map is still marked TODO in `examples/lcd_demo/`.
  Once that lands, the same framebuffer can be picked up by GLCDC and
  the demo's pixels will appear on the panel.
- The shim in `port/guix/gx_display_driver_ra_glcdc.{h,c}` runs in
  single-buffer mode; the `buffer_toggle` hook just bumps a swap
  counter. A future revision can plug in true double-buffering by
  writing the alternate framebuffer base into the GLCDC `GR1_FLM2`
  register on each toggle.
- Touch input on EK-RA8D2 is not wired in this demo. The "tap" widget
  animates automatically from the `app` thread instead of waiting on
  a real GPIO event.
