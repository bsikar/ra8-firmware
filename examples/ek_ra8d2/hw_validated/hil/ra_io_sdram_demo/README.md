# ra_io_sdram_demo -- ra_io fabric over external SDRAM (epic #155)

Proves the `ra_io` fabric's swappable-backend promise over the EK-RA8D2's 64 MiB
external **SDRAM** (at `0x68000000`). It is the same app as `ra_io_demo` with one
thing changed: the block device is the SDRAM backend (`ra_io_blockdev_sdram_init`,
which brings the controller up via `ra_sdramc_init`) instead of an in-SRAM RAM
disk. The `ra_fs` + VFS layers above are byte-for-byte identical.

No external hardware required; it runs headlessly in `board_sim`, which maps the
SDRAM region and models the controller bring-up.

What it exercises:

1. **Block device (Phase 1, #156):** an SDRAM block device over a 256 KiB window
   of the external SDRAM (`ra_io_blockdev_sdram`).
2. **Filesystem bridge + VFS (Phase 3, #158):** the block device is bridged to
   `ra_fs` (`ra_io_blockdev_as_fs_backend`), formatted + mounted as FAT12, and
   registered in the VFS under the name `dr`.
3. **Named file I/O:** a file is written, then read back through the
   `"dr:/HELLO.TXT"` path and byte-compared.
4. **mkdir + nested paths (Phase 3b, #158):** `mkdir dr:/SUB` then a round-trip
   of `dr:/SUB/NOTE.TXT` two levels deep.
5. **Targetable stdio (Phase 2, #157):** progress is printed over the SCI8
   console through a `ra_io_stream` UART sink, and `ra_log` is routed into the
   same stream (`ra_io_log_attach`).

## Build

```
make            # -> build/ra_io_sdram_demo.elf
```

## Run in the simulator

```
BOARD_SIM_WALL_S=20 tools/board_sim/build/board_sim build/ra_io_sdram_demo.elf
```

Expected console output:

```
[uart] SCI8: ra_io_sdram_demo: boot
[uart] SCI8: ra_io_sdram_demo: wrote/read 128 bytes dr:/HELLO.TXT PASS
[uart] SCI8: ra_io_sdram_demo: mkdir+nested dr:/SUB/NOTE.TXT PASS
```

## Status

`hw_pending`: the logic is proven in `board_sim` (which maps the SDRAM region and
models the controller bring-up). The same code runs on silicon; promote to
`hw_validated` after a bench run captures the PASS line over the J-Link UART.
