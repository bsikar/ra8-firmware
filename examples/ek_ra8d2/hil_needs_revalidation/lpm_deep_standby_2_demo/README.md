# lpm_deep_standby_2_demo

Deep Software Standby variant 2 (LPSCR.LPMD = 0x9) entry demo for
the bare EK-RA8D2 EVM.

Variant 2 stops the voltage-monitor and sub-clock-detection
domains that variant 1 (`lpm_deep_standby_1_demo`) leaves running
(HUM Ch 11.1 Table 11.3 p 431..432), trading wake-up coverage for
additional standby current savings. Variant 3
(`lpm_deep_standby_3_demo`) stops the LOCO on top of that for the
deepest variant available.

## What it does

1. CGC + SysTick + SCI8 + RTC + LPM bring-up.
2. Emit boot banner `lpm_dpsby2: boot` over SCI8.
3. Seed the RTC and arm a +5 s alarm.
4. Programme the deep-standby cancel matrix:
   - DPSIER2.DRTCAIE = 1 (RTC alarm cancels deep standby)
   - WUPEN0.RTCALMWUPEN = 1 (alarm armed in the wake-up matrix)
5. `ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_standby_2)`.
6. On wake the chip RESETS -- control lands back in
   `Reset_Handler` and step 1 repeats.

## HIL gate

`HIL_MODE=uart_scrape` on `lpm_dpsby2: boot`. The deep-standby wake
path is reset-based, so on a working EVM the banner re-emits every
~5 seconds. On a chip with a dead sub-clock crystal the banner
emits exactly once before WFI hangs forever.

## SOSC caveat

Identical to `lpm_software_standby_demo`. The RTC alarm runs off
SOSC, and SOSC is intermittent on this EVM. The HIL gate is
boot-banner-only so the firmware build + bring-up + standby-entry
path is verified even when the crystal is silent. The actual wake
behaviour needs benchwork to confirm.

## Bench verification

1. `make lpm_deep_standby_2_demo`
2. Flash via the Pi-bound HIL flasher: `bash scripts/hil/flash.sh
   examples/ek_ra8d2/hil_needs_revalidation/lpm_deep_standby_2_demo/build/lpm_deep_standby_2_demo.hex`
3. Run the HIL gate from the repo root: `bash scripts/hil/run.sh
   lpm_deep_standby_2_demo`
4. If SOSC is alive, watch the banner repeat at ~0.2 Hz on the
   J-Link CDC console.

No external hardware required.
