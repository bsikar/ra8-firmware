# ra8_io_xspi_demo

The `ra8_io` fabric (#155, #156) over the on-board Octo-SPI NOR flash -- a third
storage tier alongside the RAM (`ra8_io_demo`) and SD (`ra8_io_sd_demo`) cases.

NOR is what makes this one different. It is an **erase-before-write** medium: a
program operation can only clear bits, so a 4 KiB sector has to be erased back
to all-ones before any byte in it is rewritten. Every 512-byte block write
therefore drives a whole-sector erase-and-reprogram read-modify-write inside the
backend. That exercises the block-device capability flags
(`must_erase_before_write`, whole-sector RMW) that the RAM and SD backends never
trigger, which is the reason this app exists.

Above the block device everything is the fabric as usual: bridge to `ra8_fs`,
format and mount FAT12, register in the VFS, `mkdir`, write a payload, read it
back through the VFS path and byte-compare. The controller comes up in 1S-1S-1S
link mode via `ra8_xspi_init`. The window and payload are kept deliberately
small because each write costs a full sector RMW.

The real part is an IS25LX512M. Its bus is the constraint to watch on this
bench: the Octo-SPI pins are the PMOD1 pins, so anything else wired onto PMOD1
takes the flash away from this app and it cannot reach its storage. The carrier
PCB (#318) is the way out.
