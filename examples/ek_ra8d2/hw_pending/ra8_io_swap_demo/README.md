# ra8_io_swap_demo -- ra8_io fabric capstone (epic #155, issue #264)

A single self-contained app that shows off every `ra8_io` abstraction at once:
two block-device backends swapped behind one interface, a VFS open/read/write/
close round-trip, and stdio retargeted to two different sinks. Where each sibling
`ra8_io_*_demo` binds ONE backend, this is the capstone that ties the fabric
together in one binary.

## What it exercises

1. **Drop-in block-device swap (`ra8_io_blockdev.h` + backends).** One
   backend-agnostic engine (`swap_run_one`) runs the *identical* fabric
   round-trip over a table of backends:

   - Row 0 -- an in-SRAM RAM block device (`ra8_io_blockdev_ram`): volatile,
     erases to zero, no erase-before-write.
   - Row 1 -- the on-board Octo-SPI (xSPI) NOR flash (`ra8_io_blockdev_xspi`): an
     *erase-before-write* medium that reads back `0xFF` after erase and does a
     whole-4-KiB-sector read-modify-write on every 512-byte block write.

   The engine names no peripheral -- the two capability-different media are proven
   interchangeable behind the one `ra8_io_blockdev_t` vtable. Adding a third
   medium (SD, SDRAM, MRAM) is one more `swap_backend_t` table row; the engine
   body never changes.

2. **VFS open/read/write/close (`ra8_io_vfs.h`).** Each backend is bridged to
   `ra8_fs` (`ra8_io_blockdev_as_fs_backend`), formatted + mounted as FAT12, and
   registered in the VFS under a short name (`ram`, `xs`). The round-trip opens
   `<name>:/DATA.BIN` for write, writes, closes, re-opens for read, reads, closes,
   and byte-compares -- the streaming file API, not a whole-file shortcut.

3. **Targetable stdio, retargeted to two sinks (`ra8_io_stream.h`).** The engine
   writes its progress through a `ra8_io_stream_t`. During the swap phase that
   stream is an in-RAM capture sink (`ra8_io_stream_ram`), so the same `puts` /
   `put_u32` calls that would otherwise reach the serial console are captured into
   a byte buffer. Afterwards the captured bytes are replayed out of the UART sink
   (`ra8_io_stream_uart`) -- one writer, two destinations.

## Build

```
make            # -> build/ra8_io_swap_demo.elf
```

## Run in the simulator

board_sim models both the RAM region and the OSPI NOR array, so no `--sd` flag is
needed. The OSPI RMW is slow in the emulator, so give it a generous instruction
budget:

```
BOARD_SIM_MAX_CHUNKS=6000000 BOARD_SIM_WALL_S=120 \
  tools/ra8_emulator/build/ra8_emulator build/ra8_io_swap_demo.elf
```

Expected console output (RAM leg prints first, the OSPI leg follows after its RMW
budget):

```
[uart] SCI8: ra8_io_swap_demo: boot
[uart] SCI8: ra8_io_swap_demo: --- ram-captured stdio ---
[uart] SCI8: swap[ram]: mount RAIORAM -> 96 bytes ok
[uart] SCI8: swap[xs]: mount RAIOXS -> 96 bytes ok
[uart] SCI8: ra8_io_swap_demo: --- end capture ---
[uart] SCI8: ra8_io_swap_demo: two-backend swap (ram + xs) PASS
[uart] SCI8: ra8_io_swap_demo: ram-stream capture <N> bytes PASS
```

## Host unit test

The engine logic, the deterministic payload pattern, and the read-back verdict
are covered on the host in `tests/test_app_ra8_io_swap_demo.c`, which runs the
same drop-in swap over a RAM backend and the register-level xSPI NOR model
(`tests/mocks/ra8_sim_xspi_flash.c`) plus the in-RAM stdio sink -- so the whole
fabric abstraction is exercised without hardware. MC/DC vectors cover the
compound read-back verdict and the pattern-equality guard.

## Status

`hw_pending`: the RAM, VFS, and stdio legs are fully exercised in `board_sim` and
in the host unit test. The xSPI leg programs the *non-volatile* on-board OSPI NOR
and has not yet been captured on the bench -- it is proven in `board_sim` (which
models the OSPI NOR flash) and is register-identical to the already-sim-proven
`ra8_io_xspi_demo`. Promote to `hw_validated` after a bench run captures the
`two-backend swap (ram + xs) PASS` line over the J-Link UART against the real
IS25LX512M.
