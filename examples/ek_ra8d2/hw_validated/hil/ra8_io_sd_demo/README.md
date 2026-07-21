# ra8_io_sd_demo

Status: **hw_pending** (board_sim-validated; not yet run on real silicon).

Proves the `ra8_io` fabric's "swappable backend" promise (epic #155, phase #156):
the **same** `ra8_io` VFS API that `ra8_io_demo` runs over a RAM disk, now running
over a **micro-SD card** by swapping only the block-device backend.

## What it proves

`ra8_io_demo` binds the RAM block device; this app binds the SD-over-SPI block
device (`ra8_io_blockdev_sdspi_init`) on top of the `ra8_sdmmc_spi` driver. The SD
bring-up (Pmod2 / SCI0 Simple-SPI transport adapter, card init) is reused
verbatim from `fs_format_mount`. Everything above the block device is the
identical fabric:

1. `ra8_io_blockdev_sdspi_init` -- SD-SPI block-device vtable over the card.
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
ra8_io_sd_demo: boot
ra8_io_sd_demo: card ready
ra8_io_sd_demo: sd:/LOGS/A.TXT 512 bytes PASS
```

The HIL runner and the board_sim smoke gate scrape for the
`sd:/LOGS/A.TXT 512 bytes PASS` line.

## Hardware

| EK-RA8D2 pin | PMOD MicroSD signal | Net (UM Table 19 p 27) |
|--------------|---------------------|------------------------|
| Pmod2.1 P604 | CS                  | SSLB0 (GPIO output)    |
| Pmod2.2 P603 | COPI                | MOSIB (UM legacy name) |
| Pmod2.3 P602 | CIPO                | MISOB (UM legacy name) |
| Pmod2.4 P601 | SCK                 | RSPCKB                 |
| Pmod2.5 GND  | GND                 | GND                    |
| Pmod2.6 3V3  | 3V3                 | 3V3                    |

## Build

```
make build
```

Outputs `build/ra8_io_sd_demo.elf` / `.hex` / `.bin`.

## Run in board_sim (no hardware)

board_sim models the microSD over `ra8_sdmmc_spi`; attach a blank card with
`--sd-new <MiB>[:fat16|fat32]`. From the repo root:

```
tools/board_sim/build/board_sim \
    examples/ek_ra8d2/hw_pending/ra8_io_sd_demo/build/ra8_io_sd_demo.elf \
    --sd-new 64:fat16
```

Success is the `... sd:/LOGS/A.TXT 512 bytes PASS` console line with no
`INVALID INSN` / `UNMAPPED` / `BKPT`.

## Run on the bench (real silicon)

> The on-bench flash/run is the user's to perform. The repo build + board_sim
> validation above is what is automated.

1. Plug a PMOD MicroSD into Pmod2 (J25) with a **disposable** microSD inserted.
2. `make flash` (J-Link OB via `scripts/dev/flash.sh`).
3. Open the J-Link OB CDC console at 115200 8N1.
4. Scrape for `sd:/LOGS/A.TXT 512 bytes PASS`.
