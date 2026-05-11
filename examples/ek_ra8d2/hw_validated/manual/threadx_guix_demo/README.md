# threadx_guix_demo

ThreadX + GUIX font-test demo for the EK-RA8D2.

Renders three prompt widgets, one per GUIX-bundled system font,
composited via GLCDC graphics layer 1 over the BG plane on the
1024 x 600 parallel TFT (Renesas "Parallel Graphics Expansion
Board 1").  The screen makes the relative size / weight of each
font easy to eyeball.

## What this app shows

Top 256 x 128 of the panel:

| Row | Font                     | Format             |
|-----|--------------------------|--------------------|
| 1   | `_gx_system_font_mono`   | 1 bpp monochrome   |
| 2   | `_gx_system_font_4bpp`   | 4 bpp anti-aliased |
| 3   | `_gx_system_font_8bpp`   | 8 bpp anti-aliased |

Each row shows the same sample text ("AaBb 0123") so character
shapes / kerning / anti-aliasing differences are directly
comparable.

The rest of the panel (right strip + bottom strip) is solid black
from the BG plane.  The on-board LED1 toggles as a heartbeat in
the GUIX system thread.

## Boot sequence (what we put together)

1. **`main()` (bare-metal, before ThreadX):**
   - `ra_cgc_init`  -- bring up clocks (PLL1 + LCDCLK divider).
   - `ra_mstp_init` -- release peripheral module-stop gates.
   - `ra_board_led_init(LED1)`.
   - `tx_kernel_enter` -- never returns.
2. **`gui_thread` (priority 5, first ThreadX thread to run):**
   - `ra_time_init` + `ra_board_lcd_panel_power_on`
     + `ra_board_glcdc_init(RGB888)` -- panel POR / GLCDC pin
     routing.  All blocking sleeps use `tx_thread_sleep` so
     SysTick stays in ThreadX's hands.
   - `ra_glcdc_init` + `ra_glcdc_start`
     + `ra_glcdc_layer1_show(&s_framebuffer)` -- chip-side
     compositor up; layer 1 reads the GUIX framebuffer.
   - `ra_guix_display_driver_bind` + `gx_system_initialize`
     + `gx_display_create` -- wire GUIX through the
     `port/guix/gx_display_driver_ra_glcdc.c` shim.
   - `gx_display_color_table_set` + `gx_display_font_table_set`
     -- 32-slot color table and 4-slot font table (slot 0 unused).
   - Build the widget tree:
     `gx_canvas_create` -> `gx_canvas_show`
     -> `gx_window_root_create` -> `gx_window_create` (main)
     -> three `gx_prompt_create` calls, one per font.
   - `gx_widget_show` + `gx_system_canvas_refresh` to force
     the first paint before the system thread blocks on the
     event queue.
   - `gx_system_start` (returns immediately after resuming the
     GUIX internal thread; `gui_thread_entry` then exits).
3. **GUIX system thread (priority 16, spawned by GUIX itself):**
   - Pumps the event queue and repaints the dirty list.

## Known issues / TODO

### First-child blank workaround

A zero-size `s_prompt_dummy` is attached to the main window
BEFORE the three real font prompts.  On this GUIX 6.x / 565RGB /
EK-RA8D2 configuration the **first** prompt attached to a parent
window paints its background but never renders text, every
subsequent prompt renders fine.  We could not reproduce on the
GUIX win32 simulation, and reading `_gx_canvas_drawing_initiate`
+ `_gx_widget_children_draw` did not surface an obvious cause.
The dummy widget eats the bug so the three real font prompts all
render.  Filed as TODO -- pursue with a real debugger attached
inside the GUIX paint path.

### OTF asset, not yet usable

`libs/fonts/ArnoPro-Regular.otf` is committed as the source for a
future custom font but GUIX only consumes bitmap `GX_FONT` tables
-- converting the OTF requires either GUIX Studio (Renesas
Windows tool) or a FreeType-based emitter that doesn't exist in
the tree yet.  See `libs/fonts/README.md`.

## Build / flash

```sh
make threadx_guix_demo     # from repo root
# or from this directory:
make
make flash                 # via SEGGER J-Link OB
```

A power-cycle is required after flashing for the chip to boot
the new firmware fresh.

## Validation

Visually verified 2026-05-11 on EK-RA8D2 v1 with Parallel
Graphics Expansion Board 1.  All three font lines render in the
top 256 x 128 region of the panel.  HIL flash via Pi + J-Link OB.
