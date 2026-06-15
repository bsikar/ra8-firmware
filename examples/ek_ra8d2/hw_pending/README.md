# examples/ek_ra8d2/hw_pending/

Apps here compile and pass all CI gates but have **not yet been confirmed
working on hardware end-to-end** -- each is blocked by a peripheral that
isn't on the bench, or by a documented firmware gap. These are the apps to
work through next when the missing hardware is available.

To build: `make <appname>` from the repo root, e.g. `make sd_font_render`.
Move an app to `hw_validated/hil/` (with a `git mv`) once it passes a
hardware HIL probe AND the gap below is resolved.

## Apps and blocking reasons (current as of 2026-06-15)

### Blocked by a peripheral not on the bench

| App | Blocking reason |
|-----|-----------------|
| i3c_i2c_peripheral_demo | I2C/I3C peripheral mode needs an external controller to talk to; the bench has no controller wired up. |
| imu_lsm6dso_demo | Needs an LSM6DSO IMU on the I2C/I3C bus; not fitted. |
| sd_font_render | Needs a microSD card carrying `FONT.OTF`; the bench card is not provisioned for this app (board_sim covers it with a synthetic card image). |
| threadx_filex_demo | FileX over SDHI; needs a microSD card. |
| tz_secure_only_sd | Needs a microSD card (plus the TrustZone split). |
| usb_host_cdc_echo | Needs a real USB-CDC peripheral on the port; the two USB jacks are cabled to each other for the self-loop self-tests, and a CDC **host** class is not yet first-party. |
| usb_host_keyboard | Needs a real USB keyboard, plus a first-party HID-keyboard host class. |
| usb_host_msc_browse | Needs a real USB mass-storage device (the MSC host path itself is validated on the self-loop -- see `hw_validated/hil/usb_selftest_*`). |

### Blocked by a module / firmware gap

| App | Blocking reason |
|-----|-----------------|
| da16600_probe | The DA16600 Wi-Fi/BLE module ships factory firmware that does not answer AT commands; it needs the AT-command SDK flashed first. Not an RA8D2-side wiring issue. |
| tz_nsc_cgc_usb | Needs the real TrustZone secure / non-secure partition (separate S + NS stacks/.bss and a proper BLXNS transition): the NS-pointer veneer check rejects args whose `.data`/`.bss`/stack live in secure SRAM. Tracked in #60 / #54. |

## Recently promoted

The on-board **USB self-loop self-tests** (`usb_selftest_hs_host`, `_fs_host`,
`_cdc`, `_hid`, `_microsd`, `_mlun`, `_wlun`, `_ospi`, `_ospi_rw`, `_soak`) were
validated on real hardware (FS jack cabled to HS jack, the board both hosts and
devices itself) and moved to `hw_validated/hil/`.
