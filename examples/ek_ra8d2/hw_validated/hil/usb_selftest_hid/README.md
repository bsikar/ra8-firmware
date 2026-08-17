# usb_selftest_hid

The **interrupt-transfer** member of the USB self-loop matrix: the device
streams input reports on its interrupt-IN endpoint and the host reads and
byte-checks them, through a real HID class. The two USB ports are cabled to each
other and one image runs both stacks -- USBFS (J11) is a ThreadX + USBX HID
class with a vendor 8-byte input report, USBHS (J7) a self-contained polled host
on the first-party `ra8_usb_host_*` primitives. No OS HID driver is involved.

Each report carries a rolling sequence byte plus a fixed pattern, so the host
sees not just correct bodies but *fresh* ones rather than one stale buffer. The
report is well under the 64-byte endpoint MPS, so it arrives as a single short
packet; the host SIE drives the interrupt endpoint as a receive pipe, issuing IN
tokens on demand, which is all a polled report read needs.

The device worker yields a tick between queue attempts so the lower-priority
host thread can drain the report queue -- both live on one CPU.

`usb_host_keyboard` covers the boot-keyboard descriptor and keycode decode
instead. The `s_dbg_*` globals publish phase, round count, the first mismatch
and the last sequence number read, for a J-Link readout.

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data (PSEL
usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18 powers J7),
P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8. The device advertises
VID 0x1209 with a per-app PID; bench use only.
