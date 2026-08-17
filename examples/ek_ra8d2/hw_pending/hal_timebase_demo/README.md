# hal_timebase_demo

Exercises the `ra8_systick` SysTick + DWT cycle-counter primitive **alongside**
the `ra8_time` millisecond-tick path, so the two can be diffed on the bench
(#582). Both now share the same primitive; this demo is the on-silicon
confirmation.

`ra8_time` historically programmed SysTick (`SYST_CSR` / `SYST_RVR` / `SYST_CVR`)
and the DWT cycle counter (`DEMCR.TRCENA` / `DWT_CTRL.CYCCNTENA` / `DWT_CYCCNT`)
through its own private address-cast accessors, and the ThreadX port
re-programmed the same SysTick independently. The primitive collects those exact
accesses behind one driver. It lives in **`ra8_core`, not `ra8_hal`**, so the
Ring-1 `ra8_time` consumer can include it without an upward layering dependency.
These are Arm v8-M architectural registers, so they carry Arm-architecture
references rather than Hardware User's Manual citations.

The app prints the reload value `ra8_systick_reload_for()` derives, then arms the
same SysTick directly through the primitive and, once a second, brackets a
one-second `ra8_delay_ms()` with a DWT reset and read.

## What to confirm on silicon

`ra8_time_init()` programs `SYST_RVR` with the same value through the same
primitive, so a debugger read of `SYST_RVR` must match the printed reload. The
printed cycle count is the DWT-measured length of a one-second delay and must
land near one billion cycles at 1 GHz. Together those are the evidence that the
primitive programs the timebase correctly and that `ra8_delay_ms` stayed accurate
across the refactor. Stock EK-RA8D2, no extra hardware.
