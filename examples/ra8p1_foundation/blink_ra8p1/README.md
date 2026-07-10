# blink_ra8p1 -- RA8P1 build-foundation

Minimal LED blink that exists to **prove the RA8 multi-chip foundation**: the
`ra_core` + `ra_hal` libraries compile and link for the Renesas **RA8P1**
(`R7KA8P1KFLCAC`), not just the RA8D2.

## Build

```sh
cd examples/ra8p1_foundation/blink_ra8p1
make                 # -> build/blink_ra8p1.elf, built with cmake/toolchain-ra8p1.cmake
make size
```

`cmake/toolchain-ra8p1.cmake` includes the RA8D2 toolchain (identical Cortex-M85
compiler and flags) and adds `-DRA_DEVICE_RA8P1`. `libs/ra_core/inc/ra_device.h`
reads that define to select the RA8P1 feature set. A `#error` guard in `main.c`
fails the build loudly if it is ever configured with the RA8D2 toolchain.

## Why it is not under `examples/ek_ra8d2/`

The peripheral register bases and the memory map are **byte-identical** between
the RA8D2 and RA8P1 (verified from both chips' FSP CMSIS headers and Zephyr
device trees; see the RA8P1 difference-analysis issue). The only hardware
addition is the Arm Ethos-U55 NPU (`libs/ra_hal/inc/ra_npu_regs.h`). So this app
reuses the `ra_board_ek_ra8d2` board layer unchanged and differs from the RA8D2
`blink` only in its toolchain file and a RA8P1-accurate `linker_script.ld`
(1664 KB SRAM, RA8P1 HUM `R01UH1064EJ`).

It is **excluded from the top-level RA8D2 unified build** (`ra8p1_foundation` is
skipped in the repo-root `CMakeLists.txt`) so `make blink` / `make blink_hal`
and every other RA8D2 target stay byte-for-behaviour unchanged.

## Status

Build-foundation only -- **not** hardware-validated. There is no RA8P1 board
layer or on-silicon bring-up yet; those are tracked as follow-up issues.
