# sdhi_card_demo

Native **4-bit SDHI** microSD bring-up + FAT round-trip for the EK-RA8D2 (#123).
The SDHI counterpart to the SPI-mode SD apps (`tz_secure_only_sd`,
`sd_font_render`), which clock the card over an SCI Simple-SPI bus.

## What it does

Drives the on-board microSD through the dedicated **SDHI** controller (not
SPI), then mounts it and round-trips a file:

1. CGC + SysTick + SCI8 + LEDs + MSTP.
2. Routes the eight SDHI pins (port 4, pins 0..7: CMD / CLK / DAT0..3 / WP / CD)
   to `PSEL = k_ra_psel_sdhi`.
3. `ra_sdhi_init(0)` + `ra_sdcard_init({.instance = 0})` -- the full SD Physical
   Layer identification (CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3 -> CMD9 -> CMD7)
   and the clock step-up from the 400 kHz ident rate to default speed, all in
   `ra_sdcard` / `ra_sdhi`.
4. Binds `ra_sdcard_read_blocks` / `ra_sdcard_write_blocks` /
   `ra_sdcard_get_capacity` into an `ra_fs_backend_t` and `ra_fs_mount`s the FAT
   volume.
5. Writes a fixed-seed 512-byte payload to `TEST.TXT`, reads it back, compares.

On success: `sdhi: roundtrip ok`. `g_sdhi_stage` / `g_sdhi_ok` / `g_sdhi_blocks`
/ `g_sdhi_heartbeat` mirror the result for headless probing. LED1 toggles while
healthy; LED2 on a fault.

No external hardware beyond a FAT-formatted microSD in the on-board slot.

## Why this is in hw_pending

`tools/board_sim` does **not** model the SDHI controller or a card on it, so
the identification sequence times out on the emulator (`SD_INFO1.RSPEND` never
asserts) and the demo stops at `g_sdhi_stage = cardinit`, printing
`sdhi: card init failed (needs silicon+card)`. It runs **without faulting** on
the unmodeled SDHI register window (verified -- the `ra_sdcard` CMD0/CMD8
sequence drives `0x4025_20xx` and degrades gracefully), but cannot reach
`roundtrip ok` without a real card. The payload pattern + round-trip verdict
(MC/DC) are host-tested; the mount + read/write round-trip needs silicon, so
the app stays in `hw_pending/`.

## Registers (HUM R01UH1065EJ0130 Rev.1.30, Ch 47 "SDHI")

- SD command sequencing via `SD_CMD` / `SD_ARG` / `SD_INFO1.RSPEND`
  (HUM Ch 47.2.1 "SD_CMD : Command Type Register"); identification + block I/O
  live in `ra_sdcard` / `ra_sdhi`.
- SDHI pin map: port 4, pins 0..7 -> `PSEL = k_ra_psel_sdhi` (RA8D2 datasheet
  R01DS0493EJ "Pin Functions" -> "SDHI").

## On-silicon bench plan

1. Insert a FAT-formatted microSD into the on-board slot.
2. `make sdhi_card_demo`, then flash the EK-RA8D2.
3. Confirm `sdhi: roundtrip ok` (`g_sdhi_ok == 1`, `g_sdhi_stage == 4`,
   `g_sdhi_blocks` == the card's 512-byte block count).
4. (Optional) Measure SDHI 4-bit throughput vs the SPI-mode `sd_font_render`
   path for the perf note in #123.
5. Once the round-trip is confirmed on the bench, move the app to
   `hw_validated/hil/`.

Build / flash:

```
make sdhi_card_demo
make -C examples/ek_ra8d2/hw_pending/sdhi_card_demo flash
```
