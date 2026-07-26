# ra8_io_sdhi_demo

Status: **hw_pending** (board_sim-validated; not yet run on real silicon).

Proves the `ra8_io` fabric's "swappable backend" promise (epic #155, phase #156)
on the **native 4-bit SDHI** controller (#123): the **same** `ra8_io` VFS API that
`ra8_io_demo` runs over a RAM disk and `ra8_io_sd_demo` runs over SD-over-SPI, now
running over a **micro-SD card reached through the dedicated SDHI host
controller** by swapping only the block-device backend.

## What it proves

`ra8_io_sd_demo` binds the SD-over-SPI block device; this app binds the
native-SDHI block device (`ra8_io_blockdev_sdhi_init`) on top of the `ra8_sdcard` +
`ra8_sdhi` HAL drivers. The SD bring-up routes the eight SDHI pins (port 4, pins
0..7) and runs the full SD Physical Layer identification through
`ra8_sdcard_init`. Everything above the block device is the identical fabric:

1. `ra8_io_blockdev_sdhi_init` -- native-SDHI block-device vtable over the card.
2. `ra8_io_blockdev_as_fs_backend` -- bridge the block device to `ra8_fs`.
3. `ra8_fs_format` (FAT16) + `ra8_fs_mount`, then `ra8_io_vfs_mount("sd", ...)`.
4. `ra8_io_vfs_mkdir("sd:/LOGS")` -- exercise the VFS mkdir path.
5. `ra8_io_vfs_open("sd:/LOGS/A.TXT", write)` + `ra8_fs_write` -- write 512 bytes.
6. `ra8_io_vfs_open("sd:/LOGS/A.TXT", read)` + `ra8_fs_read` -- read it back.
7. byte-compare the read-back against the deterministic LCG payload.

Every return value is checked; the first failing step prints `FAIL` and parks
the CPU.

> **Warning:** this app **erases the card** (it reformats FAT16). Insert a
> disposable microSD before booting.

## Expected output (success path)

```
ra8_io_sdhi_demo: boot
ra8_io_sdhi_demo: card ready
ra8_io_sdhi_demo: sd:/LOGS/A.TXT 512 bytes PASS
```

The HIL runner and the board_sim smoke gate scrape for the
`sd:/LOGS/A.TXT 512 bytes PASS` line.

## Hardware

The eight SDHI bus pins are on port 4, pins 0..7:

| Port-4 pin | SDHI signal |
|------------|-------------|
| P400       | SD_CMD      |
| P401       | SD_CLK      |
| P402       | SD_DAT0     |
| P403       | SD_DAT1     |
| P404       | SD_DAT2     |
| P405       | SD_DAT3     |
| P406       | SD_WP       |
| P407       | SD_CD       |

All eight route to `PSEL = k_ra8_psel_sdhi`.

## Build

```
make build
```

Outputs `build/ra8_io_sdhi_demo.elf` / `.hex` / `.bin`.

## Run in board_sim (no hardware)

board_sim models the native SDHI host controller (`board_periph_sdhi.c`) over the
same `--sd-new` card image as the SPI model; attach a blank card with
`--sd-new <MiB>[:fat16|fat32]`. From the repo root:

```
tools/ra8_emulator/build/ra8_emulator \
    examples/ek_ra8d2/hw_pending/ra8_io_sdhi_demo/build/ra8_io_sdhi_demo.elf \
    --sd-new 64:fat16
```

Success is the `... sd:/LOGS/A.TXT 512 bytes PASS` console line with no
`INVALID INSN` / `UNMAPPED` / `BKPT`.

## Run on the bench (real silicon)

> The on-bench flash/run is the user's to perform. The repo build + board_sim
> validation above is what is automated.

1. Wire a microSD card to the port-4 SDHI bus with a **disposable** card inserted.
2. `make flash` (J-Link OB via `scripts/dev/flash.sh`).
3. Open the J-Link OB CDC console at 115200 8N1.
4. Scrape for `sd:/LOGS/A.TXT 512 bytes PASS`.
