# txm_touch_led_m85

Legible touch-to-LED ThreadX Module demo (`RA8FW-1262`).

## Flow

1. `txm_touch_led_m33/module_start.zig` compiles into a Cortex-M33 ThreadX module
   binary and packs into `zig-out/arm/txm_touch_led_m33.ra8app` with capabilities
   `display | led` (`0x1 | 0x8 = 0x9`, `min_api_version = 2`).
2. `txm_touch_led_m85` embeds that signed `.ra8app` in `.txm_module` MRAM and on
   boot brings up EK-RA8D2 clocks, MSTP, timebase, UART console, LED1 (P600),
   and GT911 touch (`ra8_board_touch_open`).
3. Before loading, `verify.zig` proves the admission gate refuses the container
   if `led` is withheld from the host grant and refuses a tampered payload, then
   admits it with `display | led`.
4. Once started inside the ThreadX Module Manager, `txm_touch_led_m33` clears
   LED1, paints its region dark slate, and polls `touchEvent()`. On a touch
   `.press` (from GT911 silicon or `ra8_emulator --click`), it calls
   `ledSet(0, true)` (`ra8_board_led_on(LED1)`), paints the region green, and
   exits `0` so the manager verifies clean unload with zero byte-pool leak.

## Build and run

- Module only: `zig build txm-touch-led-m33` -> `zig-out/arm/txm_touch_led_m33.ra8app`
- Full image: `zig build arm` -> `zig-out/arm/txm_touch_led_m85.elf`
- Emulator verification:
  `ra8_emulator zig-out/arm/txm_touch_led_m85.elf --console --click 400 240 --ms 4000 --until PASS`
