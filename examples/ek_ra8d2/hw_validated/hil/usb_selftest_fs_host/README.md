# usb_selftest_fs_host (USB self-loop, config B)

The role-flipped twin of `usb_selftest_hs_host`. The board's two USB
ports are cabled **to each other** and one firmware image runs BOTH
sides of the link, with host and device **swapped** versus config A.

- **USBHS (J7) = device:** the `usb_msc_mram_hs` Mass-Storage class on
  the USBHS controller, exposing the 1 MiB MRAM window at `0x02000000`
  as a read-only synthesized FAT16 volume (`MRAM.BIN`). It ships both
  the HS and the FS-fallback frameworks.
- **USBFS (J11) = host:** the first-party polled host MSC stack
  (`ra8_usb_hmsc` + `ra8_fs`). It enumerates the HS device over the
  cable, mounts the FAT16 volume, then streams the data region back
  with raw multi-block `READ(10)` and memcmp's every burst against the
  same MRAM bytes read directly.

The link runs at **12 Mbps**: the FS host is the ceiling, so the HS
device **falls back to full speed** -- the USBHS-device-at-full-speed
path, which neither the Mac ladders (HS) nor config A (FS device)
exercised.

## Why this matters

Config A proved HS-host / FS-device. This proves the mirror image:
FS-host / HS-device, AND the HS device controller correctly
negotiating down to full speed. Together the two configs show the USBX
device + first-party host stacks run concurrently on either controller
in either role, returning the chip's flash byte-for-byte.

## Result (validated 2026-06-13 on real hardware)

Fresh-reset console (SCI8 / J-Link OB CDC, 115200):

```
host up on USB-FS, probing the loop...
enumerated vid=0x1209 pid=0x000F over the loop cable
mounted fs=fat16
verified 1048576 bytes vs MRAM in 16895 ms (60 KiB/s)
WRITE(10) into RO LUN must be rejected...
write rejected (code 0x00000204), MRAM protected
USB SELFTEST CONFIG B PASS
```

J-Link probes confirm the device side negotiated full speed:
`s_dbg_ux_speed == 1` (UX_FULL_SPEED_DEVICE), `s_dbg_framework ==`
the FS framework address, `s_dbg_fw_len == 50` (the FS config blob),
`s_dbg_dev_state == 3` (CONFIGURED), `s_dbg_pass_count == 1`,
`s_dbg_verified_bytes == 0x100000`, `s_dbg_mismatch_off == 0xFFFFFFFF`
(byte-perfect). Throughput is lower than config A's 117 KiB/s because
the polled FS host + single-banked HS-device-IN path is slower; the
bytes are identical.

## The fix this app forced (see #67)

The HS DCD seeded `ux_system_slave_speed` (and the current device
framework) to HS at init, before the link speed is known. Against an
FS host the device handed a 512-byte-bulk-MPS descriptor over a
full-speed link, so the bulk pipes never carried a CBW and the storage
class thread never ran `media_read` (device stuck ADDRESSED). The DCD
now mirrors the **settled** `DVSTCTR0.RHST` into both the speed field
and the current framework on every bus reset, so an HS device falls
back to the 64-byte FS framework automatically.

## Known limitation (tracked in #92)

Same as config A: the WRITE(10) rejection leaves the bulk-OUT endpoint
STALLed; full BOT reset + Clear Feature recovery is the robustness
sweep (#92). The pass parks after the single PASS.

## Diagnostics (J-Link, re-resolve with `arm-none-eabi-nm`)

`s_dbg_phase` (::selftest_phase_t), `s_dbg_pass_count`,
`s_dbg_verified_bytes`, `s_dbg_mismatch_off`, `s_dbg_verify_ms`,
`s_dbg_read_calls` (device `media_read` count), plus the FS-fallback
bring-up set: `s_dbg_dev_state`, `s_dbg_ux_speed`, `s_dbg_thr_state` /
`s_dbg_thr_runs` (storage class thread), `s_dbg_state3_seen`,
`s_dbg_framework` / `s_dbg_fw_len` and the `s_dbg_fw_fs_addr` /
`s_dbg_fw_hs_addr` framework-address references for comparison.

## Pinout

HS device: P4_08 USBHS_VBUS sense (PSEL usb_hs), PD07 LOW (J7 = Device,
no U18 back-feed); D+/D- dedicated PHY balls. FS host: P4_07 VBUS
sense, P5_00 VBUSEN peripheral-routed (USBFS sources J11 VBUS),
P8_14/P8_15 data (PSEL usb_fs). Console: PD_02/PD_03 SCI8.

## VID / PID

Device side advertises VID 0x1209, PID 0x000F (config A = 0x000E,
Mac-facing MRAM apps = 0x000C / 0x000D). Bench use only.
