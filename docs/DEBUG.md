# Debugging the EK-RA8D2

The EK-RA8D2 carries an on-board SEGGER J-Link OB. Both supported host
toolchains talk to that same probe over USB / SWD, so the choice between them is
reversible at any time.

**SEGGER J-Link / Ozone.** Proprietary, free for evaluation, with a polished GUI
debugger. This is what the project develops against day to day, and it is the
path that can program MRAM.

**OpenOCD + arm-none-eabi-gdb.** GPL, and needs no vendor download -- which is
the whole point if you cannot install the SEGGER package. It attaches and
debugs, but read the MRAM limitation below before relying on it to flash.

Wrapper scripts for both paths live in `scripts/dev/` (`flash.sh`, `debug.sh`
and `ozone.sh` for SEGGER; `openocd_flash.sh` and `openocd_debug.sh` for the
other), and the root justfile wraps those in turn. The OpenOCD board
config is `scripts/dev/openocd/ek-ra8d2.cfg`, which documents its own limits
inline.

## Gotchas

- **OpenOCD has no MRAM flash driver.** The RA8D2 boots from MRAM at
  `0x02000000`, and there is no upstream OpenOCD driver for the RA-series MRAM
  controller. RAM-resident loads work today, including `load` from gdb into
  SRAM, but persistent MRAM programming falls back to the SEGGER path. This is
  the one reason the two paths are not interchangeable.
- **Cortex-M85 CPUID.** OpenOCD recognises the M85 from 0.12 onward. An older
  build still attaches, but logs "Cortex-M unknown" -- easy to misread as a
  broken connection.
- **The secondary M33 is not configured.** The board config declares a single
  target; debugging the M33 means adding a second one.
- **DAP / IDCODE values are unconfirmed.** The `expected-id` entries in the
  board config were never captured from real silicon, and the cfg carries
  `TODO: confirm` notes where that matters.
- **Linux needs libusb permissions** for the on-board probe. The SEGGER udev
  rules cover it, or add a generic rule for SEGGER's USB vendor ID `1366`.

When in doubt during hardware bring-up, use the SEGGER path.
