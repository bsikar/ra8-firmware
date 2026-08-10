# threadx_fs_levelx_demo

ThreadX + ra8_fs-on-LevelX-on-OSPI-flash format/mount/write/read demo.

The worker thread formats + opens a LevelX partition on the on-board ISSI
IS25LX512M (U16, xSPI controller **CS1**, driven by `ra8_xspi` through
`port/levelx/src/lx_nor_driver_ra8_xspi.c`), binds the LevelX flash to the
ra8_fs block-device backend (`port/levelx/src/lx_fs_backend.c`), installs the
`ra8_fs_set_lock()` seam over a ThreadX mutex (#608), formats + mounts a FAT
volume, writes `/levelx_test.txt`, and reads it back to SCI8.

(This demo ran on the vendored FileX until #611 retired it -- ra8_fs covers
the whole surface it used, so it was ported and FileX was deleted.)

## History

The OSPI flash bring-up that blocked this app is fixed (#44): the earlier
"JTAG-confirmed dead chip / physically unresponsive" conclusion was wrong --
it was a controller chip-select bug, see
`examples/ek_ra8d2/hw_validated/hil/flash_journal/README.md`. The LevelX
format failure (#87) is also fixed: the LevelX NOR driver owns the OCTA bus
bring-up, so the app must not route the xSPI pins itself.

## Run

```
make threadx_fs_levelx_demo
bash scripts/hil/run_local.sh threadx_fs_levelx_demo
```

## HIL

`uart_scrape` gate on the exact read-back line
`[fslx] readback: Hello from wear-leveled FAT!` (negative regex catches the
LevelX format/open, backend bind, and ra8_fs format/mount/write/read failure
banners). Runs board-only.
