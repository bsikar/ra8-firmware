# threadx_fs_levelx_demo

Formats and mounts a FAT volume on top of LevelX on the on-board ISSI
IS25LX512M NOR flash (U16), writes a file and reads it back, all from one
ThreadX worker. Same stack as `threadx_fs_demo`, but this app is the proof of
the LevelX integration rather than of the file API.

**The LevelX NOR driver owns the xSPI bus bring-up, and the app must not
duplicate it.** The driver routes the pins and initialises the controller
exactly once. An app that also calls the board pin-init or `ra8_xspi_init` has
its second PFS route rejected, and the failure then surfaces far from its cause
as `lx_nor_flash_format` returning `LX_NO_MEMORY` (#87).

The part hangs off xSPI controller **CS1**, not CS0. The earlier
"JTAG-confirmed dead chip" conclusion about this flash was wrong -- it was a
chip-select bug (#44) -- which is worth remembering the next time a peripheral
looks physically unresponsive.

It also installs the `ra8_fs_set_lock()` seam over a ThreadX mutex (#608). Like
its sibling it ran on the vendored FileX until #611 retired it.
