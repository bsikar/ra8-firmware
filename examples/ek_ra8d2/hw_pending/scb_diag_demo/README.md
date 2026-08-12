# scb_diag_demo

Cortex-M85 System Control Block (SCB) HAL demonstration for the EK-RA8D2
(issue #583). It drives the `ra8_scb` driver -- the one abstraction over the
Arm v8-M SCB (PPB window `0xE000ED00`) that the exception decoder, the DFU
copy-to-run launcher, and the ITM log transport otherwise reach raw -- and
prints what those call sites read for themselves, so the HAL primitive can be
diffed against them on the bench.

## What it does

Once per second it:

1. Queries the vector-table base with `ra8_scb_get_vtor()` (the primitive the
   DFU launcher writes to relocate the table) and logs it as hex. It only
   **queries** VTOR -- it never relocates the live table.
2. Powers the trace block up with `ra8_scb_trace_enable()` (DEMCR.TRCENA, the
   bit the log transport pre-checks) and confirms `ra8_scb_trace_enabled()`
   reads back set.
3. Reads one fault-status snapshot with `ra8_scb_read_fault_status()` --
   CFSR / HFSR / DFSR / MMFAR / BFAR / AFSR plus the Secure SFSR / SFAR pair
   (the set the exception decoder reads) -- and logs every register as hex.

On a clean boot with no fault injected, every fault-status register reads zero,
and the demo emits the verdict line `scb: probe PASS`.

## Expected output

```
scb: vtor=0x02000000
scb: trace=0x00000001
scb: cfsr=0x00000000 hfsr=0x00000000
scb: dfsr=0x00000000 afsr=0x00000000
scb: mmfar=0x00000000 bfar=0x00000000
scb: sfsr=0x00000000 sfar=0x00000000
scb: probe PASS
```

## Hardware

Bare EK-RA8D2, no expansion board. Console is SCI8 on PD_02 / PD_03 routed to
the on-board J-Link OB VCOM at 115200 8N1.

## Build and run

```sh
make            # cross-compile build/scb_diag_demo.elf / .hex / .bin
make flash      # load via the on-board J-Link
```
