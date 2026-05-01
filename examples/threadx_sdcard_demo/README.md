# threadx_sdcard_demo

Eclipse ThreadX bring-up smoke test on the EK-RA8D2.

Two ThreadX threads, each blinking a different LED at a different rate:

| Thread     | Priority | Period           | LED        |
|:-----------|:---------|:-----------------|:-----------|
| `blink_a`  | 4        | 1000 ms (1 Hz)   | LED1 P6_00 |
| `blink_b`  | 4        | 2000 ms (0.5 Hz) | LED2 P3_03 |

## What it exercises

- The project's port glue under `port/threadx/cortex_m85/`:
  - `tx_initialize_low_level.S` -- 1 ms SysTick reload, system stack
    pointer save, priority fix-up.
- The vendored upstream port at
  `libs/third_party/threadx/ports/cortex_m85/gnu/` (PendSV / SVC /
  schedule / context save / context restore).
- `port/threadx/tx_user.h` tunables (1 ms tick, single-mode-secure,
  TIMER_PROCESS_IN_ISR, etc.).
- The HAL's `ra_gpio_output_init` + `ra_gpio_toggle` -- same paths
  `examples/blink_hal` uses, but driven by the RTOS instead of a
  busy-wait.

## What it deliberately skips

- `ra_cgc_init()` -- the CGC PRCR-protected register sequence still
  HardFaults on the bare chip today. Without it the CPU runs on
  MOCO ~8.4 MHz, which `port/threadx/cortex_m85/tx_initialize_low_level.S`
  uses to compute the SysTick reload value.

## Build / flash

```sh
cd examples/threadx_sdcard_demo
make             # produces build/threadx_sdcard_demo.elf / .hex / .bin
make flash       # JLinkExe load via scripts/flash.sh
make ozone       # SEGGER Ozone debugger
```

## Vector-table contract

`vector_table.c` keeps the project's standard set of weak handler
aliases. The application overrides `SysTick_Handler` in `main.c` to
tail-call `_tx_timer_interrupt`. `PendSV_Handler` and `SVC_Handler`
are supplied as strong symbols by the upstream ThreadX port and
override the weak aliases automatically.
