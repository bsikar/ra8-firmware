# blink

Minimum-viable LED-blink smoke test for the EK-RA8D2.

## What it does

Drives the EK-RA8D2 user-LED candidate pins (P6_00, P6_01, P6_02,
P3_03, P10_07) at a clean 1 Hz cycle (500 ms on, 500 ms off) using
the Cortex-M85 SysTick timer for delay -- no busy-wait.

## Build + flash

From the repo root:

```sh
make build               # cross-compiles examples/blink/main.c
make flash               # flashes via on-board J-Link OB
```

Or pick the example explicitly:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8d2.cmake \
                    -DEXAMPLE=blink -DCMAKE_BUILD_TYPE=Release
cmake --build build
./scripts/flash.sh
```

## Notes

- The chip boots on MOCO at ~8.4 MHz (measured via DWT.CYCCNT, lines
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
