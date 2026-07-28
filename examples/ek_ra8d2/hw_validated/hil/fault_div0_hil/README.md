# fault_div0_hil

Proof that `CCR.DIV_0_TRP` (set by the shared boot in
`libs/ra8_board_ek_ra8d2/boot/system_init.c`) turns an integer divide by
zero into a **decoded UsageFault** instead of the ARM-default silent
quotient of 0.

## What it does

1. Brings up clocks + the SCI8 J-Link VCOM console (115200) and
   registers the console as the `ra8_log` byte sink, so the fault
   handler's dump is visible on the bench UART (the default ITM log
   path deliberately drops every byte from fault context).
2. Reads `CCR` back and prints `fault-div0: trap armed` when
   `DIV_0_TRP` is set (proves the boot write landed); a clear bit
   prints `fault-div0: FAIL trap not armed` and halts.
3. Executes a guarded volatile `100 / 0` (a run-time `UDIV`).

## Expected output on silicon

The divide never completes. The CPU takes UsageFault, the per-app
vector-table trampoline forwards the stacked frame into
`ra8_exception_report()`, and the decoded dump prints before the CPU
parks at `ra8_exception_halt_loop`:

```
fault-div0: boot
fault-div0: trap armed
[EXC] ERROR: exception=6
[EXC] ERROR: pc  =...
[EXC] ERROR: lr  =...
...
[EXC] ERROR: cfsr =33554432
...
```

`exception=6` is the true UsageFault class (not an anonymous
HardFault); `cfsr =33554432` is `0x02000000` = `CFSR.DIVBYZERO`. The
`hil.conf` gate scrapes the DIVBYZERO decode and rejects the
`survived divide` / `FAIL` lines.

The full snapshot (frame, CFSR/HFSR/BFAR/MMFAR/SFSR/SFAR) is also in
`g_ra8_exception_last` (magic `0xFA17DEAD`) for a post-mortem J-Link
attach.

## Validation

**Silicon (EK-RA8D2, J-Link):** the guarded divide raises UsageFault and
the shared decoder prints `[EXC] ERROR: exception=6` +
`[EXC] ERROR: cfsr =33554432` (`0x02000000` = `CFSR.DIVBYZERO`) on the
SCI8 VCOM console -- proof `CCR.DIV_0_TRP` is set (T2-01) and the frame
decoder runs on hardware (T2-02). Recorded on tracker issue #191.

**Emulator-in-the-loop (`scripts/emu/eil_all.sh`):** ra8_emulator models the
divide-by-zero trap faithfully. Its CPU-model seam scans the image for
every `UDIV`/`SDIV` site and -- **only after** the firmware sets
`CCR.DIV_0_TRP` (watched via the SCB control-register write hook) --
overwrites those sites with a `UDF`, then services the resulting trap as
a UsageFault latching `CFSR.DIVBYZERO`. An EIL run therefore reaches the
same `cfsr =33554432` gate the bench does, with the same arm-then-fault
ordering (a firmware that stopped arming the trap would instead print
`fault-div0: FAIL trap not armed` and halt, tripping the negative gate).

```
make -C examples/ek_ra8d2/hw_validated/hil/fault_div0_hil build flash
# scrape the SCI8 VCOM console for: cfsr =33554432
```
