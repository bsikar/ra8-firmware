# clock_check

Brings the chip from the reset-default MOCO up to its rated CPUCLK0 through
`ra8_cgc_init()` -- HOCO start, PLL1 lock, clock mux, peripheral dividers --
then walks a table of clock IDs (CPUCLK0, ICLK, PCLKA..E, FCLK, MRICLK) asking
`ra8_cgc_get_clock_hz` for each and comparing it against the frequency the
driver was supposed to program. `g_clock_check_match` advances per agreeing
sweep, `g_clock_check_mismatch` on any disagreement or read error.

It also programs SysTick from the *reported* rate and blinks the LEDs at 1 Hz,
which is a second and independent check. If the reload is computed against the
wrong source clock or with a truncating divide, the error is not subtle -- it is
a factor of a hundred, and the LEDs visibly flicker or freeze rather than
ticking once a second on a stopwatch. Everything downstream that needs a baud
divisor or a timer period depends on this readback being honest.

Not covered here: TrustZone partitioning, bus-fabric ECC, and the MPU and caches
(`cache_mpu_hil` is the app for those).

When `ra8_cgc_init` faults during bring-up, the causes in order of likelihood
are: a PRCR-protected SYSC register written without unlocking `SYSC.PRCR` first;
a PLL1 stabilisation timeout because the external crystal is not running (check
`SYSC.MOSCCR.MOSTP` and the OFS option byte's oscillator-enable); or
`SYSC.SCKSCR` switching the mux to a clock that has not stabilised yet (check
the write order against `SYSC.OSCSF`).

LEDs map to P600 / P303 / PA07 per EK-RA8D2 v1 UM (R20UT5523EG0101) Table 24
p 31; the peripheral is HUM R01UH1065EJ0130 Ch 8 "Clock Generation Circuit
(CGC)".
