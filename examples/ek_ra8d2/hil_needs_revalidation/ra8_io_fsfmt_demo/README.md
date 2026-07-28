# ra8_io_fsfmt_demo -- pluggable filesystem-format registry (Phase 4, #159)

A single self-contained app that drives the `ra8_io` filesystem-format registry
with no external hardware, so it runs headlessly in `ra8_emulator`.

The registry lets the fabric recognise the on-disk filesystem on a block device
and report what that format can do, without the upper layers hard-coding a
`switch` over FAT vs exFAT. Each format provides a `probe` (does this volume
look like me?) and a capability descriptor (read-only? sub-directories?
streaming writes? name length?).

What it exercises:

1. **Built-in probe (#159):** a FAT12 volume is formatted on a RAM block device
   (`ra8_io_blockdev_ram` -> `ra8_io_blockdev_as_fs_backend` -> `ra8_fs_format`),
   then `ra8_io_fsfmt_init` registers the built-ins and `ra8_io_fsfmt_probe`
   resolves the volume to the `"fat"` format. Its capabilities are checked:
   writable, streaming-write, 8.3 (12-byte) name length.
2. **Foreign-format seam (#159):** a stub format whose `probe` claims any volume
   bearing a magic byte in block 0 is registered through `ra8_io_fsfmt_register`.
   A second tiny RAM device is marked with that byte and probed; it resolves to
   `"stub"` with the stub's read-only / case-sensitive capabilities -- proving a
   new format plugs in with **no change** to the built-ins.
3. **Targetable stdio (Phase 2, #157):** progress is printed over the SCI8
   console through a `ra8_io_stream` UART sink, with `ra8_log` routed into the
   same stream (`ra8_io_log_attach`).

## Build

```
make            # -> build/ra8_io_fsfmt_demo.elf
```

## Run in the emulator

```
RA8_EMU_WALL_S=12 tools/ra8_emulator/build/ra8_emulator build/ra8_io_fsfmt_demo.elf
```

Expected console output:

```
[uart] SCI8: ra8_io_fsfmt_demo: boot
[uart] SCI8: ra8_io_fsfmt_demo: probed fat maxname=12 + foreign stub seam PASS
```

## Status

`hw_pending`: the logic is proven in `ra8_emulator` (the RAM backend is pure
memory, so no peripheral model is needed). The same code runs on silicon;
promote to `hw_validated` after a bench run captures the PASS line over the
J-Link UART.
