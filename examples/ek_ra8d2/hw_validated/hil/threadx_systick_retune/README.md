# threadx_systick_retune

Eclipse ThreadX SysTick "retune to the live CPUCLK0" demo for the
EK-RA8D2 (issue #287).

`port/threadx/src/cortex_m85/tx_initialize_low_level.S` programs
SysTick.LOAD from a compile-time clock assumption (`RA8_BOOT_CLOCK_HZ` =
1 GHz, the post-CGC CPUCLK0 target). That is only correct if the app
raised CPUCLK0 to exactly that rate before `tx_kernel_enter()`. Entering
the kernel on a different clock -- the boot-default MOCO (~8 MHz) or any
other CGC target -- would run the nominal 1 ms kernel tick scaled by the
clock ratio (~119x too slow on MOCO), silently drifting every
`tx_thread_sleep` / timer.

`ra8_threadx_systick_retune()` (in
`port/threadx/src/cortex_m85/tx_systick_retune.c`) closes that gap: called
from `tx_application_define`, it re-derives SysTick.LOAD from the *live*
CPUCLK0 rate reported by `ra8_cgc_get_clock_hz`, so the tick is accurate
regardless of clock. If the live clock is too high for the 24-bit
SYST_RVR register it returns an error instead of truncating.

| Thread    | Priority | Period          | Action                 |
|:----------|:---------|:----------------|:-----------------------|
| `retune`  | 4        | 250 ms (4 Hz)   | Toggle LED1, bump tick |

## What it exercises

- `ra8_cgc_init()` raises CPUCLK0 to the PLL1 target (1 GHz).
- `tx_application_define` calls `ra8_threadx_systick_retune()`, then
  independently recomputes the expected reload with
  `ra8_threadx_systick_reload_for()` from the live CPUCLK0 and reads
  SYST_RVR back to confirm the retune programmed the correct value.
- The project port glue under `port/threadx/src/cortex_m85/`
  (`tx_initialize_low_level.S` SysTick bring-up) plus the vendored
  upstream Cortex-M85 GNU port (PendSV / SVC / schedule).

## HIL / SIL contract

`hil.conf` uses `HIL_MODE=jlink_memprobe` with two symbols:

- `g_threadx_retune_tick` (primary) -- bumped each worker wake; must
  advance `>= 3` over the window (scheduler alive + retuned SysTick still
  ticks).
- `g_threadx_retune_bad` (failure) -- set to 1 iff the retune failed or
  SYST_RVR did not match the reload computed from the live clock; must
  stay `0`.

`g_threadx_retune_reload` publishes the computed reload (999999 at
CPUCLK0 = 1 GHz) for observability.

board_sim runs the identical ARM retune path -- the SYST_RVR write lands
in the emulated System Control Space and reads back -- so the board_sim
gate (`scripts/sim/sil_all.sh`) verdict equals the on-hardware probe: SIM ==
HIL.

## Build / flash

```sh
cd examples/ek_ra8d2/hw_validated/hil/threadx_systick_retune
make             # produces build/threadx_systick_retune.elf / .hex / .bin
make flash       # JLinkExe load via scripts/dev/flash.sh
make ozone       # SEGGER Ozone debugger
```

## Vector-table contract

`vector_table.c` keeps the project's standard weak handler aliases. The
shared weak `SysTick_Handler` in `libs/ra8_core/src/ra8_time.c` dispatches
to `_tx_timer_interrupt`, so no per-app SysTick override is needed (issue
#8). `PendSV_Handler` and `SVC_Handler` are strong symbols supplied by the
upstream ThreadX port and override the weak aliases automatically.

## BSP usage

Uses the `ra8_board_ek_ra8d2` BSP for LED1 init/toggle (P600 per EK-RA8D2
v1 UM Table 24 p 31).
