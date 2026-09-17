# threadx_usbx_cdc_demo

ThreadX + USBX CDC ACM echo on the board's on-board USB-FS receptacle (J11),
running through the project's USBX device-controller bridge onto the
hand-written `ra8_usb` driver (HUM Ch 36 "USBFS"). The host enumerates the
device because USBX's chapter-9 state machine answers SETUP packets through that
bridge. Every byte received on bulk-OUT is echoed back and LED1 toggles per
byte, so typing at the far end produces a visible blink pattern.

`usb_cdc_echo` is the same echo test through the same stack.

It is manual because the verdict is at the other end of the cable: a PC has to
open the enumerated serial port, type into it, and see the bytes come back. Two
cables are involved -- J10 for the J-Link OB, J11 for the device under test.

The four-pin USB-FS set this app routes is the only routing the chip exposes for
J11 (EK-RA8D2 v1 UM Table 22 "USB Full Speed Port Pin Assignments" p 30); LED1
is per EK-RA8D2 v1 UM Table 24 p 31.
