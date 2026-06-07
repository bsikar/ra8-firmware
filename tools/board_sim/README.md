<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# board_sim -- RA8D2 board emulator (companion dev tool)

Boots the **real, unmodified firmware `.elf`** -- the same image that flashes to
the EK-RA8D2 -- on an emulated Cortex-M and shows what its GLCDC drives, in a
macOS window. Unlike `tools/simulator` (which recompiles the GUIX UI natively),
board_sim runs the actual ARM binary, so it exercises the genuine bring-up and
peripheral-driver code path.

Standalone tool under `tools/`, outside the firmware CI gates. Needs Unicorn +
Capstone (`brew install unicorn capstone`).

## Build & run

```sh
cd tools/board_sim && cmake -B build -S . && cmake --build build -j
./build/board_sim <firmware.elf>                 # headless: boot + MMIO report
./build/board_sim <firmware.elf> --view          # live macOS window
./build/board_sim <firmware.elf> --ppm out.ppm   # write the final frame
./build/board_sim <firmware.elf> --size 480x272  # frame size (default 1024x600)
```

Or from the repo root: `make simulate-<app>` (e.g. `make simulate-lcd_color_cycle`)
cross-builds the app and opens its live window. Close the window to exit.

## How it works

- **CPU**: Unicorn (QEMU's core as a library) tops out at Cortex-M33 (Armv8-M),
  but the M85 (Armv8.1-M) firmware executes on it -- the boot path emits no
  v8.1-M-only opcode. An invalid-instruction trap reports any that ever appears.
- **Memory map**: ITCM / MRAM (code+vectors) / DTCM / SRAM / DATA_FLASH / SDRAM /
  PPB mapped as RAM; the Renesas peripheral space (`0x40000000`+) is callback MMIO.
- **Peripheral model** (sparse): control writes read back as written, so
  "configure then verify" works; once the firmware spins reading one address (a
  "wait for ready/idle" poll) past a threshold, reads alternate `0` / all-ones so
  a single-bit poll for either edge completes instead of timing out. This generic
  rule satisfies almost every stabilization poll on the boot path.
- **Targeted quirk**: the MRMS frequency latches (MRCFREQ/MREFREQ) strip a write
  key byte on readback -- modelled directly, since it is an exact-value (not
  edge) poll. This is currently the *only* register needing bespoke behaviour.
- **Time**: bare-metal delays are SysTick-driven. Between emulation chunks the
  installed `SysTick_Handler` is cooperatively invoked as a function so the tick
  counter advances and `ra_delay_ms` returns.
- **Display**: the current frame is read from emulated GLCDC state -- the BG_BGC
  background colour today -- and presented via a small self-contained Cocoa view.

## Status / limits

- Proven on `lcd_color_cycle`: boots clocks -> SDRAM -> GLCDC and cycles the
  background colour (red/green/blue/white), shown live.
- Graphics-layer framebuffers in SDRAM (GUIX UIs) are not blitted yet, and
  ThreadX apps use a different (PendSV-scheduled) boot path not yet modelled.
- The peripheral model fakes hardware *handshakes*; it validates "does the
  firmware drive the controller correctly," not real silicon timing. It
  complements HIL, it does not replace it.
