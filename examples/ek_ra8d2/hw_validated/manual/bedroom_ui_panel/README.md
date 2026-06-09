# bedroom_ui_panel

Bare-metal (no ThreadX) GUIX firmware that renders the shared `bedroom_ui`
on the EK-RA8D2 7-inch 1024x600 parallel TFT via GLCDC.

The GUIX source drawn here is the shared
`examples/shared/bedroom_ui/bedroom_ui.c` -- the same widget tree, layout,
colours, and tab-click handling whether it runs on the real panel (GLCDC) or in
the board emulator (`make sim-bedroom_ui_panel`). Only the display backend and
RTOS bind are wired per target; the UI code is identical.

## What to expect

After flashing:

1. Board boots; clocks, MSTP, the 1 kHz SysTick tick, and external SDRAM come
   up. The panel is reset, the backlight (`P514` BLEN) is asserted, and GLCDC
   is configured for 1024x600 RGB565, scanning out the SDRAM framebuffer.
2. GUIX initialises single-threaded (no ThreadX), creates a display + canvas
   over the SDRAM framebuffer, and builds the bedroom UI.
3. The panel shows the 3-tab smart-room surface (tab 0 active): a top tab bar
   over a room heading and a 2x2 grid of stat cards (climate / lighting /
   security).
4. A cooperative loop pumps GUIX events and refreshes the canvas (~60 Hz).

If any boot or GUIX-init step fails, the red LED latches on and the CPU parks
in WFI.

## Why this app exists

The shared `bedroom_ui` previously had only a host preview -- no target build.
This is the target counterpart: it proves the single-threaded GUIX bind that
the host preview validates also flashes and draws on the real GLCDC panel.

It is single-threaded GUIX on purpose:

- The host preview already proves the single-threaded GUIX bind works.
- The board emulator (`tools/board_sim`) boots bare-metal firmware with a
  cooperative SysTick but does **not** model ThreadX, so a bare-metal GUIX app
  is the one that both flashes AND renders in the emulator.

`threadx_guix_demo` is the ThreadX-driven counterpart; this app drops ThreadX
and drives GUIX from `main()`.

## Architecture notes

- **One buffer, three roles.** The 1024x600 RGB565 framebuffer (1.2 MiB) lives
  in external SDRAM (`0x68000000`, via the linker's `.sdram_data` NOLOAD
  section -- it does not fit the 2 MiB SRAM alongside .data/.bss/stack). That
  one pointer is the GLCDC GR1 scan-out source, the display PAL's bound
  framebuffer, and the GUIX canvas memory. GUIX paints straight into the buffer
  the panel scans out -- no copy, single-buffer.
- **No ThreadX, no `cmake/guix.cmake`.** `cmake/guix.cmake` hard-requires
  ThreadX, so this app's `CMakeLists.txt` builds the GUIX core directly (every
  common TU except the ThreadX bind), exactly like the host preview but
  cross-compiled. GUIX runs with `GX_DISABLE_THREADX_BINDING` on its generic
  RTOS surface, bound to `port/guix/gx_generic_rtos_bare.c`.
- **Time source.** The bare-metal bind's `gx_generic_system_time_get()` returns
  the SysTick-backed `ra_time_ms()` (vs the host bind's POSIX `clock_gettime`).

## Build

```
make            # -> build/bedroom_ui_panel.elf / .hex / .bin
```

Or from the repo root: `make bedroom_ui_panel`.

## Render-verify on the board emulator

```
cd tools/board_sim && cmake -B build -S . && cmake --build build -j
./build/board_sim \
    ../../examples/ek_ra8d2/hw_validated/manual/bedroom_ui_panel/build/bedroom_ui_panel.elf \
    --ppm /tmp/bedroom.ppm
```

Success: the final PC is not in the panic-halt range and `/tmp/bedroom.ppm`
shows the bedroom UI (varied pixels -- cards / text -- not blank or one solid
colour).

For a live window straight from the repo root: `make sim-bedroom_ui_panel`
(cross-builds the app, then boots its `.elf` in the emulator).
