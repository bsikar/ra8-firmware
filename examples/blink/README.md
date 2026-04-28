# blink

Minimum-viable LED-blink smoke test for the EK-RA8D2. Standalone
example app: `examples/blink/main.c` plus its own `vector_table.c`,
`system_init.c`, `secure_exception.c`, `trustzone_init.c`,
`linker_script.ld`, `Makefile`, and `CMakeLists.txt`.

## What it does

Drives the EK-RA8D2 user-LED candidate pins (P6_00, P6_01, P6_02,
P3_03, P10_07) at a clean 1 Hz cycle (500 ms on, 500 ms off) using
the Cortex-M85 SysTick timer for delay -- no busy-wait.

## Build + flash

From the repo root:

```sh
make blink                       # cross-compile -> examples/blink/build/blink.elf
make -C examples/blink flash     # flash via on-board J-Link OB
```

Or standalone, from inside `examples/blink/`:

```sh
cd examples/blink/
make                   # configure + build -> build/blink.{elf,hex,bin}
make flash             # flash build/blink.hex
make clean             # rm -rf build
```

Or directly via cmake (e.g. for a `Release` build):

```sh
cmake -S examples/blink -B examples/blink/build \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8d2.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build examples/blink/build
bash scripts/flash.sh examples/blink/build/blink.hex
```

## Debugging

```sh
make -C examples/blink ozone    # open SEGGER Ozone GUI debugger on blink.elf
make -C examples/blink debug    # attach gdb via JLinkGDBServer on blink.elf
```

## Notes

- The chip boots on MOCO at ~8.4 MHz (measured via `DWT.CYCCNT`, lines
  up with the RA-family MOCO nominal 8 MHz spec). `k_ra_cpu_hz_at_reset`
  in `main.c` reflects this. After CGC bring-up brings HOCO + PLL up
  to 1 GHz, the constant should be replaced with the actual operating
  rate.
- `SystemInit` currently skips cache, MPU, and TrustZone init -- those
  caused HardFaults on bring-up and need their own dedicated work
  before being re-enabled.
- The linker stack is in SRAM-0 only (1 MiB). SRAM-1 (the second 1 MiB
  bank) needs explicit MSTPCR + SRAMSAR programming before it's
  accessible.
- Only `P6_00`, `P3_03`, and `P10_07` are coded in
  `libs/ra_core/inc/ra_port_constants.h` as the canonical EK-RA8D2
  LED pins; the demo also drives `P6_01` and `P6_02` because the
  EK-RA family commonly uses three LEDs on consecutive port-6 pins.
  Confirm against the EK-RA8D2 schematic before relying on a
  specific pin in production code.
