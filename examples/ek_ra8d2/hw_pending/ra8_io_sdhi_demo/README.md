# ra8_io_sdhi_demo

Proves the `ra8_io` fabric's swappable-backend promise (epic #155, phase #156) on
the **native 4-bit SDHI** controller (#123): the same VFS API that `ra8_io_demo`
runs over a RAM disk and `ra8_io_sd_demo` runs over SD-over-SPI, now running over
a micro-SD card reached through the dedicated SDHI host controller, by swapping
only the block-device backend.

Everything above the block device is the identical fabric -- bridge to `ra8_fs`,
format and mount FAT16, register in the VFS, mkdir, then a write / read /
byte-compare round-trip through the streaming file API. `ra8_io_sd_demo` binds
the SD-over-SPI block device; this app binds `ra8_io_blockdev_sdhi_init` on top of
the `ra8_sdcard` + `ra8_sdhi` HAL drivers, and the SD bring-up runs the full SD
Physical Layer identification. Every return value is checked; the first failing
step parks the CPU.

> **Warning:** this app **erases the card** -- it reformats FAT16. Insert a
> disposable microSD before booting.

## Hardware

The eight SDHI bus pins are on port 4, pins 0..7, all routed to
`PSEL = k_ra8_psel_sdhi`:

| Port-4 pin | SDHI signal |
|------------|-------------|
| P400       | SD_CMD      |
| P401       | SD_CLK      |
| P402       | SD_DAT0     |
| P403       | SD_DAT1     |
| P404       | SD_DAT2     |
| P405       | SD_DAT3     |
| P406       | SD_WP       |
| P407       | SD_CD       |

## Blocked on

A bench run with a real card. The native SDHI host controller is modelled
off-target against a blank card image, so the fabric round-trip is automatable;
what that cannot prove is the controller against real silicon and a real card's
timing.
