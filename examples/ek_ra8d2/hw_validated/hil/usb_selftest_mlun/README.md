# usb_selftest_mlun

Brings up a **multi-LUN** USB Mass-Storage device and verifies it against itself
on-chip -- no PC in the loop. The two USB ports are cabled to each other and one
image runs both USB stacks. USBFS (J11) exposes two read-only raw block logical
units in a single device, each synthesizing a deterministic per-`(LUN, LBA)`
byte pattern in `media_read`; USBHS (J7) is the polled first-party host stack
(`ra8_usb_hmsc`), which reads `GET_MAX_LUN` and then sweeps each LUN with
`READ_CAPACITY` plus a full raw multi-block `READ(10)`, recomputing the same
formula to check every sector.

No filesystem is involved, so this is a pure read-path test of whether the host
**addresses each logical unit independently**. Because the two LUNs synthesize
distinct data with no shared backing, an addressing bug cannot hide behind
identical bytes.

## Why two LUNs

The vendored USBX Cortex-M33 port defaults `UX_MAX_SLAVE_LUN` to **1**, and that
value sizes the storage class's LUN arrays -- a device declaring more LUNs than
the cap overruns the parameter struct. `cmake/usbx.cmake` raises the cap
tree-wide so the Mass-Storage class can expose more than one unit. Two is the
smallest genuinely-multi-LUN device and is enough to exercise per-LUN addressing
on both sides; single-LUN apps are unaffected, using LUN 0 of a slightly larger
array.

The `s_dbg_*` globals publish phase, LUNs verified, the reported `GET_MAX_LUN`,
the first mismatch as `lun<<24 | sector`, and device-worker progress, for a
J-Link readout.

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data (PSEL
usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18 powers J7),
P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8. The device advertises
VID 0x1209 with a per-app PID; bench use only.
