# fault_div0_hil

Proof that `CCR.DIV_0_TRP`, set by the shared boot, turns an integer divide by
zero into a decoded UsageFault instead of the ARM-default silent quotient of
zero. The app reads `CCR` back first -- so a boot that stopped arming the trap
fails loudly rather than appearing to pass -- then executes a guarded run-time
`UDIV` by zero.

The divide never completes. The CPU takes UsageFault, the per-app vector-table
trampoline forwards the stacked frame into `ra8_exception_report()`, and the
decoded dump prints before the CPU parks in `ra8_exception_halt_loop`. What
matters in that dump is the true UsageFault class rather than an anonymous
HardFault, with `CFSR.DIVBYZERO` (`0x02000000`) set. The full snapshot -- frame,
CFSR, HFSR, BFAR, MMFAR, SFSR, SFAR -- also lands in `g_ra8_exception_last`
(magic `0xFA17DEAD`) for a post-mortem J-Link attach.

The console is registered as the `ra8_log` byte sink deliberately: the default
ITM log path drops every byte from fault context, so the dump would otherwise be
invisible on the bench.

`ra8_emulator` reproduces the trap faithfully. Its CPU-model seam scans the
image for every `UDIV` and `SDIV` site and -- only after the firmware sets
`CCR.DIV_0_TRP`, watched through the SCB control-register write hook --
overwrites those sites with a `UDF`, then services the resulting trap as a
UsageFault latching `CFSR.DIVBYZERO`. An emulator run therefore reaches the same
verdict as the bench, with the same arm-then-fault ordering.
