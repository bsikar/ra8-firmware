# usb_selftest_microsd (the microSD card as a read-only USB drive)

Brings the board's **Pmod2 microSD card** up as a USB drive and verifies
it against itself on-chip -- no PC in the loop. The two USB ports are
cabled **to each other** and one firmware image runs both USB stacks plus
the SCI0 Simple-SPI SD driver.

- **At boot** the device snapshots **SD LBA 0..63** (the FAT/exFAT boot
  region, 32 KiB) into a RAM buffer with `ra_sdmmc_spi_read_block` and
  confirms the `0x55AA` boot signature. The card is **never written**
  (strictly read-only), so the user's filesystem is untouched.
- **USBFS (J11) = device:** a ThreadX + USBX Mass-Storage class exposes
  that 64-sector window as a single **read-only** logical unit
  (`GET_MAX_LUN` = 0); media-read serves the boot snapshot from RAM (no
  live card access on the class thread).
- **USBHS (J7) = host:** the polled first-party host stack (`ra_usb_hmsc`)
  enumerates the device over the cable, `READ_CAPACITY`, then streams the
  window back with raw multi-block `READ(10)` and byte-checks every
  sector against the SD snapshot.

The host compares the bytes it pulled over USB against the exact image
the device read off the card, so the loop proves the **SD read path AND
the USB transport deliver the card's real content intact** -- without
writing a single sector to the card.

## Result (validated 2026-06-13 on real hardware)

Fresh-reset console (SCI8 / J-Link OB CDC, 115200):

```
ra8d2 microsd: host up on USB-HS, probing the loop...
ra8d2 microsd: enumerated pid=0x0015, GET_MAX_LUN=0
ra8d2 microsd: LUN 0 OK (64 sectors, SD vs snapshot)
ra8d2 microsd: USB SELFTEST MICROSD PASS
```

J-Link probes confirm the card is real and the loop matched:
`s_dbg_pass_count == 1`, `s_dbg_luns_ok == 1`, `s_dbg_max_lun == 0`,
`s_dbg_mismatch == 0xFFFFFFFF` (every sector matched the snapshot),
`s_dbg_sd_err == 0` (card read into the snapshot OK),
`s_dbg_sd_bootsig == 1` (LBA0 carried the `0x55AA` FAT/exFAT signature),
`s_dbg_sd_blocks` = the card capacity in 512-B blocks (e.g. `0x0E8F6800`
for a ~119 GiB card), `s_dbg_dev_step == 5` (device worker parked).

## Read-only by design

The card holds the user's filesystem, so this test deliberately does
**no** `ra_sdmmc_spi_write_block` anywhere -- it snapshots a fixed window
once at boot and serves that read-only. A host-**writable** microSD drive
would need a writable LUN plus the device bulk-OUT WRITE(10) path, which
is tracked with the writable-OSPI / CDC work.

## microSD bring-up (mirrors sd_font_render)

`ra_pfs_route_peripheral` routes the Pmod2 SCK/CIPO/COPI to the SCI async
PSEL; CS is a plain GPIO held low across each SD command (idle high).
`ra_sci_spi_init` opens SCI0 Simple-SPI at the slow init clock;
`ra_sdmmc_spi_init` then enumerates the card and negotiates up.

## Diagnostics (J-Link, re-resolve with `arm-none-eabi-nm`)

`s_dbg_phase`, `s_dbg_pass_count`, `s_dbg_luns_ok`, `s_dbg_max_lun`,
`s_dbg_mismatch`, `s_dbg_read_calls` (device `media_read` count),
`s_dbg_sd_err` (SD bring-up result), `s_dbg_sd_bootsig` (boot signature
seen), `s_dbg_sd_blocks` (card capacity), `s_dbg_dev_step` /
`s_dbg_dev_err` (device-worker progress).

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data
(PSEL usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18
powers J7), P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8.
microSD: Pmod2 SCI0 Simple-SPI (SCK/CIPO/COPI PSEL sci_async, CS GPIO).

## VID / PID

Device side advertises VID 0x1209, PID 0x0015. Bench use only.
