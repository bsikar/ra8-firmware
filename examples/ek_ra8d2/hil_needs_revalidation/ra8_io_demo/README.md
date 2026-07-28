# ra8_io_demo -- ra8_io fabric end-to-end (epic #155)

A single self-contained app that drives the whole `ra8_io` I/O fabric with no
external hardware, so it runs headlessly in `ra8_emulator`.

What it exercises:

1. **Block device (Phase 1, #156):** a RAM block device over a 256 KiB in-SRAM
   buffer (`ra8_io_blockdev_ram`).
2. **Filesystem bridge + VFS (Phase 3, #158):** the block device is bridged to
   `ra8_fs` (`ra8_io_blockdev_as_fs_backend`), formatted + mounted as FAT12, and
   registered in the VFS under the name `ram`.
3. **Named file I/O:** a file is written, then read back through the
   `"ram:/HELLO.TXT"` path and byte-compared.
4. **Targetable stdio (Phase 2, #157):** progress is printed over the SCI8
   console through a `ra8_io_stream` UART sink, and `ra8_log` is routed into the
   same stream (`ra8_io_log_attach`).

## Build

```
make            # -> build/ra8_io_demo.elf
```

## Run in the simulator

```
RA8_EMU_WALL_S=12 tools/ra8_emulator/build/ra8_emulator build/ra8_io_demo.elf
```

Expected console output:

```
[uart] SCI8: ra8_io_demo: boot
[uart] SCI8: ra8_io_demo: wrote/read 128 bytes ram:/HELLO.TXT PASS
```

## Status

`hw_pending`: the logic is proven in `ra8_emulator` (RAM backend is pure memory, so
no peripheral model is needed). The same code runs on silicon; promote to
`hw_validated` after a bench run captures the PASS line over the J-Link UART.
