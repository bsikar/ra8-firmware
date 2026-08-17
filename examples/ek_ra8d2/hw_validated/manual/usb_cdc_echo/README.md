# usb_cdc_echo

ThreadX + USBX CDC ACM echo on the board's on-board USB-FS receptacle (J11),
through the project's USBX device-controller bridge onto the hand-written
`ra8_usb` driver (HUM Ch 36 "USBFS"). USBX's chapter-9 state machine answers
SETUP packets through that bridge, which is what makes the host enumerate the
device at all; the worker thread then loops read-then-write, and LED1 toggles
per byte echoed.

`threadx_usbx_cdc_demo` is the same echo test through the same stack.

It is manual because the verdict is at the other end of the cable: a PC has to
open the enumerated serial port, type into it, and see the bytes come back. J10
carries the J-Link OB, J11 is the device under test.
