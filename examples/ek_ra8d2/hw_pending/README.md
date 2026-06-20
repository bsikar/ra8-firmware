# examples/ek_ra8d2/hw_pending/

Apps here compile and pass all CI gates but have **not yet been confirmed
working on hardware end-to-end** -- each is blocked by a peripheral that
isn't on the bench, or by a documented firmware gap. These are the apps to
work through next when the missing hardware is available.

To build: `make <appname>` from the repo root, e.g. `make sd_font_render`.
Move an app to `hw_validated/hil/` (with a `git mv`) once it passes a
hardware HIL probe AND the gap below is resolved.

## Apps and blocking reasons (current as of 2026-06-20)

### Blocked by a peripheral not on the bench

| App | Blocking reason |
|-----|-----------------|
| i3c_i2c_peripheral_demo | I2C/I3C peripheral mode needs an external controller to talk to; the bench has no controller wired up. |
| imu_lsm6dso_demo | Needs an LSM6DSO IMU on the I2C/I3C bus; not fitted. |

### Blocked by a module / firmware gap

| App | Blocking reason |
|-----|-----------------|
| da16600_probe | The DA16600 Wi-Fi/BLE module ships factory firmware that does not answer AT commands; it needs the AT-command SDK flashed first. Not an RA8D2-side wiring issue. |
| tz_nsc_cgc_usb | Needs the real TrustZone secure / non-secure partition (separate S + NS stacks/.bss and a proper BLXNS transition): the NS-pointer veneer check rejects args whose `.data`/`.bss`/stack live in secure SRAM. Tracked in #60 / #54. |
| dtc_transfer_demo | `tools/board_sim` shadows the DTC control window (DTCCR/DTCVBR/DTCST/DTCSTS) but models no descriptor-table transfer engine, so the SRAM-to-SRAM copy reports `match=N` on the emulator and can only be confirmed on silicon. The TI encoding + indirect vector-table model + slot re-arm are host-tested; the bring-up + ELC/DTC activation run headless. Tracked in #124. |
| lvd_monitor_demo | `tools/board_sim` shadows the PVD control window but models no analog voltage comparator, so `PVD1SR.MON` reads back 0 and the banner reports `mon=below ok=N` on the emulator; the live `mon=above` reading needs silicon. The verdict logic + status decode + PRCR.PRC3 unlock word are host-tested; the safe flags-only (`response=none`) bring-up runs headless. Tracked in #125. |
| bkup_survival_demo | The VBTBKRn read/write window passes on `tools/board_sim` (`rw=ok`), but the emulator models no reset-retained VBATT power domain and clears memory each run, so it reports `survived=N boot=1` -- the reset-survival proof needs a real EK-RA8D2 and a reset cycle (boot1 `survived=N` -> reset -> boot2 `survived=Y`). The pattern + verdict + survival decision are host-tested. Tracked in #131. |
| cac_accuracy_demo | `tools/board_sim` shadows the CAC control registers but models no edge counter, so `CASTR.MENDF` never asserts and `ra_cac_measure` returns `k_ra_err_hw_timeout` (`meas=TIMEOUT ok=N`). Measuring the 24 MHz main osc against LOCO/32 (~23437 counts, +/-6% window) needs silicon. The expected-count + window math, the verdict (MC/DC), and the CACR1/CACR2 clock-select are host-tested. Tracked in #126. |

## Recently promoted

The on-board **USB self-loop self-tests** (`usb_selftest_hs_host`, `_fs_host`,
`_cdc`, `_hid`, `_microsd`, `_mlun`, `_wlun`, `_ospi`, `_ospi_rw`, `_soak`) were
validated on real hardware (FS jack cabled to HS jack, the board both hosts and
devices itself) and moved to `hw_validated/hil/`.

`sd_font_render` was reworked to **self-provision** its font through the new
`libs/ra_sdfont` helper: it mounts the Pmod2 microSD, and if `FONT.OTF` is absent
it writes a baked Latin-1 font (`libs/fonts/arnopro_latin1.otf`) to the card and
reads it back -- so any FAT-formatted "random" card just works, no host-side
image prep. Validated on real hardware (`g_sfr_stage` = render_ok, font 404 KB,
`g_sfr_ink` = 1254 inked pixels) and in board_sim against a blank card image
(`mkfontimg --blank`), gated by a `g_sfr_heartbeat` memprobe, and moved to
`hw_validated/hil/`. The same helper now backs `ereader_ui`'s font load.

`tz_secure_only_sd` was also validated -- a real SPI-mode microSD round-trip
(SCI0 Simple-SPI -> `ra_sdmmc_spi` -> `ra_fs`: init, mount, write+read+compare,
`sd: roundtrip ok`) on the Pmod2 card -- and moved to `hw_validated/hil/` with a
`uart_scrape` gate.

`threadx_filex_demo` was rewritten to run FileX over the on-board OSPI flash
(LevelX) instead of the unreachable SDHI card path, validated on real hardware
(format -> FAT -> create/list/read-verify/delete -> `ospi FAT roundtrip ok`), and
moved to `hw_validated/hil/`.

`usb_host_msc_browse` was re-based onto the self-loop (board hosts AND simulates
the MSC peripheral over the J7<->J11 cable, no external drive): the host
enumerates, mounts, and BROWSES the device's FAT root before read-verify
(`USB HOST MSC BROWSE PASS`), and moved to `hw_validated/hil/`.

`usb_host_keyboard` was likewise re-based onto the self-loop: the board simulates
a boot-keyboard device and the host decodes its keycodes back to "RA8D2"
(`USB HOST KEYBOARD PASS`), and moved to `hw_validated/hil/`.

`dfu_bootloader` was **fully bench-validated** end-to-end and moved to
`hw_validated/hil/`. The "needs an external dfu-util" gap is closed: the board is
its own DFU host over the J7<->J11 self-loop via
[`dfu_selftest_boot`](../hw_validated/hil/dfu_selftest_boot), which DFU-flashes +
commits a bootable Slot A; flashing the bootloader afterward (Slot A untouched)
then boots it (J-Link sentinel `0x600D600D` @ `0x22040000`). The bootloader itself
is gated in HIL with an `alive` probe (boots + runs its A/B boot decision without
faulting -- PC in code, CycleCnt advancing, CFSR/HFSR clean), deterministic
regardless of slot state, so it needs no human in the loop.

## Retired

`usb_host_cdc_echo` was **deleted**: its board-only function (a CDC host
enumerating + byte-echoing a CDC-ACM device) is already fully validated by
`hw_validated/hil/usb_selftest_cdc` (the CDC host+device self-loop). The only
things it additionally exercised -- a *real external* CDC peripheral and the
interrupt-driven host path -- need external hardware on J7 plus the USB-host
ICU/NVIC IRQ wiring (#62), so keeping a redundant self-loop duplicate added no
coverage. Its host test and its SRS/SVCP/roadmap entries were removed with it.
