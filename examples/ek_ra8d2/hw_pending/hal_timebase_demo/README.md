# hal_timebase_demo

Demonstrates the `ra8_core` SysTick + DWT cycle-counter timebase primitive
(`libs/ra8_core/inc/ra8_systick.h`) on the EK-RA8D2, **alongside** the existing
`ra8_time` millisecond-tick path, so the two can be diffed on the bench
(issue #582). Both now share the same primitive; the demo confirms it on silicon.

## What it shows

`ra8_time.c` (in `ra8_core`) has historically programmed SysTick (`SYST_CSR` /
`SYST_RVR` / `SYST_CVR`) and the DWT cycle counter (`DEMCR.TRCENA` /
`DWT_CTRL.CYCCNTENA` / `DWT_CYCCNT`) through its own private address-cast
accessors, and the ThreadX port re-programmed the same SysTick independently.
The `ra8_systick` primitive collects those exact accesses behind one
`ra8_core` driver (in `ra8_core`, not `ra8_hal`, so the Ring-1 `ra8_time`
consumer can include it without an upward layering dependency):

| Primitive | Register(s) |
|---|---|
| `ra8_systick_reload_for()` | pure arithmetic: `cpu_hz / tick_hz - 1`, 24-bit range-checked |
| `ra8_systick_configure()` | `SYST_CSR` / `SYST_RVR` / `SYST_CVR` -- arm + start |
| `ra8_systick_set_reload()` | `SYST_RVR` + `SYST_CVR` -- re-arm, leave `SYST_CSR` |
| `ra8_systick_current_value()` | `SYST_CVR` -- live down-counter |
| `ra8_dwt_cyccnt_enable()` | `DEMCR.TRCENA` + `DWT_CTRL.CYCCNTENA` |
| `ra8_dwt_cyccnt_reset()` / `ra8_dwt_cyccnt_read()` | `DWT_CYCCNT` |

These are Arm v8-M **architectural** registers, so they carry Arm-architecture
references rather than Hardware User's Manual citations.

## Sequence

1. `ra8_cgc_init()` -- XTAL + PLL1 up, CPUCLK0 = 1 GHz.
2. `ra8_time_init(cpuclk0_hz)` -- the `ra8_time` tick path arms SysTick + DWT
   (now through the same primitive) and backs `ra8_delay_ms()`.
3. `ra8_board_uart_console_init(115200)` -- SCI8 J-Link OB console.
4. `ra8_dwt_cyccnt_enable()` -- the primitive owns the DWT enable.
5. `ra8_systick_reload_for(cpuclk0_hz, 1000, &reload)` -- print `reload=999999`.
6. `ra8_systick_configure(reload, k_ra8_systick_clk_cpu, true)` -- arm the
   **same** SysTick directly via the primitive (same reload + control bits
   `ra8_time_init` programmed).
7. Loop: reset DWT, `ra8_delay_ms(1000)`, read DWT, print
   `hal timebase ms=1000 cycles=<n> us=<n>`, toggle LED1.

## Wiring / build / run

Stock EK-RA8D2, no extra hardware. Console is the on-board J-Link OB CDC port
(TXD8 = PD_02 / RXD8 = PD_03) at 115200 8N1.

```sh
make            # cross-compile build/hal_timebase_demo.elf / .hex
make flash      # program via J-Link
```

Open the CDC port at 115200 8N1, e.g. `picocom -b 115200 /dev/cu.usbmodem...`
(macOS) or `minicom -D /dev/ttyACM0 -b 115200` (Linux). Expect one
`reload=999999` line, then a `hal timebase ms=1000 cycles=<n> us=<n>` line per
second with LED1 toggling. On a 1 GHz core the derived microseconds land near
`1000000` for a 1000 ms delay.

## What to confirm on silicon

The printed `reload` is the value `ra8_systick_reload_for()` derives;
`ra8_time_init()` programs `SYST_RVR` with the same value through the same
primitive, so a debugger read of `SYST_RVR` must match it. The printed `cycles`
is the DWT-measured length of `ra8_delay_ms(1000)` (which now reads the DWT
counter through the primitive); it must be ~1 000 000 000 cycles at 1 GHz. This
is the on-silicon evidence that the primitive programs the timebase correctly
and `ra8_delay_ms` remains accurate after the refactor.

## Status

`hw_pending`: written and cross-build-verified, not yet bench-validated. Once
confirmed on the rig it can be promoted under `hw_validated/hil/`.
