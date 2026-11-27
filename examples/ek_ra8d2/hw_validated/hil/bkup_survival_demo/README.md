# bkup_survival_demo

Exercises the 128-byte VBATT backup-register store (`VBTBKRn`, via `ra8_bkup`):
it writes a pattern across the backup words and reads it back, and it plants a
sentinel plus a boot counter that has to survive a reset. A counter that keeps
climbing across resets is the proof that the VBATT domain retained state.
`g_bkup_rw_ok` / `g_bkup_survived` / `g_bkup_boot_count` mirror the result for a
headless J-Link probe.

VBATT is tied to VCC on the EVM, so the backup domain is always powered and the
demo needs no external hardware. A coin cell on VBATT would extend survival
across a full power cycle too.

## Hardware-verified two-boot survival (PRCR.PRC1 + SYSRESETREQ)

Early failures (`rw=BAD`) came from `libs/ra8_hal/src/ra8_bkup.c` issuing
writes while SYSC `PRCR.PRC1` was locked (HUM Ch 13.1 Table 13.1 p 520-521);
a write to a protected `VBT*` register while its group is locked is dropped
silently. The old `OFS1.PVDAS` option-byte hypothesis is superseded: EK-RA8D2
ties `VBATT` to `VCC`, and `VBTBKRn` is writable and retained once `PRC1` is
unlocked and `VBTBER.VBAE = 1` is set with the >= 500 ns settle.

On Boot 1 (`cause != k_ra8_reset_cause_software`), the demo verifies words
`0..29`, plants the sentinel (`0x600DCAFE`) and `boot=1`, prints
`bkup: rw=ok survived=N boot=1`, and issues `ra8_reset_software_reset()`.
On Boot 2 (`k_ra8_reset_cause_software`), it observes the retained sentinel,
increments `boot=2`, and emits `bkup: rw=ok survived=Y boot=2` at 1 Hz.

`tools/ra8_emulator` also models `PRCR.PRC1` gating and retains `VBTBKRn`
across `AIRCR.SYSRESETREQ`, matching EK-RA8D2 silicon across both boots.

## Registers (HUM R01UH1065EJ0130 Rev.1.30)

- `SYSC.PRCR.PRC1` (Ch 13.1 Table 13.1 p 520-521) -- unlock required for
  `VBT*` register writes.
- `VBTBER.VBAE` access enable at R_SYSTEM + 0xC40 (Ch 12.2.6 p 504) -- write 1
  and wait at least 500 ns before any `VBTBKRn` access.
- The 32 backup words `VBTBKRn` at R_SYSTEM + 0xD00 (Ch 12.2.7 p 505).
