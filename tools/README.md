<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# Developer tooling (`tools/`)

Two ways to *see the bedroom UI / firmware run* without a board, plus the gates
that keep them honest. They look similar but answer different questions -- pick
by what you're doing.

| You want to... | Use | How |
|----------------|-----|-----|
| Iterate on the **UI** fast, click tabs, try any display size | **`tools/simulator`** (native GUIX recompile) | `make sim [PANEL=<name>]` (alias `make ui`) |
| Check the **real flashed `.elf`** actually boots + drives the panel | **`tools/board_sim`** (Unicorn CPU emulator) | `make emulate-<app> [PANEL=<name>]` |
| Preview a **macOS host demo** (floorplan, guix_tabs) | host examples | `make <host-app>` / `make run-<host-app>` |

`make apps` lists every firmware + host app; `make help` is the grouped target
reference. Git hooks auto-install on first `make` (or `make hooks`).

## `tools/simulator` -- UI design simulator

Recompiles the **shared** `examples/shared/bedroom_ui/bedroom_ui.c` *natively on
macOS* and runs it under real GUIX, in a clickable window. Fast edit-render loop
for laying out screens; **not** the firmware binary. The display is config-driven
(`tools/simulator/panels/<name>.toml`: `name`/`width`/`height`/`format`), and the
UI is resolution-adaptive, so any panel size renders without clipping.
Headless `--png` for scripted renders. Details: `tools/simulator/README.md`.

## `tools/board_sim` -- board emulator

Boots the **unmodified cross-compiled firmware `.elf`** on an emulated Cortex-M
(Unicorn), models the RA8D2 peripheral space, ticks SysTick, and presents the
real GLCDC output. Highest fidelity -- it exercises the actual bring-up
(clocks/SDRAM/GLCDC) and driver code. `--view` (live window), `--ppm` (headless
frame), `--panel <file.toml>` (any display), `--click X Y` (inject touch through
the real GT911 path). Details: `tools/board_sim/README.md`.

## Regression gates

- `scripts/board_sim_smoke.sh` -- boots each display app on the emulator and
  asserts it runs to its main loop without faulting (no invalid opcode / unmapped
  access, not parked in the panic-halt loop).

Run either tool with `--help`/no args for its full flag set.
