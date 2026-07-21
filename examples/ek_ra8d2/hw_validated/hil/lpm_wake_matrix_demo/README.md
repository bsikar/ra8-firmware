# lpm_wake_matrix_demo

Exercises every WUPEN0 / WUPEN1 wake-source enable bit the LPM HAL
exposes, on real silicon. Useful as a smoke test for the
`ra8_lpm_arm_wupen0_bits` / `ra8_lpm_arm_wupen1_bits` /
`ra8_lpm_clear_wupen0_bits` / `ra8_lpm_clear_wupen1_bits` helpers and
the `ra8_lpm_get_exit_cause` packed-snapshot helper.

## What it does

1. CGC + SysTick + SCI8 + LED1 + LPM block bring-up.
2. Emit boot banner `lpm_wake_matrix: boot`.
3. Walk WUPEN0 internal-peripheral sources: IWDT / PVD1 / PVD2 /
   VBATT / RTC alarm / RTC periodic.
4. Walk WUPEN1 internal-peripheral sources: COMPHS0 / SOSC /
   ULPT0U / ULPT0A / ULPT0B / I3C0.
5. Disarm everything and confirm both WUPEN registers read zero.
6. Emit `lpm_wake_matrix: done` and park LED1 on.

The demo deliberately does NOT enter Software Standby / Deep
Standby. Most WUPEN sources need the underlying peripheral to be
armed and wired to external HW (USB device attached, IRQ pin
driven, ULPT armed, ...) which the bare EK-RA8D2 cannot synthesise
without a shield. The point of this demo is the HAL-level matrix
walk -- proving the WUPEN registers are reachable and read back as
written.

## HIL gate

`HIL_MODE=uart_scrape` on `lpm_wake_matrix: done`. The walk completes
in well under a second so the 15 s timeout is generous.

## Bench verification

1. `make lpm_wake_matrix_demo`
2. Flash via the Pi-bound HIL flasher: `bash scripts/hil/flash.sh
   examples/ek_ra8d2/hw_validated/hil/lpm_wake_matrix_demo/build/lpm_wake_matrix_demo.hex`
3. Run the HIL gate from the repo root: `bash scripts/hil/run.sh
   lpm_wake_matrix_demo`

The `g_lpm_wake_matrix_armed` symbol is also exposed for SWD
inspection -- attach the debugger and watch it advance through each
WUPEN0/WUPEN1 bit during the walk if you want a finer-grained trace.

No external hardware required.
