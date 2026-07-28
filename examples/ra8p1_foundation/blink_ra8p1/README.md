# blink_ra8p1 -- RA8P1 build-foundation

Minimal LED blink that exists to **prove the RA8 multi-chip foundation**: the
`ra8_core` + `ra8_hal` libraries compile and link for the Renesas **RA8P1**
(`R7KA8P1KFLCAC`), not just the RA8D2.

## Build

```sh
cd examples/ra8p1_foundation/blink_ra8p1
make                 # -> build/blink_ra8p1.elf, built with cmake/toolchain-ra8p1.cmake
make size
```

`cmake/toolchain-ra8p1.cmake` includes the RA8D2 toolchain (identical Cortex-M85
compiler and flags) and adds `-DRA8_DEVICE_RA8P1`. `libs/ra8_core/inc/ra8_device.h`
reads that define to select the RA8P1 feature set. A `#error` guard in `main.c`
fails the build loudly if it is ever configured with the RA8D2 toolchain.

## Why it is not under `examples/ek_ra8d2/`

The peripheral register bases and the memory map are **byte-identical** between
the RA8D2 and RA8P1 (verified from both chips' FSP CMSIS headers and Zephyr
device trees; see the RA8P1 difference-analysis issue). The only hardware
addition is the Arm Ethos-U55 NPU (`libs/ra8_hal/inc/ra8_npu_regs.h`). So this app
builds against the dedicated `ra8_board_ra8p1` board layer (issue #226, selected
via `ra8_add_app(... BOARD ra8p1)`) and differs from the RA8D2 `blink` only in
its toolchain file and a RA8P1-accurate `linker_script.ld` (1664 KB SRAM, RA8P1
HUM `R01UH1064EJ`). That board layer's LED/switch/console pins are provisional
(mirrored from the pin-compatible EK-RA8D2) with a `TODO(EK-RA8P1 UM /
ra8p1_kicad)` until an RA8P1 board is defined.

It is **excluded from the top-level RA8D2 unified build** (`ra8p1_foundation` is
skipped in the repo-root `CMakeLists.txt`) so `make blink` / `make blink_hal`
and every other RA8D2 target stay byte-for-behaviour unchanged.

## Status

Build-foundation only -- **not** hardware-validated. The dedicated
`ra8_board_ra8p1` board layer now exists (issue #226) and this app builds and
runs against it under `ra8_emulator --device ra8p1`, but there is no RA8P1 board for
on-silicon bring-up yet; that is tracked as follow-up.
