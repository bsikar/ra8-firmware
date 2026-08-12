# threadx_fs_demo

ThreadX + ra8_fs FAT file-operations demo on the EK-RA8D2's on-board **Octo-SPI
NOR flash** (via LevelX wear-levelling) -- **no SD card**.

Originally an SD-card demo, but the board's microSD is on Pmod2 / SCI0
Simple-SPI, so it was rewritten to target the always-present OSPI flash. It is
the file-operations counterpart to `threadx_fs_levelx_demo` (which proves the
LevelX integration); this one exercises the ra8_fs FAT API itself from an RTOS
world, through the `ra8_fs_set_lock()` seam bound to a ThreadX mutex (#608).
(Both demos ran on the vendored FileX until #611 retired it -- ra8_fs covers
the whole surface they used, so they were ported and FileX was deleted.)

## What this app does

1. Brings the chip up like `uart_hello` (CGC + SCI8 @ 115200 8N1 on PD_02/PD_03).
2. Hands control to ThreadX. The single worker thread:
   - Formats + opens a LevelX NOR partition on the OSPI flash
     (`lx_nor_driver_ra8_xspi`).
   - Lays down + mounts a FAT volume on top via the LevelX<->ra8_fs backend
     (`lx_fs_backend_bind` -> `ra8_fs_format` -> `ra8_fs_mount`).
   - Creates `readme.txt` and `scratch.txt`, lists the root, reads `readme.txt`
     back and **verifies the bytes match**, deletes `scratch.txt` and confirms
     it is gone.
   - Prints `[fs] ospi FAT roundtrip ok` on success.

## Run

```
make threadx_fs_demo
bash scripts/hil/run_local.sh threadx_fs_demo   # flash + scrape the banner
```

No card, no jumpers -- the flash is soldered on the board, so this runs
unattended on the HIL bench.

## Expected output

```
[fs] formatting + opening LevelX on OSPI flash
[fs] formatting + mounting FAT volume
[fs] root listing:
[fs] readback verified
[fs] ospi FAT roundtrip ok
[fs] done
```

## Build dependencies

`USES threadx levelx` plus `LIBS ra8_fs`: the per-app `CMakeLists.txt` splices
in the LevelX xSPI NOR driver and the LevelX<->ra8_fs block-device backend
(`port/levelx/src/lx_fs_backend.c`) -- mirroring `threadx_fs_levelx_demo`.

## HIL

`uart_scrape` gate on `ospi FAT roundtrip ok` (negative regex catches the
format/mount/write/verify/delete failure banners). Runs board-only.
