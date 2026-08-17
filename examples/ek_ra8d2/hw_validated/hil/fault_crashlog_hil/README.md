# fault_crashlog_hil

Proof of the cross-reset crash log and reset-loop guard (`ra8_crashlog`): a
decoded fault snapshot is persisted into a `.noinit` record the reset handler
does not zero, crash-reboot cycles are counted, and a safe-mode request latches
once the count crosses a threshold. It is the persistence sibling of
`fault_div0_hil` -- that app provokes and decodes a real CPU fault, this one is
about surviving one across a reset.

It installs the exception-persist hook so any later real fault is copied into
the record before the CPU halts, peeks whatever a previous boot left behind
(reporting the exception, PC and boot-loop count, and dropping into safe mode
plus a `ra8_crashlog_claim()` if the count crossed the threshold), then
synthesises a decoded snapshot, pushes it through the installed write path and
reads it back. The console is registered as the `ra8_log` sink so a real fault
dump would be visible on the bench UART.

## What survives which reset class

The record lives in a dedicated NOLOAD `NOINIT` region carved from the top of
SRAM, directly above the main stack, so the reset handler's `.bss` zero-fill
never touches it. It survives any reset that keeps SRAM powered, and not a power
cut:

| Reset class                         | SRAM kept | Record survives |
|-------------------------------------|-----------|-----------------|
| Watchdog / IWDT underflow (warm)    | yes       | yes             |
| Software reset (SYSRESETREQ)        | yes       | yes             |
| Pin / debugger reset (no power cut) | yes       | yes             |
| Power-on reset / LVD brown-out      | no        | no (fail-safe)  |

A cold power-on randomises SRAM; the record's magic and CRC then fail validation
and the log reads empty. That is the intended fail-safe, so garbage SRAM can
never be mistaken for a post-mortem. Surviving a full power loss is out of
scope -- VBATT-backed or MRAM persistence via `ra8_bkup` is the named follow-up
and is silicon-blocked (#131).

Two legs are proven outside this app. The fill / validate / claim / loop-counter
/ threshold / corrupted-magic lifecycle lives in `tests/test_ra8_crashlog.c`.
Real cross-reset survival, with the boot-loop count climbing toward safe mode,
is bench-only and needs a reset that keeps power on -- a J-Link reset or a
watchdog underflow -- because the emulator cold-loads the image every run and a
power-cycle randomises SRAM by design.
