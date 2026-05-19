# threadx_filex_levelx_demo

ThreadX + FileX-on-LevelX-on-OSPI-flash format/mount/write/read
heartbeat demo.

## Why this is `hw_pending`

LevelX runs against the on-board IS25LX512M Octo-SPI flash (U16),
which JTAG probing in 2026-05-19 confirmed is physically
unresponsive (see `examples/ek_ra8d2/hw_pending/flash_journal/
README.md` for the JTAG evidence). Every LevelX format / mount /
write call hits the dead chip and times out, so the demo cannot
get past `fxlx: lx_nor_flash_format` regardless of how the HAL
is fixed.

## How to graduate back

Same as `flash_journal`: confirm U16 is powered + responsive on
the bench. Once `make flash_journal` passes its memprobe gate,
this demo can be promoted back without firmware changes.
