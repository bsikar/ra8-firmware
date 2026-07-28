# ra8_io_xspi_demo -- ra8_io fabric over OSPI NOR flash (epic #155, #156)

A single self-contained app that drives the `ra8_io` I/O fabric over the
on-board Octo-SPI (OSPI / xSPI) NOR flash -- a third storage tier alongside the
RAM (`ra8_io_demo`) and SD (`ra8_io_sd_demo`) demos. NOR is an
*erase-before-write* medium: a program can only clear bits (1 -> 0), so a 4 KiB
sector must be erased back to all-ones before any byte in it is rewritten. This
demo proves the fabric exercises the block-device capabilities
(`must_erase_before_write`, whole-sector read-modify-write) that the RAM and SD
backends never trigger.

What it exercises:

1. **OSPI controller bring-up:** `ra8_xspi_init(0, k_ra8_xspi_lio_1s1s1s)` brings
   the xSPI controller up in 1S-1S-1S link mode.
2. **Erase-before-write block device (Phase 1, #156):** an xSPI block device
   (`ra8_io_blockdev_xspi`) over a 256 KiB NOR window. Each 512-byte block write
   triggers a whole-4-KiB-sector erase + reprogram RMW inside the backend.
3. **Filesystem bridge + VFS (Phase 3, #158):** the block device is bridged to
   `ra8_fs` (`ra8_io_blockdev_as_fs_backend`), formatted + mounted as FAT12, and
   registered in the VFS under the name `xs`.
4. **Named file I/O:** `mkdir xs:/CFG`, then a 256-byte payload is written to
   `xs:/CFG/SET.BIN`, read back through the VFS path, and byte-compared.
5. **Targetable stdio (Phase 2, #157):** progress is printed over the SCI8
   console through a `ra8_io_stream` UART sink, and `ra8_log` is routed into the
   same stream (`ra8_io_log_attach`).

The window and payload are kept small (512 blocks = 256 KiB, 256-byte payload)
because each 512-byte write drives a 4 KiB erase-reprogram RMW that is slow in
ra8_emulator; a large volume would blow the run budget.

## Build

```
make            # -> build/ra8_io_xspi_demo.elf
```

## Run in the emulator

ra8_emulator models the 2 MiB OSPI NOR array internally, so no `--sd` flag is
needed. The OSPI RMW is slow in the emulator, so give it a generous budget:

```
RA8_EMU_MAX_CHUNKS=4000000 RA8_EMU_WALL_S=60 \
  tools/ra8_emulator/build/ra8_emulator build/ra8_io_xspi_demo.elf
```

Expected console output:

```
[uart] SCI8: ra8_io_xspi_demo: boot
[uart] SCI8: ra8_io_xspi_demo: xs:/CFG/SET.BIN 256 bytes PASS
```

## Status

`hw_pending`: the logic is proven in `ra8_emulator`, which models the OSPI NOR
flash. The same code runs on silicon; promote to `hw_validated` after a bench
run captures the PASS line over the J-Link UART against the real IS25LX512M.
