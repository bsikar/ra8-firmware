# usb_selftest_ospi_rw (a writable USB drive on OSPI flash, verified on-chip)

The **non-volatile write-path** counterpart to the read-only MSC
self-loops. It exercises the host->device bulk-OUT data phase (SCSI
`WRITE(10)`) landing on the onboard OSPI flash, and is the on-bench
validation for the device bulk-OUT WRITE(10) driver fix against
*persistent storage* (not just RAM). The two USB ports are cabled **to
each other** and one firmware image runs both USB stacks.

- **USBFS (J11) = device:** a ThreadX + USBX Mass-Storage class exposes a
  single **writable** logical unit (`GET_MAX_LUN` = 0) backed by a
  64-sector (32 KiB) window of the onboard OSPI flash (IS25LX512M at xSPI
  CS1, offset `0x00200000`). `media_write` programs host data into the
  flash window with `ra8_xspi_flash_program`; `media_read` serves it back
  with `ra8_xspi_flash_read`.
- **USBHS (J7) = host:** the polled first-party host stack (`ra8_usb_hmsc`)
  enumerates the device, `WRITE(10)`s a deterministic per-LBA pattern
  across the whole window in 8-block bursts, then `READ(10)`s it back and
  byte-checks every sector against the same pattern.

Because the host writes the data and reads it back *off real flash*, the
loop proves the device **bulk-OUT WRITE data phase round-trips intact
onto non-volatile storage**, end to end on chip -- item 4 of the USB
self-loop matrix (writable OSPI).

## OSPI window and the program-only write path

The device worker provisions OSPI once at boot
(`ospirw_ospi_provision`): activate the octo-SPI mux via the U15 I/O
expander, init the xSPI pins, `ra8_xspi_init` in 1S-1S-1S, read the JEDEC
id (expect `0x009D5A1A`), then **erase the 32 KiB window once** (eight
4 KiB sectors at `0x00200000`). Per-WRITE(10) the callback is therefore a
fast **program-only** path -- no per-write 4 KiB erase, which would blow
the host's BOT timeout. The window (`0x00200000`) is a scratch region
clear of `flash_journal` (offset 0) and the read-only OSPI image
(`usb_selftest_ospi`, offset `0x00100000`); it is erased then rewritten
every run, so the test never touches another app's data.

## The driver fix this validates

A host->device bulk-OUT data phase needs four things working together
(all landed in `ra8_usb` / the DCD / `cmake/usbx.cmake`):

1. **Host MPS** -- chunk the data-out at the device's enumerated endpoint
   `wMaxPacketSize` (FS = 64), not the host controller's speed ceiling
   (HS = 512). `ra8_usb_host_bulk_out` ships one packet per call.
2. **Device DBLB** -- double-buffer the device bulk-OUT pipe so the host's
   next packet lands in bank B while the ISR drains bank A.
3. **Loop-drain** -- the DCD drains every ready OUT bank per interrupt.
4. **Single-chunk buffer** -- `UX_SLAVE_REQUEST_DATA_MAX_LENGTH = 4096`
   keeps a typical WRITE in one device transfer (no inter-chunk gap).

## Result (validated 2026-06-13 on real hardware)

Fresh-reset console (SCI8 / J-Link OB CDC, 115200):

```
ra8d2 ospirw: host up on USB-HS, probing the loop...
ra8d2 ospirw: enumerated pid=0x0016, GET_MAX_LUN=0
ra8d2 ospirw: LUN 0 OK (64 sectors, write+read verified)
ra8d2 ospirw: USB SELFTEST WRITABLE-OSPI PASS
```

J-Link probes confirm the round-trip onto flash: `s_dbg_pass_count == 1`,
`s_dbg_ospi_prov == 0` (window provisioned + erased),
`s_dbg_ospi_id == 0x009D5A1A` (IS25LX512M JEDEC id), `s_dbg_luns_ok == 1`,
`s_dbg_max_lun == 0`, `s_dbg_mismatch == 0xFFFFFFFF` (every written
sector read back equal), `s_dbg_dev_step == 5` (device worker parked),
`s_dbg_dev_err == 0`. Deterministic across resets (3/3). The five
read-only self-loops stay green with the same driver fix.

## Diagnostics (J-Link, re-resolve with `arm-none-eabi-nm`)

`s_dbg_phase` (1 init, 2 enum, 3 verify, 4 pass),
`s_dbg_ospi_prov` (provision verdict; 0 = OK),
`s_dbg_ospi_id` (JEDEC id readback), `s_dbg_luns_ok`, `s_dbg_max_lun`,
`s_dbg_mismatch` (first differing sector), `s_dbg_pass_count`,
`s_dbg_read_calls` / `s_dbg_write_calls` / `s_dbg_write_blocks` (device
media callbacks), `s_dbg_dev_step` / `s_dbg_dev_err` (device-worker
progress).

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data
(PSEL usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18
powers J7), P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8.
OSPI: xSPI CS1 to the onboard IS25LX512M via the U15 octo-SPI mux.

## VID / PID

Device side advertises VID 0x1209, PID 0x0016. Bench use only.
