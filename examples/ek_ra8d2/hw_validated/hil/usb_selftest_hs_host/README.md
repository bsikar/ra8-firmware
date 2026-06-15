# usb_selftest_hs_host (USB self-loop, config A)

The board's two USB ports are cabled **to each other** and ONE firmware
image runs BOTH sides of the link, then verifies itself on-chip -- no
PC in the loop.

- **USBFS (J11) = device:** the `usb_msc_mram` Mass-Storage class,
  exposing the 1 MiB MRAM window at `0x02000000` as a read-only
  synthesized FAT16 volume (`MRAM.BIN`). IRQ-driven via the
  `port/usbx/ux_dcd_ra_usb` device bridge.
- **USBHS (J7) = host:** the first-party polled host MSC stack
  (`ra_usb_hmsc` + `ra_fs`), in a low-priority ThreadX thread. It
  enumerates the FS device over the cable, mounts the FAT16 volume,
  then streams the data region back with raw multi-block `READ(10)` and
  memcmp's every burst against the same MRAM bytes read directly.

The link runs at **12 Mbps** -- the FS device is the speed ceiling and
the HS host serves a full-speed downstream device (RHST = FS), a path
the Mac-attached ladders never exercised.

## Why this matters

It proves the USBX device stack and the first-party host stack run
**concurrently in one image, one controller each**, and that the SCSI
transport returns the chip's flash byte-for-byte -- the whole USB data
path validated against itself, deterministically, with no external
host.

## Result (validated 2026-06-13 on real hardware)

Console (SCI8 / J-Link OB CDC, 115200) and J-Link probes both confirm:

```
host up on USB-HS, probing the loop...
enumerated vid=0x1209 pid=0x000E over the loop cable
verified 1048576 bytes vs MRAM in 8704 ms (117 KiB/s)
WRITE(10) into RO LUN must be rejected...
write rejected (code 0x00000204), MRAM protected
USB SELFTEST CONFIG A PASS
```

- 1,048,576 bytes (full 1 MiB) read over USB == MRAM `0x02000000`,
  byte-for-byte (`s_dbg_mismatch_off == 0xFFFFFFFF`).
- `s_dbg_pass_count == 1`, `s_dbg_verified_bytes == 0x100000`,
  `s_dbg_verify_ms == 0x2200` (8704 ms).
- WRITE(10) into the read-only LUN is rejected with `0x204`
  (`k_ra_err_hw_error`, a clean bulk-OUT STALL) -- the MRAM is never
  touched.

## Bring-up notes (the two non-obvious fixes)

1. **Pre-kernel SysTick.** `main()` starts SysTick (for `ra_delay_ms`)
   before `tx_kernel_enter`, and this app's setup window is long (the
   U15 expander I2C transaction blocks for ms), so the tick fires
   before ThreadX timer state exists. Feeding `_tx_timer_interrupt`
   then walks a zeroed expiration list and bus-faults. The handler
   gates ThreadX delivery on `s_tx_kernel_up` (set in
   `tx_application_define`).
2. **Thread priority.** The polled host loop busy-waits for device
   data. The USBX device storage **class thread**
   (`UX_THREAD_PRIORITY_CLASS = 20`) actually runs `media_read`, so the
   host worker must sit BELOW it (priority 24) or that class thread can
   never preempt the spin and every bulk read times out -- even though
   enumeration still works (device SETUP is ISR-driven). This was the
   `0x203` mount timeout.

## Known limitation (tracked in #92)

The WRITE(10) rejection leaves the bulk-OUT endpoint STALLed.
Recovering the BOT transport afterwards (Bulk-Only Mass Storage Reset +
Clear Feature ENDPOINT_HALT) is not yet in the host class, so this pass
parks after the single PASS rather than looping. Full STALL/recovery
soak is part of the robustness sweep (issue #92).

## Diagnostics (J-Link, re-resolve with `arm-none-eabi-nm`)

`s_dbg_phase` (::selftest_phase_t), `s_dbg_pass_count`,
`s_dbg_verified_bytes`, `s_dbg_mismatch_off`, `s_dbg_verify_ms`,
`s_dbg_read_calls` (device-side `media_read` count).

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (device role),
P8_14/P8_15 data (PSEL usb_fs). HS host: SW4-8 Host via the U15
expander, PD07 HIGH (U18 powers J7), P4_08 VBUS sense (PSEL usb_hs).
Console: PD_02/PD_03 SCI8.

## VID / PID

Device side advertises VID 0x1209, PID 0x000E (distinct from the
Mac-facing `usb_msc_mram` 0x000C / `usb_msc_mram_hs` 0x000D). Bench use
only.
