# usb_selftest_wlun (a writable USB drive, verified on-chip)

The **write-path** counterpart to the read-only MSC self-loops. It
exercises the host->device bulk-OUT data phase (SCSI `WRITE(10)`) and is
the on-bench validation for the device bulk-OUT WRITE(10) driver fix. The
two USB ports are cabled **to each other** and one firmware image runs
both USB stacks.

- **USBFS (J11) = device:** a ThreadX + USBX Mass-Storage class exposes a
  single **writable** logical unit (`GET_MAX_LUN` = 0) backed by a
  64-sector (32 KiB) RAM disk. `media_write` copies host data into the
  RAM disk; `media_read` serves it back.
- **USBHS (J7) = host:** the polled first-party host stack (`ra8_usb_hmsc`)
  enumerates the device, `WRITE(10)`s a deterministic per-LBA pattern
  across the whole disk in 8-block bursts, then `READ(10)`s it back and
  byte-checks every sector against the same pattern.

Because the host writes the data and reads it back, the loop proves the
device **bulk-OUT WRITE data phase round-trips intact over USB**, end to
end on chip -- the capability that gates writable OSPI, CDC, and HID.

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
ra8d2 wlun: host up on USB-HS, probing the loop...
ra8d2 wlun: enumerated pid=0x0014, GET_MAX_LUN=0
ra8d2 wlun: LUN 0 OK (64 sectors, write+read verified)
ra8d2 wlun: USB SELFTEST WRITABLE-LUN PASS
```

J-Link probes confirm the round-trip: `s_dbg_pass_count == 1`,
`s_dbg_luns_ok == 1`, `s_dbg_max_lun == 0`,
`s_dbg_mismatch == 0xFFFFFFFF` (every written sector read back equal),
`s_dbg_dev_step == 5` (device worker parked), `s_dbg_dev_err == 0`.
Deterministic across resets (3/3). The five read-only self-loops stay
green with the same driver fix.

## Diagnostics (J-Link, re-resolve with `arm-none-eabi-nm`)

`s_dbg_phase` (1 init, 2 enum, 3 verify, 4 pass), `s_dbg_luns_ok`,
`s_dbg_max_lun`, `s_dbg_mismatch` (first differing sector),
`s_dbg_pass_count`, `s_dbg_read_calls` / `s_dbg_write_calls` /
`s_dbg_write_blocks` (device media callbacks), `s_dbg_dev_step` /
`s_dbg_dev_err` (device-worker progress).

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data
(PSEL usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18
powers J7), P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8.

## VID / PID

Device side advertises VID 0x1209, PID 0x0014. Bench use only.
