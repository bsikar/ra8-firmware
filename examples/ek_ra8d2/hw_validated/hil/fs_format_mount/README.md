# fs_format_mount

Format + mount + file-ops HIL demo across every filesystem `ra8_fs` can write
(FAT12, FAT16, FAT32, and exFAT) on a real microSD card.

This app is the on-hardware exercise for the `ra8_fs_format()` mkfs API. It does
not assume any pre-formatted layout: it **reformats the card four times**, once
per filesystem type, and validates each format end to end.

## What it does

1. Bring CGC + SCI8 console + SCI0 Simple-SPI up.
2. Probe a Digilent PMOD MicroSD (part 410-380) plugged into Pmod2 (J25) and run
   the SD SPI-mode bring-up (CMD0, CMD8, ACMD41, CMD58, CMD9, CMD16).
3. For each filesystem in `{ FAT12, FAT16, FAT32, exFAT }`:
   1. `ra8_fs_format()` the card as that type (auto cluster size).
   2. `ra8_fs_mount()` it and assert the detected `ra8_fs_type` matches.
   3. Create + write a 1300-byte deterministic payload (`FMTTEST.BIN`).
   4. Read it back and byte-compare.
   5. Rename `FMTTEST.BIN` -> `FMTDONE.BIN`; the old name must be gone and the
      new name must read back intact.
   6. `unlink` `FMTDONE.BIN`.
   7. `unmount`.
   8. Print `fsfmt: FS <TYPE> FORMAT+MOUNT PASS`.
4. After all four, print `fsfmt: FS FORMAT+MOUNT ALL PASS`.

On any failure it prints `fsfmt: FS <TYPE> FAIL <step>` and parks the CPU.

> **Warning:** this app **erases the card**. All four filesystems run the
> identical create / write / read-back / rename / unlink cycle; the exFAT trial
> additionally asserts an empty root both right after format (the bitmap /
> up-case / volume-label system entries must stay hidden) and again after the
> unlink, proving the directory-set teardown fully reclaimed the entry.

## Hardware

| EK-RA8D2 pin | PMOD MicroSD signal | Net (UM Table 19 p 27) |
|--------------|---------------------|------------------------|
| Pmod2.1 P604 | CS                  | SSLB0 (GPIO output)    |
| Pmod2.2 P603 | COPI                | MOSIB (UM legacy name) |
| Pmod2.3 P602 | CIPO                | MISOB (UM legacy name) |
| Pmod2.4 P601 | SCK                 | RSPCKB                 |
| Pmod2.5 GND  | GND                 | GND                    |
| Pmod2.6 3V3  | 3V3                 | 3V3                    |

Insert any microSD card before booting -- the demo formats it itself. A card of
any size >= ~512 KiB works: the formatter's auto cluster-size sweep lands each
type's cluster count in its valid band (FAT12 < 4085, FAT16 4085..65524,
FAT32 >= 65525). On a typical multi-GB card, FAT12/FAT16 may exceed their
cluster ceilings even at the maximum cluster size; a type that cannot fit the
card is **skipped** (`SKIP: capacity out of range`), not failed, so the run
still ends in `ALL PASS`. Use a small card (or a board_sim `--sd-new` card) to
exercise every type.

## Build

```
make
```

Outputs `build/fs_format_mount.elf` / `.hex` / `.bin`.

## Run in board_sim (no hardware)

board_sim models the microSD over `ra8_sdmmc_spi` and can attach a blank card of
any size with `--sd-new <MiB>[:fat16|fat32]`. The initial format does not matter
(the app reformats it), but `--sd-new` gives the modelled card a correct CSD
size so the SD bring-up reports a real capacity.

```
cmake -B tools/board_sim/build -S tools/board_sim
cmake --build tools/board_sim/build
make fs_format_mount
tools/board_sim/build/board_sim \
    examples/ek_ra8d2/hw_validated/hil/fs_format_mount/build/fs_format_mount.elf \
    --sd-new 64:fat32
```

Expected console tail:

```
fsfmt: boot
fsfmt: card ready
fsfmt: FS FAT12 FORMAT+MOUNT PASS
fsfmt: FS FAT16 FORMAT+MOUNT PASS
fsfmt: FS FAT32 FORMAT+MOUNT PASS
fsfmt: FS EXFAT FORMAT+MOUNT PASS
fsfmt: FS FORMAT+MOUNT ALL PASS
```

The same gate runs under `scripts/sim/smoke.sh fs_format_mount` (it stops
on the `PASS` banner with a bounded chunk/wall budget).

## Run on the bench (real silicon)

> The on-bench flash/run is the user's to perform. The repo build + board_sim
> validation above is what is automated.

1. Plug a PMOD MicroSD into Pmod2 (J25) with a **disposable** microSD card
   inserted (this erases it).
2. `make flash` (J-Link OB via `scripts/dev/flash.sh`).
3. Open the J-Link OB CDC console at 115200 8N1 (`scripts/dev/debug.sh` or any
   serial terminal on the CDC port).
4. The HIL runner scrapes for `FS FORMAT+MOUNT ALL PASS` (see `hil.conf`).

## Console output (success path)

```
fsfmt: boot
fsfmt: card ready
fsfmt: FS FAT12 FORMAT+MOUNT PASS
fsfmt: FS FAT16 FORMAT+MOUNT PASS
fsfmt: FS FAT32 FORMAT+MOUNT PASS
fsfmt: FS EXFAT FORMAT+MOUNT PASS
fsfmt: FS FORMAT+MOUNT ALL PASS
```

## HIL plan

`uart_scrape` on the banner:

```
HIL_MODE=uart_scrape
HIL_EXPECT="FS FORMAT+MOUNT ALL PASS"
HIL_EXPECT_NEGATIVE="FAIL|HardFault|TIMEOUT|mismatch"
HIL_TIMEOUT_S=240
```
