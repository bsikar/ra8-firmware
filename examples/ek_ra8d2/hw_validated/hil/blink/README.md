# blink

Minimum-viable LED-blink HIL test for the EK-RA8D2. Standalone
example app: `examples/ek_ra8d2/hw_validated/hil/blink/main.c` plus its own `vector_table.c`,
`system_init.c`, `secure_exception.c`, `trustzone_init.c`,
`linker_script.ld`, `Makefile`, and `CMakeLists.txt`.

## What it does

Drives EK-RA8D2 user-LED1 (P600, blue) at a clean 1 Hz cycle (500 ms
on, 500 ms off) using the Cortex-M85 SysTick timer for delay -- no
busy-wait. LED1 / P600 wiring per EK-RA8D2 v1 User's Manual Table 24
("EK-RA8D2 Board LED Functions") page 31.

Uses `ra8_board_ek_ra8d2` BSP for LED init/toggle (`ra8_board_led_init`,
`ra8_board_led_toggle`) and `ra8_time` for the 1 ms SysTick. For a
multi-LED HAL-driven variant see `examples/ek_ra8d2/hw_validated/hil/blink_hal`.

## Build + flash

From the repo root:

```sh
make blink                       # cross-compile -> examples/ek_ra8d2/hw_validated/hil/blink/build/blink.elf
make -C examples/blink flash     # flash via on-board J-Link OB
```

Or standalone, from inside `examples/ek_ra8d2/hw_validated/hil/blink/`:

```sh
cd examples/ek_ra8d2/hw_validated/hil/blink/
make                   # configure + build -> build/blink.{elf,hex,bin}
make flash             # flash build/blink.hex
make clean             # rm -rf build
```

Or directly via cmake (e.g. for a `Release` build):

```sh
cmake -S examples/ek_ra8d2/hw_validated/hil/blink -B examples/ek_ra8d2/hw_validated/hil/blink/build \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8d2.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build examples/ek_ra8d2/hw_validated/hil/blink/build
bash scripts/dev/flash.sh examples/ek_ra8d2/hw_validated/hil/blink/build/blink.hex
```

## Debugging

```sh
make -C examples/blink ozone    # open SEGGER Ozone GUI debugger on blink.elf
make -C examples/blink debug    # attach gdb via JLinkGDBServer on blink.elf
```

## Notes

- The chip boots on MOCO at ~8.4 MHz (measured via `DWT.CYCCNT`, lines
  up with the RA-family MOCO nominal 8 MHz spec). `k_blink_cpu_hz_at_reset`
  in `main.c` reflects this. After CGC bring-up brings HOCO + PLL up
  to 1 GHz, the constant should be replaced with the actual operating
  rate.
- `SystemInit` currently skips cache, MPU, and TrustZone init -- those
  caused HardFaults on bring-up and need their own dedicated work
  before being re-enabled.
- The linker stack is in SRAM-0 only (1 MiB). SRAM-1 (the second 1 MiB
  bank) needs explicit MSTPCR + SRAMSAR programming before it's
  accessible.

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Table 24 "EK-RA8D2 Board LED Functions" p 31, and HUM
(R01UH1065EJ0130) Ch SysTick / IOPORT.
