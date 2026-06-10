<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# Developer tooling (`tools/`)

Host-side dev tools that run the real firmware (and feed it data) without a
board, plus the gate that keeps them honest.

| You want to... | Use | How |
|----------------|-----|-----|
| Run a firmware **`.elf`** with no board: boot it, drive the panel, preview its UI | **`tools/board_sim`** (Unicorn CPU emulator) | `make sim-<app> [PANEL=<name>]` |
| Build a FAT **SD-card image** carrying a font (for `board_sim --sd` or a real card) | **`tools/mkfontimg`** | `cmake -S tools/mkfontimg -B tools/mkfontimg/build && cmake --build tools/mkfontimg/build` |

`make apps` lists every firmware app; `make help` is the grouped target
reference. Git hooks auto-install on first `make` (or `make hooks`).

## `tools/board_sim` -- the board emulator

Boots the **unmodified cross-compiled firmware `.elf`** on an emulated Cortex-M
(Unicorn), models the RA8D2 peripheral space, ticks SysTick, and presents the
real GLCDC output. Highest fidelity -- it exercises the actual bring-up
(clocks/SDRAM/GLCDC) and driver code, so the panel/UI you see is exactly what the
flashed firmware draws. Because it runs the real binary, the firmware renders at
the resolution it was *built* for: pointing a fixed-panel app at a different
`--panel` shows the genuine mismatch, not a magically re-laid-out screen.

Flags: `--view` (live macOS window), `--ppm <file>` (headless frame),
`--panel <file.toml>` / `--size WxH` (model a given display), `--click X Y`
(inject touch through the real GT911 path), `--sd <image>` (attach a FAT
SD-card image to the modelled SD-over-SPI device -- see `tools/mkfontimg`).
Panel descriptors live in `tools/board_sim/panels/<name>.toml`. From the repo
root, `make sim-<app>` builds the app and opens its live window; a chrome app
(e.g. `make sim-ereader_ui`) doubles as the UI preview. Layout: `inc/`
(headers) + `src/` (sources), matching the `libs/` convention. Details:
`tools/board_sim/README.md`.

For heavy headless runs (a large SD font read + glyph rasterisation is slow
under the CPU emulator), the run budget is env-overridable:
`BOARD_SIM_WALL_S=<seconds>` and `BOARD_SIM_MAX_CHUNKS=<n>` (no effect in
`--view`).

## `tools/mkfontimg` -- FAT SD-card image builder

Writes a font file into a FAT16 image **through the real `ra_fs`**, so the
on-card layout is byte-for-byte what the firmware reads back (same code path as
`tests/test_ra_sdmmc_card_reflow.c`). Used to feed `board_sim --sd` for the
`sd_font_render` app, or to format a physical card.

```sh
cmake -S tools/mkfontimg -B tools/mkfontimg/build && cmake --build tools/mkfontimg/build
tools/mkfontimg/build/mkfontimg libs/fonts/ArnoPro-Regular.otf font.img FONT.OTF
tools/board_sim/build/board_sim <app>.elf --sd font.img --ppm out.ppm
```

## Regression gate

- `scripts/board_sim_smoke.sh` -- boots each display app on the emulator and
  asserts it runs to its main loop without faulting (no invalid opcode / unmapped
  access, not parked in the panic-halt loop), and for the chrome UI app renders one
  frame and asserts the panel drew rich content.

Run the tool with `--help`/no args for its full flag set.
