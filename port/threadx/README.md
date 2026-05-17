# ra8d2-firmware ThreadX port

Project glue around the upstream Eclipse ThreadX Cortex-M85 GNU port that
ships under `libs/third_party/threadx/ports/cortex_m85/gnu/`.

## What lives here

- `tx_user.h` -- RA8D2-tuned tunables (1 ms tick, 32 priorities, single
  mode secure, etc.). Both the ThreadX library build and any consuming
  application TU pick this up via `TX_INCLUDE_USER_DEFINE_FILE`.
- `cortex_m85/tx_initialize_low_level.S` -- replacement for the upstream
  port's low-level init. Drops the upstream's `_vectors`,
  `HardFault_Handler`, `UsageFault_Handler`, and `SysTick_Handler`
  symbols (which would collide with the project's `vector_table.c`),
  configures SysTick to fire at the rate declared in `tx_user.h`, and
  saves the boot main-stack pointer for ISR processing.
- `CMakeLists.txt` -- thin shim that re-includes
  `cmake/threadx.cmake`. Either include path produces a static library
  named `threadx` with the right include dirs and the user-define
  symbol.

## How a per-app build links it in

```cmake
option(RA_USE_THREADX "Link Eclipse ThreadX into this app" OFF)
if(RA_USE_THREADX)
    include(${RA_REPO_ROOT}/cmake/threadx.cmake)
    target_link_libraries(<app>.elf PRIVATE threadx)
endif()
```

The example app `examples/ek_ra8d2/hw_validated/hil/threadx_blink/` defaults `RA_USE_THREADX=ON`
in its `CMakeLists.txt`.

## Vector table contract

The ThreadX scheduler relies on the project's `vector_table.c` to route:

- `SysTick_Handler` -> tail-call `_tx_timer_interrupt`.
- `PendSV_Handler` -> resolves to the upstream port's strong symbol
  (the project's vector_table provides only a weak alias to
  `Default_Handler`, which the linker overrides).
- `SVC_Handler` -> same story; upstream port wins via strong symbol.

`HardFault_Handler` and `UsageFault_Handler` stay project-owned; the
upstream port's versions are NOT included in the build (the upstream
file `tx_initialize_low_level.S` is excluded by `cmake/threadx.cmake`).

## SysTick reload value

`tx_initialize_low_level.S` uses the boot-default MOCO clock
(~8.4 MHz) to compute SysTick reload. If a future app calls
`ra_cgc_init()` before `tx_kernel_enter()` the reload value will need
to be reprogrammed to keep the 1 ms tick accurate. Today's
`examples/ek_ra8d2/hw_validated/hil/threadx_blink` skips `ra_cgc_init()` for exactly this reason,
matching `examples/ek_ra8d2/hw_validated/hil/blink_hal`.
