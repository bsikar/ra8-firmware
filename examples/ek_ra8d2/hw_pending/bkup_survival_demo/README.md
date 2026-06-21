# bkup_survival_demo

VBATT backup-register read/write window + reset-survival demo for the bare
EK-RA8D2 EVM. Exercises the 128-byte `VBTBKRn` backup store (`ra_bkup`).

## What it does

Brings up SCI8 + LEDs, then:

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

## Why this is in hw_pending

`tools/board_sim` now models the `VBTBKRn` window as a reset-retained
domain (`tools/board_sim/src/board_periph_bkup.c`): the backup bytes live
in a buffer whose reset hook deliberately leaves them untouched, so the
read/write half passes headlessly (`g_bkup_rw_ok = 1`, banner `rw=ok`).
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

## Registers (HUM R01UH1065EJ0130 Rev.1.30, Ch 12 "Battery Backup")

- 32 x 32-bit backup words `VBTBKRn` at R_SYSTEM + 0xD00
  (HUM Ch 12.2.7 "VBTBKRn" p 505); accessed via `ra_bkup_read_word` /
  `ra_bkup_write_word` (indices 0..31).

## On-silicon bench plan

1. `make bkup_survival_demo`, then flash the EK-RA8D2.
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
make -C examples/ek_ra8d2/hw_pending/bkup_survival_demo flash
```
