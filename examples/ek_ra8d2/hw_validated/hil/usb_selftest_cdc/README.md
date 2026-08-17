# usb_selftest_cdc

The **bidirectional bulk** member of the USB self-loop matrix: host bulk-OUT ->
device receive -> device echo -> host bulk-IN, through a real CDC-ACM class. The
two USB ports are cabled to each other and one image runs both stacks -- USBFS
(J11) is the ThreadX + USBX CDC-ACM device, USBHS (J7) a self-contained polled
CDC host built straight on the first-party `ra8_usb_host_*` primitives. No
serial terminal is involved; raw bulk transfers only. It is the on-bench
evidence that the device bulk-OUT WRITE(10) driver fix carries over to a non-MSC
class.

The echo runs in a USBX worker thread (read then write), not the DCD ISR
auto-echo. Auto-echo storms when both stacks share one CPU; the worker path
rides the normal device bulk-OUT receive that the WRITE(10) fix repaired.

Each round ships a sub-MPS payload, so the echo returns as a single short packet
and there is no MPS-exact ZLP ambiguity on the host bulk-IN.

The `s_dbg_*` globals publish phase, round count and the first mismatching round
for a J-Link readout.

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data (PSEL
usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18 powers J7),
P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8. The device advertises
VID 0x1209 with a per-app PID; bench use only.
