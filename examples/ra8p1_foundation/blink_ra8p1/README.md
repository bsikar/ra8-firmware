# blink_ra8p1

A minimal LED blink whose only job is to prove the RA8 multi-chip foundation:
that `ra8_core` and `ra8_hal` compile and link for the Renesas RA8P1, not just
the RA8D2. Everything else in this tier stands on it.

The RA8P1 toolchain file includes the RA8D2 one -- identical Cortex-M85
compiler and flags -- and adds the device define that `ra8_device.h` reads to
select the RA8P1 feature set. A `#error` guard in `main.c` fails the build
loudly if it is ever configured with the RA8D2 toolchain, so the two cannot be
confused silently.

## Why it is not under `examples/ek_ra8d2/`

The peripheral register bases and the memory map are byte-identical between the
RA8D2 and the RA8P1, verified from both chips' vendor headers and device trees.
The only hardware addition is the Ethos-U55 NPU. So this app differs from the
RA8D2 blink only in its toolchain file, an RA8P1-accurate linker script, and
the dedicated `ra8_board_ra8p1` board layer it selects. That board layer's LED,
switch and console pins are provisional, mirrored from the pin-compatible
EK-RA8D2 and marked with a TODO until an RA8P1 board is defined.

The whole RA8P1 tier is excluded from the top-level RA8D2 unified build, so
every RA8D2 target stays byte-for-behaviour unchanged.

It builds and runs against that board layer under the emulator's RA8P1 device
model. Nobody has run it on an RA8P1, because there is no RA8P1 board.
