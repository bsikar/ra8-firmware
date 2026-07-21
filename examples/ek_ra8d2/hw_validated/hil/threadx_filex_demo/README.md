# threadx_filex_demo

ThreadX + FileX FAT file-operations demo on the EK-RA8D2's on-board **Octo-SPI
NOR flash** (via LevelX wear-levelling) -- **no SD card**.

Originally an SD-card (SDHI) demo, but the board's microSD is on Pmod2 / SCI0
Simple-SPI, which the FileX SDHI driver cannot reach, so it faulted on
`fx_media_open`. Rewritten to target the always-present OSPI flash. It is the
file-operations counterpart to `threadx_filex_levelx_demo` (which proves the
LevelX integration); this one exercises the FileX FAT API itself.

## What this app does

1. Brings the chip up like `uart_hello` (CGC + SCI8 @ 115200 8N1 on PD_02/PD_03).
2. Hands control to ThreadX. The single worker thread:
   - Formats + opens a LevelX NOR partition on the OSPI flash
     (`lx_nor_driver_ra8_xspi`).
   - Lays down + mounts a FAT volume on top via the LevelX<->FileX adapter
     (`lx_filex_adapter_bind` -> `fx_media_format` -> `fx_media_open`).
   - Creates `readme.txt` and `scratch.txt`, lists the root, reads `readme.txt`
     back and **verifies the bytes match**, deletes `scratch.txt` and confirms
     it is gone.
   - Prints `[filex] ospi FAT roundtrip ok` on success.

## Run

```
make threadx_filex_demo
bash scripts/hil/run_local.sh threadx_filex_demo   # flash + scrape the banner
```

No card, no jumpers -- the flash is soldered on the board, so this runs
unattended on the HIL bench.

## Result (validated 2026-06-15 on real hardware)

```
[filex] formatting + opening LevelX on OSPI flash
[filex] formatting + opening FAT volume
[filex] root listing:
[filex] readback verified
[filex] ospi FAT roundtrip ok
[filex] done
```

## Build dependencies

Links the FileX **core** plus the **LevelX** flash port (not the FileX SDHI
bridge): the per-app `CMakeLists.txt` enables `RA8_USE_FILEX`, `USES threadx
levelx`, links `filex`, and adds the LevelX<->FileX port sources -- mirroring
`threadx_filex_levelx_demo`.

## HIL

`uart_scrape` gate on `ospi FAT roundtrip ok` (negative regex catches the
format/mount/write/verify/delete failure banners). Runs board-only.
