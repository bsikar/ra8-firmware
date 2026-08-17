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

## The battery-backup function is gated on LVD0, not on VBAE

The non-obvious prerequisite: the VBATT function is inoperative unless voltage
monitor 0 (LVD0) reset is enabled. HUM Ch 12.1.3 p 499 -- "It is necessary to
enable voltage monitor 0 resets to use the battery backup function" -- and
Ch 12.3.2 p 514. With the repo-default `OFS1 = 0xFFFFFFFF` (`PVDAS = 1`, LVD0
off) the whole VBATT area is held in `VBATT_POR` reset: `VBTBPSR.VBPORF` reads
set, and writes to `VBTBPCR1` / `VBTBKRn` are dropped from firmware and from a
debugger alike even with `VBTBER.VBAE = 1`. VBAE resets to 1, so it was never
the gate.

`CMakeLists.txt` therefore overrides the option byte to `OFS1 = 0xFFFFFFF0`
(`PVDAS = 0`, `VDSEL0 = 000` = 2.85 V: below VCC 3.3 V so no spurious reset,
above the 2.80 V `VDETBATT` switch level per HUM Ch 12.3.2).

That override cannot currently reach this board, which is the blocker (#131).
The option bytes live in the `.option_setting_*` sections at `0x0300A100+`; the
HIL flasher strips them because J-Link RAMCode times out on option bytes,
`rfp-cli -p` rejects the region, and `JLinkExe loadfile` fails because the board
sits in the Secure Debug (SSD) DLM state, which restricts option-byte
programming. Until a DLM-authenticated option-byte flash exists, the read/write
window reads bad on silicon whatever the firmware does.

An emulator will not reproduce any of this: modelling `VBTBKRn` as plain
retained RAM with no option memory at all makes the read/write half pass while
the bench fails.

## Registers (HUM R01UH1065EJ0130 Rev.1.30)

- `OFS1.PVDAS` option byte (Ch 7 p 284) -- 0 enables LVD0 reset, the
  prerequisite for battery backup (Ch 12.1.3 p 499, Ch 12.3.2 p 514).
- `VBTBER.VBAE` access enable at R_SYSTEM + 0xC40 (Ch 12.2.6 p 504) -- write 1
  and wait at least 500 ns before any `VBTBKRn` access.
- The 32 backup words `VBTBKRn` at R_SYSTEM + 0xD00 (Ch 12.2.7 p 505).
