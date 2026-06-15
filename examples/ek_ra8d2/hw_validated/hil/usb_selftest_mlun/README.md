# usb_selftest_mlun (a multi-LUN USB drive, verified on-chip)

Brings up a **multi-LUN** USB Mass-Storage device and verifies it against
itself on-chip -- no PC in the loop. The two USB ports are cabled **to
each other** and one firmware image runs both USB stacks.

- **USBFS (J11) = device:** a ThreadX + USBX Mass-Storage class exposes
  **two logical units** in a single device (`GET_MAX_LUN` = 1). Each LUN
  is a read-only 256-sector (128 KiB) raw block device whose media-read
  synthesizes a deterministic per-`(LUN, LBA)` byte pattern, so the two
  LUNs return distinct, predictable data.
- **USBHS (J7) = host:** the polled first-party host stack
  (`ra_usb_hmsc`) enumerates the device over the cable, reads
  `GET_MAX_LUN`, then for **each** LUN issues `READ_CAPACITY` + a full
  raw multi-block `READ(10)` sweep and byte-checks every sector against
  that LUN's pattern formula.

No filesystem is involved (raw SCSI `READ(10)` per LUN), so this is a
pure read-path test that proves the host correctly **addresses each
logical unit independently** and gets the right data from each, end to
end on chip.

## Why two LUNs

The vendored USBX Cortex-M33 port (`ux_port.h`) defaults
`UX_MAX_SLAVE_LUN` to **1**, which sizes the storage class LUN arrays;
a device that declares more LUNs than that overruns the parameter
struct. `cmake/usbx.cmake` raises the cap (`RA_USBX_MAX_SLAVE_LUN`, the
`-DUX_MAX_SLAVE_LUN` define) to **2** for the whole tree so the
Mass-Storage class can expose more than one logical unit. Two LUNs is
the smallest genuinely-multi-LUN device (`GET_MAX_LUN` = 1) and is
enough to exercise per-LUN addressing on both the device and the host;
single-LUN apps are unaffected (they use LUN 0 of a slightly larger
array).

## Result (validated 2026-06-13 on real hardware)

Fresh-reset console (SCI8 / J-Link OB CDC, 115200):

```
ra8d2 mlun: host up on USB-HS, probing the loop...
ra8d2 mlun: enumerated pid=0x0013, GET_MAX_LUN=1
ra8d2 mlun: LUN 0 OK (256 sectors, pattern verified)
ra8d2 mlun: LUN 1 OK (256 sectors, pattern verified)
ra8d2 mlun: USB SELFTEST MULTI-LUN PASS
```

(The host worker retries until the device has attached, so an early
`host up` line may print before the successful pass.)

J-Link probes confirm the run is real and complete:
`s_dbg_pass_count == 1`, `s_dbg_luns_ok == 2` (both LUNs verified),
`s_dbg_max_lun == 1` (device reported `GET_MAX_LUN` = 1),
`s_dbg_mismatch == 0xFFFFFFFF` (every sector of every LUN matched),
`s_dbg_read_calls == 0x80` (128 device-side `media_read` calls across
both LUNs), `s_dbg_dev_step == 5` (device worker brought USBX, the
class register, the DCD, and the attach up, then parked) and
`s_dbg_dev_err == 0`.

## Pattern formula

Byte `i` of LUN `lun`, LBA `lba` is
`(lun * 97 + lba * 7 + i + 0x5A) & 0xFF`. The device synthesizes it in
`media_read`; the host recomputes the same formula to check the data,
so the two LUNs are byte-for-byte distinct and there is no shared
backing to mask an addressing bug.

## Diagnostics (J-Link, re-resolve with `arm-none-eabi-nm`)

`s_dbg_phase` (1 init, 2 enum, 3 verify, 4 pass), `s_dbg_luns_ok`,
`s_dbg_max_lun`, `s_dbg_mismatch` (`lun<<24 | sector` on first
mismatch), `s_dbg_pass_count`, `s_dbg_read_calls` (device `media_read`
count), `s_dbg_dev_step` (device-worker progress: 1 stack, 2 class,
3 dcd, 4 attach, 5 parked), `s_dbg_dev_err` (device-worker first
failing return code).

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data
(PSEL usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18
powers J7), P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8.

## VID / PID

Device side advertises VID 0x1209, PID 0x0013. Bench use only.
