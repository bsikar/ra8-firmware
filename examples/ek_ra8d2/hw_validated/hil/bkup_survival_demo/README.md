# bkup_survival_demo

VBATT backup-register read/write window + reset-survival demo for the bare
EK-RA8D2 EVM. Exercises the 128-byte `VBTBKRn` backup store (`ra8_bkup`).

## What it does

Brings up SCI8 + LEDs, then **arms the backup-register access window**
(`ra8_bkup_init` with `enable_backup = true`, which writes 1 to `VBTBER.VBAE`
and waits >= 500 ns per HUM Ch 12.2.6 p 504). The block also needs voltage
monitor 0 (LVD0) enabled via the `OFS1.PVDAS` option byte -- see
[Root cause](#root-cause--silicon-status-131) below. Then:

1. **Read/write window** -- fills backup words 0..29 with a deterministic
   pattern (`(i * 0x01010101) ^ 0xA5A5A5A5`), reads them back, and
   compares (`g_bkup_rw_ok`).
2. **Reset survival** -- word 30 holds a sentinel (`0x600DCAFE`) and word
   31 a boot counter. On a cold boot the sentinel is absent, so it is
   planted and the counter set to 1 (`g_bkup_survived = 0`). After a
   **reset** the sentinel is still there, the counter increments, and
   `g_bkup_survived = 1` -- proving the VBATT domain retained state.

Each second it prints e.g. `bkup: rw=ok survived=Y boot=3` on the J-Link
OB CDC channel. LED1 toggles while the rw window is healthy; LED2 toggles
on a mismatch. `g_bkup_rw_ok` / `g_bkup_survived` / `g_bkup_boot_count` /
`g_bkup_heartbeat` mirror the result for headless J-Link probing.

No external hardware required (VBATT is tied to VCC on the EVM, so the
backup domain is always powered; a coin cell on VBATT would extend
survival across a full power cycle too).

## Root cause / silicon status (#131)

On real EK-RA8D2 silicon the read/write window fails (`bkup: rw=BAD`). The
root cause is that the VBATT battery-backup function is inoperative unless
**voltage monitor 0 (LVD0) reset is enabled**:

- HUM Ch 12.1.3 p 499: "It is necessary to enable voltage monitor 0 resets
  to use the battery backup function."
- HUM Ch 12.3.2 p 514: "The battery backup function should be used after the
  voltage monitoring 0 reset is enabled (`OFS1.PVDAS` bit is 0)."

With the repo-default `OFS1 = 0xFFFFFFFF` (`PVDAS = 1`, LVD0 off) the whole
VBATT area is held in `VBATT_POR` reset. Bench debugger reads confirm it:
`VBTBPSR = 0x31` (`VBPORF = 1`), and a J-Link write of `0xDEADBEEF` to
`VBTBKR0` reads back `0x00000000` -- writes to `VBTBPCR1` / `VBTBKRn` are
dropped even with `VBTBER.VBAE = 1` (VBAE resets to 1, so it was never the
gate). `tools/board_sim` models `VBTBKRn` as plain retained RAM with no
option memory, so the sim reports `rw=ok` while the bench reports `rw=BAD` --
the classic board_sim-masks-silicon pattern.

**Fix:** enable LVD0 by setting `OFS1 = 0xFFFFFFF0` (`PVDAS = 0`, `VDSEL0 =
000` = 2.85 V: below VCC 3.3 V so no spurious reset, above the 2.80 V
`VDETBATT` switch level per HUM Ch 12.3.2). This is wired in `CMakeLists.txt`
as a per-app `OFS1` / `OFS1_SEC` option-byte override.

**On-silicon verification is BLOCKED on option-byte programming.** The
option bytes live in the `.option_setting_*` sections at `0x0300A100+`.
`scripts/hil/flash.sh` deliberately strips them (J-Link RAMCode times out on
option bytes), `rfp-cli -p` rejects the region (`E3000110: operation not
supported`), and `JLinkExe loadfile` fails (`Writing target memory failed`)
because the board is in the Secure Debug (SSD) DLM state, which restricts
option-byte programming. Flipping `OFS1.PVDAS` on this board therefore needs
a full-image / DLM-authenticated option-byte flash that the current bench
tooling does not provide. The root cause + fix are proven; the final
`rw=ok` on silicon awaits that flash path.

## Why this is in hw_pending

`tools/board_sim` models the `VBTBKRn` window as a reset-retained domain
(`tools/board_sim/src/board_periph_bkup.c`): the backup bytes live in a
buffer whose reset hook deliberately leaves them untouched, and writes are
**gated on `VBTBER.VBAE`** (HUM Ch 12.2.6 p 504) so the read/write half
passes headlessly (`g_bkup_rw_ok = 1`, banner `rw=ok`) only because the demo
arms VBAE first -- a firmware that forgot the enable now reports `rw=BAD` on
the sim too. The sim cannot model the `OFS1`/LVD0 option-byte prerequisite
(no option memory in the emulator), which is why the sim passes while the
bench fails (see [Root cause](#root-cause--silicon-status-131)).
board_sim also has a **warm-reboot** capability (`--reboot N`) that re-runs
the firmware from its reset vector with that domain retained, so:

```
board_sim bkup_survival_demo.elf --reboot 1
```

plants the sentinel on the first boot, warm-reboots, and the second boot
finds it intact -- `g_bkup_survived = 1`, banner `survived=Y` -- which the
`board_sim_smoke.sh` gate asserts. The app stays in `hw_pending/` until
reset survival is confirmed on silicon (the simulator proves the VBTBKRn
read / write and the reset-retention contract, not the real battery-backed
SRAM cell).

## Registers / option bytes (HUM R01UH1065EJ0130 Rev.1.30)

- `OFS1.PVDAS` option byte (HUM Ch 7 p 284) -- 0 enables voltage monitor 0
  (LVD0) reset, the prerequisite for the battery backup function (Ch 12.1.3
  p 499, Ch 12.3.2 p 514). Set via `OFS1 = 0xFFFFFFF0` in `CMakeLists.txt`.
- `VBTBER.VBAE` access-enable at R_SYSTEM + 0xC40 (HUM Ch 12.2.6
  "VBTBER" p 504) -- write 1 + wait >= 500 ns before any `VBTBKRn` access;
  set by `ra8_bkup_init`.
- 32 x 32-bit backup words `VBTBKRn` at R_SYSTEM + 0xD00
  (HUM Ch 12.2.7 "VBTBKRn" p 505); accessed via `ra8_bkup_read_word` /
  `ra8_bkup_write_word` (indices 0..31).

## On-silicon bench plan

1. `make bkup_survival_demo`, then flash the EK-RA8D2 **including the option
   bytes** so `OFS1.PVDAS = 0` (LVD0) reaches silicon -- `scripts/hil/flash.sh`
   strips `.option_setting_*`, so use a full-image / option-byte flash (e.g.
   Renesas Flash Programmer or e2 studio) for the `OFS1` word. Without this
   the window stays `rw=BAD` regardless of firmware (see Root cause above).
2. Open the J-Link OB CDC channel at 115200 8N1. The **first** boot prints
   `bkup: rw=ok survived=N boot=1` (cold boot plants the sentinel).
3. Press the reset button (or issue a J-Link reset). The next boot must
   print `bkup: rw=ok survived=Y boot=2`, and the count must increment on
   each subsequent reset -- this is the reset-survival pass.
4. Or probe headless over SWD across a reset: `g_bkup_rw_ok == 1`,
   `g_bkup_survived == 1`, `g_bkup_boot_count` advancing.
5. Once green, move the app to `hw_validated/hil/` (the `hil.conf` keys on
   the post-reset `survived=Y` banner).

Build / flash:

```
make bkup_survival_demo
make -C examples/ek_ra8d2/hw_validated/hil/bkup_survival_demo flash
```
