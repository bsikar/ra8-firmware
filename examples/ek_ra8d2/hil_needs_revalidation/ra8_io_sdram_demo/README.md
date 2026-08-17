# ra8_io_sdram_demo

The `ra8_io` fabric (#155) over the EK-RA8D2's 64 MiB external SDRAM at
`0x68000000`. It is `ra8_io_demo` with exactly one thing changed: the block
device is the SDRAM backend (`ra8_io_blockdev_sdram_init`, which brings the
controller up via `ra8_sdramc_init`) instead of an in-SRAM RAM disk. The
`ra8_fs` and VFS layers above it are byte-for-byte identical, which is the
swappable-backend claim being made.

Over that backend it formats and mounts a FAT12 volume, registers it in the VFS,
round-trips a file through its VFS path with a byte-compare, and exercises the
nested-path case with a `mkdir` and a file two levels deep (#158). Progress goes
out over SCI8 through a `ra8_io_stream` UART sink with `ra8_log` routed into it.

No external hardware -- the SDRAM is on the board.
