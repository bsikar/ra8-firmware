# clock_check

CGC bring-up HIL test for the EK-RA8D2. Brings the chip from
reset-default MOCO (~8.4 MHz) up to its rated CPUCLK0 via the CGC
driver, programmes SysTick against the new clock rate, and toggles
the user LEDs at 1 Hz so you can stopwatch-verify the clock came up
where the driver thinks it did.

## What it tests

- `ra8_cgc_init()` end-to-end on real silicon: HOCO start, PLL1 lock,
  CPUCLK0 mux, peripheral-clock dividers.
- `ra8_cgc_get_clock_hz()` returns a sane CPUCLK0 value (matches the
  blink rate observation).
- `ra8_time_init()` arithmetic at the new clock rate -- if the SysTick
  reload is computed against the wrong PCLK source or with a
  truncating divide, `ra8_delay_ms(500)` is wildly wrong (off by 100x,
  not 1%) and the LEDs flicker or freeze instead of doing a clean 1 Hz.

## Build + flash

From the repo root:

```sh
make clock_check                       # cross-compile -> examples/ek_ra8d2/hw_validated/hil/clock_check/build/clock_check.elf
make -C examples/clock_check flash     # flash via on-board J-Link OB
```

Or standalone, from inside `examples/ek_ra8d2/hw_validated/hil/clock_check/`:

```sh
cd examples/ek_ra8d2/hw_validated/hil/clock_check/
make
make flash
make clean
```

## Pass / fail

| What you see | Verdict |
|---|---|
| LEDs toggle once per second on a stopwatch | CGC + SysTick are healthy |
| LEDs toggle but at ~10 Hz or 0.1 Hz | CGC reports a wrong CPUCLK0; check `ra8_cgc_get_clock_hz` math vs `ra8_time_init` arithmetic |
| LEDs never light, board hangs | `ra8_cgc_init()` HardFaults during PLL bring-up; attach Ozone (`make -C examples/clock_check ozone`) and inspect SCB.HFSR / CFSR + SYSC.LOCKE |
| LEDs light once and freeze | SysTick reload overflow at the new clock; the 24-bit reload + chunk-loop in `ra8_time.c` should handle it but the boundary is worth checking |

## What this does NOT test

- Trustzone / SAU partition (still skipped, as in `blink_hal`).
- Bus-fabric ECC, MPU, or cache (still off; those caused the
  HardFaults that got `system_init.c` skipping cache + MPU + TZ in
  the first place).
- Anything beyond CPUCLK0 -- once `clock_check` is green the next
  step is `uart_hello`, which exercises PCLKB and the SCI baud
  divisor too.

## Debugging

```sh
make -C examples/clock_check ozone   # SEGGER Ozone GUI
make -C examples/clock_check debug   # gdb attached via JLinkGDBServer
```

If `ra8_cgc_init` HardFaults, the most common causes (in order):
1. PRCR write protection wasn't unlocked before touching the SYSC
   register being modified -- check `SYSC.PRCR` writes in
   `ra8_cgc.c`.
2. PLL1 stabilisation timeout because the input crystal wasn't
   running -- check `SYSC.MOSCCR.MOSTP` and the OFS option byte's
   external-oscillator-enable.
3. Clock mux switched to a clock that hasn't stabilised yet --
   check `SYSC.SCKSCR` write order vs. `SYSC.OSCSF`.

## BSP usage

Uses `ra8_board_ek_ra8d2` BSP for LED1/LED2/LED3 init/toggle (LEDs map
to P600 / P303 / PA07 per EK-RA8D2 v1 UM Table 24 p 31).

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Table 24 p 31, and HUM (R01UH1065EJ0130) Ch 8 "Clock
Generation Circuit (CGC)".
