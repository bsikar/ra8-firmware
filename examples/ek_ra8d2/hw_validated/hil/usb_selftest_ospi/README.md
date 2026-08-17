# usb_selftest_ospi

Brings the board's onboard Octo-SPI flash (IS25LX512M at U16) up as a USB drive
and verifies it against itself on-chip -- no PC in the loop. The two USB ports
are cabled to each other and one image runs both USB stacks plus the xSPI flash.

At boot the device erases and programs a 1 MiB OSPI region at offset `0x100000`
with a deterministic, sector-derived pattern through `ra8_xspi`, so the flash
genuinely holds known content. USBFS (J11) exposes that region as a read-only
synthesized FAT16 volume whose media-read pulls each sector straight off the
flash. USBHS (J7) is the polled first-party host stack (`ra8_usb_hmsc` +
`ra8_fs`), which mounts the volume, streams the data region back over raw
multi-block `READ(10)`, and checks every sector against the same pattern
formula.

The host **recomputes** the expected pattern rather than reading the flash
itself, so the single xSPI controller has exactly one user -- the device class
thread -- and there is no contention, while the loop still proves the OSPI erase
+ program + read round-trips intact over USB.

## OSPI bring-up (mirrors flash_journal, #44)

`ra8_board_io_expander_set_octospi_active`, then `ra8_board_xspi_pins_init`
(OCTA pins PSEL 0x1C plus a RESET pulse on the IS25LX512M, xSPI CS1), then
`ra8_xspi_init` in 1S-1S-1S. Addressing is 0-based into the chip, and the test
window at `0x100000` is clear of `flash_journal`'s offset-0 record. Erase
granularity is the 4 KiB sector.

## Scope

This exposes OSPI **read-only**: the host verifies content the device wrote. A
host-writable OSPI drive that the host formats and writes files into, persisting
across power cycles, needs a read-modify-erase-write backing (4 KiB erase
granularity against 512-byte LBAs) and is tracked under #56 alongside the
microSD and multi-LUN backings. WRITE(10) STALL recovery is #92.

## Pinout

OSPI: OCTA pins (PSEL 0x1C), xSPI CS1 (IS25LX512M). FS device: P4_07 VBUS sense,
P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data (PSEL usb_fs). HS host: SW4-8 Host via
the U15 expander, PD07 HIGH (U18 powers J7), P4_08 VBUS sense (PSEL usb_hs).
Console: PD_02/PD_03 SCI8. The device advertises VID 0x1209 with a per-app PID;
bench use only.
