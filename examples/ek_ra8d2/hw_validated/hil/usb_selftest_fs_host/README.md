# usb_selftest_fs_host

The role-flipped twin of `usb_selftest_hs_host`: same self-loop cable, same
single image running both sides of the link, host and device swapped. USBHS (J7)
is the Mass-Storage device exposing the 1 MiB MRAM window at `0x02000000` as a
read-only synthesized FAT16 volume; USBFS (J11) is the first-party polled host
MSC stack (`ra8_usb_hmsc` + `ra8_fs`), which mounts it and memcmp's every raw
multi-block `READ(10)` burst against the same MRAM bytes read directly.

The FS host is the speed ceiling, so the HS device **falls back to full speed**
-- a path neither the Mac-attached ladders (HS) nor the other config (FS device)
exercise. Between the two configs, the USBX device stack and the first-party
host stack are shown running concurrently on either controller in either role,
returning the chip's flash byte for byte.

## The fix this app forced (#67)

The HS DCD used to seed `ux_system_slave_speed`, and the current device
framework, to high speed at init -- before the link speed is knowable. Against
an FS host the device then handed over a 512-byte-bulk-MPS descriptor on a
full-speed link, the bulk pipes never carried a CBW, and the storage class
thread never ran `media_read`: the device sat in ADDRESSED forever. The DCD now
mirrors the **settled** `DVSTCTR0.RHST` into both the speed field and the
current framework on every bus reset, so an HS device drops to the 64-byte FS
framework by itself.

## Known limitation (#92)

As in config A, the WRITE(10) rejection leaves the bulk-OUT endpoint STALLed.
Full BOT reset + Clear Feature ENDPOINT_HALT recovery is not in the host class
yet, so the run parks after a single pass.

## Pinout

HS device: P4_08 USBHS_VBUS sense (PSEL usb_hs), PD07 LOW so J7 is Device with
no U18 back-feed; D+/D- are dedicated PHY balls. FS host: P4_07 VBUS sense,
P5_00 VBUSEN peripheral-routed (USBFS sources J11 VBUS), P8_14/P8_15 data (PSEL
usb_fs). Console: PD_02/PD_03 SCI8. The device advertises VID 0x1209 with a
per-app PID; bench use only.
