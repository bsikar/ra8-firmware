# cac_accuracy_demo

Clock Frequency Accuracy Measurement Circuit (CAC) demo for the bare
EK-RA8D2 EVM. Measures the 24 MHz main oscillator against the LOCO
low-speed oscillator and flags a frequency error if the crystal is off.

## What it does

Brings up SCI8 + LEDs, programmes the CAC, and once a second:

1. Counts edges of the **main oscillator** (CACMCLK, 24 MHz) during one
   period of the **LOCO reference** (CACLCLK, 32768 Hz) divided by 32 ->
   a 1024 Hz window.
2. Expected count = `24e6 / 1024 = 23437`. The allowable window is set to
   `23437 +/- ~6%`, so a healthy crystal passes (no FERRF) while a stopped
   or grossly detuned oscillator trips the frequency-error flag.
3. Reports `cac: meas=ok ferr=0 ovf=0 ok=Y` on the J-Link OB CDC channel.
   The raw count lands in `g_cac_count`.

- LED1 toggles on a healthy measurement; LED2 toggles on timeout / error.
- `g_cac_count` / `g_cac_ok` / `g_cac_status` / `g_cac_heartbeat` mirror
  the result for headless J-Link probing.

No external hardware required. Requires the LOCO to be running (the
default low-speed clock, also feeding IWDT / RTC); if the LOCO is stopped
the reference never ticks and the measurement times out.

## Validation

Confirmed on a real EK-RA8D2 (2026-06-28): the real cross-clock edge count
of the 24 MHz main oscillator against LOCO/32 lands inside the +/-6% window
(`FERRF` / `OVFF` clear), so the gate is green (`cac: meas=ok ferr=0 ovf=0
ok=Y`).

`tools/board_sim` also models the CAC edge counter
(`tools/board_sim/src/periph/board_periph_cac.c`). A measurement start
(`CACR0.CFME = 1`) latches `CASTR.MENDF` and loads `CACNTBR` with the
midpoint of the firmware's programmed `[CALLVR, CAULVR]` window -- in-band
by construction -- so `ra8_cac_measure` completes with `FERRF` / `OVFF`
clear and the headless `board_sim_smoke.sh` gate sees the same `meas=ok ...
ok=Y` banner (the simulator proves the driver start / poll / read-back
sequence; silicon proves the real edge count).

## Configuration (HUM R01UH1065EJ0130 Rev.1.30, Ch 10 "CAC")

- `ra8_cac_init(upper, lower)` loads the +/-6% window into CAULVR / CALLVR
  (HUM Ch 10.2.6 / 10.2.7 p 425) and clears CACR0.CFME.
- CACR1 = `0x00`: target = main osc (FMCS = 000), no division, rising edge
  (HUM Ch 10.2.2 "CACR1" p 421).
- CACR2 = `0x09`: internal reference (RPS = 1), reference = LOCO
  (RSCS = 100) divided by 32 (RCDS = 00), no digital filter
  (HUM Ch 10.2.3 "CACR2" p 422).
- `ra8_cac_measure` sets CACR0.CFME = 1 and polls CASTR.MENDF
  (HUM Ch 10.2.1 p 421 / 10.2.5 p 424); the count is read from CACNTBR.

> Note: the `ra8_cac` HAL takes only the count limits and leaves CACR1 /
> CACR2 = 0 (a degenerate main-vs-main pairing). This demo selects the
> real MAIN-vs-LOCO pair by writing CACR1 / CACR2 directly. A future HAL
> enhancement could fold clock-source selection into `ra8_cac_init`.

## On-silicon bench plan

1. `make cac_accuracy_demo`, then flash the EK-RA8D2.
2. Open the J-Link OB CDC channel at 115200 8N1; expect
   `cac: meas=ok ferr=0 ovf=0 ok=Y` once a second with LED1 toggling.
3. Or probe headless over SWD: `g_cac_ok == 1`, `g_cac_count` near 23437
   (within +/-6%), `g_cac_status` MENDF set with FERRF / OVFF clear.
4. Once green, move the app to `hw_validated/hil/` and switch `hil.conf`
   to the active uart_scrape gate.

Build / flash:

```
make cac_accuracy_demo
make -C examples/ek_ra8d2/hw_validated/hil/cac_accuracy_demo flash
```
