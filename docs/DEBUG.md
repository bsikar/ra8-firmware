# Debugging the EK-RA8D2

The EK-RA8D2 ships with an on-board SEGGER J-Link OB. There are two supported
host-side toolchains for flashing and debugging:

1. **SEGGER tools (default).** Proprietary, free-for-eval, polished GUI.
   Recommended on macOS and Windows.
2. **OpenOCD + arm-none-eabi-gdb.** GPL-only, no vendor download required.
   Recommended on Linux and for users who cannot install the SEGGER package.

Both paths talk to the same on-board J-Link OB over USB / SWD. Pick whichever
matches your environment; you can switch between them at any time.

---

## Path 1: SEGGER J-Link / Ozone (default)

Install the SEGGER J-Link package: <https://www.segger.com/downloads/jlink/>.
Install Ozone if you want a GUI debugger:
`brew install --cask segger-ozone` on macOS.

Build artifacts land in `<app-dir>/build/<app>.{elf,hex}`. For
`make blink` the directory is
`examples/ek_ra8d2/hw_validated/hil/blink/`; the top-level Makefile
auto-discovers each app's directory so the `make <app>` shorthand works
regardless of tier.

```sh
# Build
make blink

# Flash
./scripts/dev/flash.sh <app-dir>/build/<app>.hex

# Debug (CLI, JLinkGDBServer + arm-none-eabi-gdb)
./scripts/dev/debug.sh <app-dir>/build/<app>.elf

# Debug (GUI, Ozone)
./scripts/dev/ozone.sh <app-dir>/build/<app>.elf
```

Per-app Makefiles wrap these:

```sh
make -C <app-dir> flash
make -C <app-dir> debug
make -C <app-dir> ozone
```

---

## Path 2: OpenOCD + arm-none-eabi-gdb (GPL alternative)

Install OpenOCD 0.12 or newer (the version that recognises the Cortex-M85
CPUID). On Linux you also need libusb permissions for the on-board J-Link
OB -- the standard SEGGER udev rules from
`/usr/share/jlink/99-jlink.rules` work, or add a generic rule for VID
`1366` (SEGGER).

```sh
# Linux
sudo apt install openocd gdb-multiarch

# macOS
brew install openocd
brew install --cask gcc-arm-embedded   # for arm-none-eabi-gdb
```

### Flash

```sh
make blink
./scripts/dev/openocd_flash.sh <app-dir>/build/<app>.hex
```

Under the hood:

```sh
openocd -f scripts/dev/openocd/ek-ra8d2.cfg \
        -c "program <app-dir>/build/<app>.hex verify reset exit"
```

### Debug

```sh
./scripts/dev/openocd_debug.sh <app-dir>/build/<app>.elf
```

This starts `openocd` in the background (GDB server on `localhost:3333`)
and runs `arm-none-eabi-gdb` against the ELF. Press `Ctrl-D` at the gdb
prompt to quit; the openocd process is cleaned up automatically.

To run the two halves in separate terminals (useful for IDE integrations
that drive gdb themselves):

```sh
# Terminal 1
openocd -f scripts/dev/openocd/ek-ra8d2.cfg

# Terminal 2
arm-none-eabi-gdb <app-dir>/build/<app>.elf \
    -ex "target extended-remote :3333" \
    -ex "monitor reset halt" \
    -ex "load"
```

---

## Known limitations of the OpenOCD path

The board config at `scripts/dev/openocd/ek-ra8d2.cfg` documents these inline,
but in summary:

- **MRAM flash driver.** The RA8D2 boots from MRAM at `0x02000000`. There is
  no upstream OpenOCD driver for the RA-series MRAM controller yet. RAM-resident
  loads (and `load` from gdb into SRAM) work today; persistent MRAM
  programming may fall back to the SEGGER path (`scripts/dev/flash.sh`) until a
  driver lands. See the `TODO: confirm MRAM unlock` block in the cfg.
- **Cortex-M85 CPUID.** OpenOCD 0.12+ recognises the M85; older builds will
  attach but log "Cortex-M unknown".
- **Secondary M33 core.** Not configured. Add a second `target create ... cpu1`
  entry to the cfg if you need to debug the M33.
- **DAP / IDCODE.** Confirmed values for `expected-id` are not yet captured;
  see the `TODO: confirm` notes in the cfg.

When in doubt for hardware bring-up, use the SEGGER path -- it is what we
develop against day-to-day.
