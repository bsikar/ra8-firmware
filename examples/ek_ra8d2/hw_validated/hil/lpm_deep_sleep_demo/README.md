# lpm_deep_sleep_demo

Cortex-M85 Deep Sleep mode (LPSCR.LPMD = 0, SCR.SLEEPDEEP = 1)
wake-count demo for the bare EK-RA8D2 EVM.

This demo is one tier deeper than `lpm_idle_demo` -- both leave
LPSCR.LPMD at 0 (System Active per HUM Ch 11.2.20 p 457), but this
one asserts the Cortex-M85 SCR.SLEEPDEEP bit before WFI so the core
power-gates more aggressively. SysTick still ticks in Deep Sleep, so
it serves as the wake source on a millisecond cadence.

## What it does

1. CGC + SysTick + LED1 + LPM block bring-up.
2. Main loop:
   - `ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_sleep)` -- WFI with
     SLEEPDEEP asserted.
   - `ra8_delay_ms(100)` lets the SysTick handler take us ~100 wakes
     deeper.
   - Increment `g_lpm_deep_wake_count` (volatile, externally
     readable via SWD).
   - Toggle LED1.

## HIL gate

`HIL_MODE=jlink_memprobe` on `g_lpm_deep_wake_count`. The probe
expects at least 5 increments inside a 3 s window. There is no
boot banner / UART scrape because the SCI8 module clock (PCLKA) is
gated under the default `ra8_lpm_init` when SLEEPDEEP is asserted,
and an earlier prototype that printed wake-count lines wedged the
console on every cycle. Reading the symbol over SWD bypasses the
gated peripheral fabric entirely.

## Bench verification

The repo is configured so anything under `hw_validated/hil/` is
expected to pass automated HIL on a real board. This demo has been
compile-verified but the human still has to:

1. `make lpm_deep_sleep_demo`
2. Flash via the Pi-bound HIL flasher: `bash scripts/hil/flash.sh
   examples/ek_ra8d2/hw_validated/hil/lpm_deep_sleep_demo/build/lpm_deep_sleep_demo.hex`
3. Run the HIL gate from the repo root: `bash scripts/hil/run.sh
   lpm_deep_sleep_demo`
4. Confirm pass / fail.

No external hardware required.
