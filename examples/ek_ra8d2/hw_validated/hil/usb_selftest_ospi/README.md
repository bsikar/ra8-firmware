# usb_selftest_ospi (the onboard OSPI flash as a USB drive)

Brings the board's **64 MiB Octo-SPI flash (IS25LX512M, U16)** up as a
USB drive and verifies it against itself on-chip -- no PC in the loop.
The two USB ports are cabled **to each other** and one firmware image
runs both USB stacks plus the xSPI flash.

- **At boot** the device side ERASES + PROGRAMS a 1 MiB region of the
  OSPI (offset `0x100000`) with a deterministic, sector-derived pattern
  via `ra8_xspi` -- so the flash genuinely holds known content.
- **USBFS (J11) = device:** a ThreadX + USBX Mass-Storage class exposes
  that OSPI region as a read-only synthesized FAT16 volume with one file
  `OSPI.BIN`; media-read pulls each sector straight off the flash with
  `ra8_xspi_flash_read`.
- **USBHS (J7) = host:** the polled first-party host stack (`ra8_usb_hmsc`
  + `ra8_fs`) enumerates the device over the cable, mounts the volume,
  streams the data region back with raw multi-block `READ(10)`, and
  checks every sector against the SAME pattern formula.

The host recomputes the expected pattern rather than reading the flash,
so the single xSPI controller has exactly one user (the device class
thread) -- no contention -- yet the loop proves the OSPI **erase +
program + read round-trips intact over USB**.

## Result (validated 2026-06-13 on real hardware)

Fresh-reset console (SCI8 / J-Link OB CDC, 115200):

```
host up on USB-HS, probing the loop...
enumerated vid=0x1209 pid=0x0010 over the loop cable
mounted fs=fat16
verified 1048576 bytes vs OSPI pattern in 13568 ms (75 KiB/s)
WRITE(10) into RO LUN must be rejected...
write rejected (code 0x00000204), OSPI window read-only
USB SELFTEST OSPI PASS
```

J-Link probes confirm the flash is real and was provisioned:
`s_dbg_ospi_id == 0x009D5A1A` (ISSI IS25LX512M JEDEC id),
`s_dbg_ospi_prov == 0` (erase + program of the 1 MiB window succeeded),
`s_dbg_pass_count == 1`, `s_dbg_verified_bytes == 0x100000`,
`s_dbg_mismatch_off == 0xFFFFFFFF` (every sector matched the pattern).

## OSPI bring-up (mirrors flash_journal, #44)

`ra8_board_io_expander_set_octospi_active` (courtesy) ->
`ra8_board_xspi_pins_init` (OCTA pins PSEL 0x1C + RESET pulse on the
IS25LX512M, xSPI CS1) -> `ra8_xspi_init(0, 1S-1S-1S)`. Addressing is
0-based into the chip; the test window at `0x100000` is clear of
flash_journal's offset-0 record. Erase granularity is the 4 KiB sector
(256 erases for 1 MiB).

## Scope / follow-ons

This exposes OSPI as a **read-only** drive (the host verifies content
the device wrote). A host-**writable** OSPI drive -- where the host
formats it and writes files that persist across power cycles -- needs a
read-modify-erase-write backing (4 KiB erase granularity vs 512-byte
LBAs) and is tracked under #56 along with the microSD and multi-LUN
backings. WRITE(10) STALL recovery is #92.

## Diagnostics (J-Link, re-resolve with `arm-none-eabi-nm`)

`s_dbg_phase`, `s_dbg_pass_count`, `s_dbg_verified_bytes`,
`s_dbg_mismatch_off`, `s_dbg_verify_ms`, `s_dbg_read_calls` (device
`media_read` count), `s_dbg_ospi_id` (JEDEC), `s_dbg_ospi_prov`
(provisioning result).

## Pinout

OSPI: OCTA pins (PSEL 0x1C), xSPI CS1 (IS25LX512M). FS device: P4_07
VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data (PSEL usb_fs). HS
host: SW4-8 Host via the U15 expander, PD07 HIGH (U18 powers J7), P4_08
VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8.

## VID / PID

Device side advertises VID 0x1209, PID 0x0010. Bench use only.
