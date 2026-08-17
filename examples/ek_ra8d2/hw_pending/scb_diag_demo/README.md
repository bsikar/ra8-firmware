# scb_diag_demo

Drives the `ra8_scb` driver -- the one abstraction over the Arm v8-M System
Control Block (PPB window `0xE000ED00`) that the exception decoder, the DFU
copy-to-run launcher and the ITM log transport otherwise reach raw -- and prints
what those call sites read for themselves, so the HAL primitive can be diffed
against them on the bench (#583).

Once per second it:

1. Queries the vector-table base with `ra8_scb_get_vtor()`, the primitive the DFU
   launcher writes to relocate the table. It only **queries** VTOR; it never
   relocates the live table.
2. Powers the trace block up with `ra8_scb_trace_enable()` (DEMCR.TRCENA, the bit
   the log transport pre-checks) and confirms the enable reads back set.
3. Reads one fault-status snapshot -- CFSR / HFSR / DFSR / MMFAR / BFAR / AFSR
   plus the Secure SFSR / SFAR pair, the set the exception decoder reads -- and
   logs every register.

On a clean boot with no fault injected, every fault-status register reads zero.
Bare EK-RA8D2, no expansion board.
