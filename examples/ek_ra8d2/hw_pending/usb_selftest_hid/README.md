# usb_selftest_hid (a HID interrupt-IN report loop, verified on-chip)

The **interrupt-transfer** entry in the self-loop matrix. It drives the
device->host interrupt report path through a real **HID device class**:
the device streams input reports on its interrupt-IN endpoint and the
host reads + byte-checks them. The two USB ports are cabled **to each
other** and one firmware image runs both USB stacks.

- **USBFS (J11) = device:** a ThreadX + USBX HID class with a vendor
  8-byte input report. A worker continuously queues reports with
  `_ux_device_class_hid_event_set` on the interrupt-IN endpoint; each
  report is `{ rolling seq, fixed 7-byte pattern }`. The worker yields a
  tick between queue attempts so the lower-priority host thread can drain
  the report queue.
- **USBHS (J7) = host:** a self-contained polled host built on the
  first-party `ra_usb_host_*` primitives (no `ra_usb_hhid`). It enumerates
  the device (bus reset -> GET_DESCRIPTOR -> SET_ADDRESS ->
  SET_CONFIGURATION), opens the HID interrupt-IN endpoint (EP1 IN) as a
  receive pipe, then polls 8 reports and byte-checks the fixed pattern in
  each. The rolling seq advances per report, so the host also sees the
  reports are fresh (not one stale buffer).

Each report is 8 bytes, well under the 64-byte endpoint MPS, so it
arrives as one short packet. The device endpoint is interrupt-type; the
host SIE drives it as a receive pipe (issues IN tokens on demand), which
is all the polled report read needs. No OS HID driver is involved.

## Result (validated 2026-06-13 on real hardware)

Fresh-reset console (SCI8 / J-Link OB CDC, 115200):

```
ra8d2 hid: host up on USB-HS, probing the loop...
ra8d2 hid: enumerated pid=0x0018
ra8d2 hid: 8 reports verified -- USB SELFTEST HID PASS
```

J-Link probes confirm the report stream: `s_dbg_pass_count == 1`,
`s_dbg_rounds_ok == 8`, `s_dbg_pid == 0x0018`,
`s_dbg_mismatch == 0xFFFFFFFF` (every report body matched),
`s_dbg_dev_step == 5` (send loop live), `s_dbg_dev_err == 0`,
`s_dbg_dev_sent` advancing (device queued > 8 reports), `s_dbg_last_seq`
advancing across reads (fresh reports). Deterministic across fresh
resets. The committed MSC + CDC self-loops stay green.

## Diagnostics (J-Link, re-resolve with `arm-none-eabi-nm`)

`s_dbg_phase` (1 init, 2 enum, 3 read reports, 4 pass), `s_dbg_rounds_ok`,
`s_dbg_pid`, `s_dbg_mismatch` (first differing report round),
`s_dbg_pass_count`, `s_dbg_dev_step` / `s_dbg_dev_err` (device-worker
progress), `s_dbg_dev_sent` (reports queued) / `s_dbg_last_seq` (seq of
the last report the host read).

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data
(PSEL usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18
powers J7), P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8.

## VID / PID

Device side advertises VID 0x1209, PID 0x0018. Bench use only.
