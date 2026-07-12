# threadx_filex_levelx_demo

ThreadX + FileX-on-LevelX-on-OSPI-flash format/mount/write/read
heartbeat demo.

## Why this is `hw_pending`

The OSPI flash bring-up that blocked this app is fixed (#44): the
on-board ISSI IS25LX512M (U16, JEDEC 0x9D5A1A) is on xSPI controller
**CS1**, and `ra8_xspi` now drives it correctly (the earlier "JTAG-
confirmed dead chip / physically unresponsive" conclusion was wrong --
it was a controller chip-select bug, see
`examples/ek_ra8d2/hw_pending/flash_journal/README.md`). LevelX runs
against that flash through the unchanged `port/levelx/lx_nor_driver_ra8_xspi`
HAL path, so it inherits the fix.

This app stays in `hw_pending` only until a full HIL re-run confirms the
ThreadX + FileX + LevelX stack mounts/formats/round-trips on the now-live
flash; `flash_journal` already verifies the underlying erase/program/read.

## How to graduate

Run the `hil.conf` uart-scrape gate on the bench: `[fxlx] booting xSPI
flash` should appear with no failure banner. `make flash_journal`
already passes its round-trip gate, so the flash itself is proven.
