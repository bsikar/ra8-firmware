# lpm_software_standby_demo

Software Standby (LPSCR.LPMD = 0x5) + RTC periodic wake-up demo for
the bare EK-RA8D2 EVM.

Software Standby is the first of the truly "deep" LPM states on the
RA8D2 -- almost every clock domain is gated, including the CPU and
SysTick. The only oscillators that survive are the sub-clock (SOSC)
and the always-on wake-up detectors fed by it (RTC alarm / periodic,
WUPEN0 / WUPEN1).

## What it does

1. CGC + SysTick + SCI8 + RTC + LPM block bring-up.
2. Emit boot banner `lpm_swstd: boot` over SCI8.
3. Seed the RTC to 2026-01-01 00:00:00.
4. Loop:
   - Read the current RTC time.
   - Arm the alarm 5 seconds in the future and enable RCR1.AIE.
   - Set WUPEN0.RTCALMWUPEN so the alarm cancels Software Standby.
   - `ra8_lpm_enter_sleep(k_ra8_sleep_mode_software_std)`.
   - On wake, increment `g_lpm_swstd_wake_count` and clear the
     alarm flag.

## HIL gate

`HIL_MODE=uart_scrape` on the banner `lpm_swstd: boot`. This is a
boot-only gate -- it confirms the firmware built, the CGC came up,
and the standby entry path executed, but it does NOT verify the
RTC alarm actually woke the chip. See the SOSC caveat below.

## SOSC caveat

The sub-clock crystal on this EVM has been observed to be
intermittent (same failure mode documented elsewhere in this repo
for ethernet bring-up). If SOSC is not ticking when Software Standby
is entered, the RTC alarm never fires and WFI hangs until an
external reset. Because the bench truth varies cycle-to-cycle, the
HIL gate is intentionally lenient -- pass-on-banner -- and the wake
path itself is verified by direct bench-work with the debugger or
an oscilloscope, not by the automated HIL.

## Bench verification

This demo has been compile-verified but the human still has to:

1. `make lpm_software_standby_demo`
2. Flash via the Pi-bound HIL flasher: `bash scripts/hil/flash.sh
   examples/ek_ra8d2/hw_validated/hil/lpm_software_standby_demo/build/lpm_software_standby_demo.hex`
3. Run the HIL gate from the repo root: `bash scripts/hil/run.sh
   lpm_software_standby_demo`
4. Optionally attach a debugger and watch `g_lpm_swstd_wake_count`
   to confirm the RTC alarm is actually firing on a board where
   the sub-clock crystal is alive.

No external hardware required.
