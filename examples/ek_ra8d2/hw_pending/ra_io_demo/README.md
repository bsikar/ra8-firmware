# ra_io_demo -- ra_io fabric end-to-end (epic #155)

A single self-contained app that drives the whole `ra_io` I/O fabric with no
external hardware, so it runs headlessly in `board_sim`.

What it exercises:

1. **Block device (Phase 1, #156):** a RAM block device over a 256 KiB in-SRAM
   buffer (`ra_io_blockdev_ram`).
2. **Filesystem bridge + VFS (Phase 3, #158):** the block device is bridged to
   `ra_fs` (`ra_io_blockdev_as_fs_backend`), formatted + mounted as FAT12, and
   registered in the VFS under the name `ram`.
3. **Named file I/O:** a file is written, then read back through the
   `"ram:/HELLO.TXT"` path and byte-compared.
4. **Targetable stdio (Phase 2, #157):** progress is printed over the SCI8
   console through a `ra_io_stream` UART sink, and `ra_log` is routed into the
   same stream (`ra_io_log_attach`).

## Build

```
make            # -> build/ra_io_demo.elf
```

## Run in the simulator

```
BOARD_SIM_WALL_S=12 tools/board_sim/build/board_sim build/ra_io_demo.elf
```

Expected console output:

```
[uart] SCI8: ra_io_demo: boot
[uart] SCI8: ra_io_demo: wrote/read 128 bytes ram:/HELLO.TXT PASS
```

## Status

`hw_pending`: the logic is proven in `board_sim` (RAM backend is pure memory, so
no peripheral model is needed). The same code runs on silicon; promote to
`hw_validated` after a bench run captures the PASS line over the J-Link UART.
