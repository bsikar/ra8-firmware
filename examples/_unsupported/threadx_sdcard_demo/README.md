# threadx_sdcard_demo

Eclipse ThreadX bring-up smoke test on the EK-RA8D2.

Two ThreadX threads, each blinking a different LED at a different rate:

| Thread     | Priority | Period           | LED        |
|:-----------|:---------|:-----------------|:-----------|
| `blink_a`  | 4        | 1000 ms (1 Hz)   | LED1 P6_00 |
| `blink_b`  | 4        | 2000 ms (0.5 Hz) | LED2 P3_03 |

## What it exercises

- The project's port glue under `port/threadx/src/cortex_m85/`:
  - `tx_initialize_low_level.S` -- 1 ms SysTick reload, system stack
    pointer save, priority fix-up.
- The vendored upstream port at
  `libs/third_party/threadx/ports/cortex_m85/gnu/` (PendSV / SVC /
  schedule / context save / context restore).
- `port/threadx/inc/tx_user.h` tunables (1 ms tick, single-mode-secure,
  TIMER_PROCESS_IN_ISR, etc.).
- The HAL's `ra8_gpio_output_init` + `ra8_gpio_toggle` -- same paths
  `examples/blink_hal` uses, but driven by the RTOS instead of a
  busy-wait.

## What it deliberately skips

- `ra8_cgc_init()` -- the CGC PRCR-protected register sequence still
  HardFaults on the bare chip today. Without it the CPU runs on
  MOCO ~8.4 MHz, which `port/threadx/src/cortex_m85/tx_initialize_low_level.S`
  uses to compute the SysTick reload value.

## Build / flash

```sh
cd examples/threadx_sdcard_demo
make             # produces build/threadx_sdcard_demo.elf / .hex / .bin
make flash       # JLinkExe load via scripts/dev/flash.sh
make ozone       # SEGGER Ozone debugger
```

## Vector-table contract

`vector_table.c` keeps the project's standard set of weak handler
aliases. The application overrides `SysTick_Handler` in `main.c` to
tail-call `_tx_timer_interrupt`. `PendSV_Handler` and `SVC_Handler`
are supplied as strong symbols by the upstream ThreadX port and
override the weak aliases automatically.

## Note on the directory name

Despite the `sdcard_demo` directory name, the current `main.c` is a
straight copy of `examples/threadx_blink` (two LED-blink threads). No
SDHI / SD-card code is present in this app today. For a real SD-backed
filesystem demo see `examples/ek_ra8d2/hw_validated/hil/tz_secure_only_sd`
or the OSPI-backed `examples/ek_ra8d2/hw_validated/hil/threadx_fs_demo`.

## BSP usage

Uses `ra8_board_ek_ra8d2` BSP for LED1 / LED2 init/toggle (P600 / P303
per EK-RA8D2 v1 UM Table 24 p 31).

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Table 24 p 31, and Eclipse ThreadX Cortex-M85 GNU port docs.
