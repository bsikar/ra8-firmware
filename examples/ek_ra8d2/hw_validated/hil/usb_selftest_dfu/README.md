# usb_selftest_dfu

The **control-transfer** member of the USB self-loop matrix: host DFU_DNLOAD ->
device capture -> host DFU_UPLOAD -> byte-exact compare, through a real DFU
class. The two USB ports are cabled to each other and one image runs both stacks
-- USBFS (J11) is a ThreadX + USBX DFU class started directly in `dfuIDLE`
(class 0xFE / subclass 0x01 / protocol 0x02), USBHS (J7) a polled DFU host on
the first-party `ra8_usb_host_*` primitives. It is the on-bench evidence that
the device EP0 **control-OUT data stage** works; each block is one full 64-byte
control data stage, MPS-exact against a single DCP bank.

The download closes with **DFU_ABORT** rather than the spec's zero-length
manifest DFU_DNLOAD: this USBX DFU class is not manifestation-tolerant, so after
MANIFEST it parks in `dfuMANIFEST-WAIT-RESET` and only a USB bus reset returns
it to a usable state -- which would tear down the in-place UPLOAD round trip.
DFU_ABORT gets back to `dfuIDLE` with no reset.

## Why the control-OUT receive is split across two interrupts

A control-WRITE data stage cannot be received synchronously in the SETUP ISR:
the FS device ISR and the lower-priority HS host worker share one CPU, so a
blocking receive spins out the very thread that has to send the data -- a
same-CPU deadlock. The SETUP ISR instead *arms* the DCP (`ra8_usb_dcp_out_arm`)
and returns; the following DCP BRDY IRQ drains the bank
(`ra8_usb_dcp_out_read`) and only then runs the chapter-9 dispatcher.

Two register details go with it. On the host the DCP must set `DCPCFG.DIR = 1`
for an OUT data stage, because the default issues IN tokens. On the device,
driving `DCPCTR.SQSET` (DATA1) and clearing a stale `CCPL` before arming is what
makes the SIE accept the data instead of flagging `CTSQ = SQER`.

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data (PSEL
usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18 powers J7),
P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8. The device advertises
VID 0x1209 with a per-app PID; bench use only.
