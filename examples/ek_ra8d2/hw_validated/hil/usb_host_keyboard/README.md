# usb_host_keyboard

Validates the first-party USB **host** HID boot-keyboard path with no keyboard
in the room: the board hosts on one jack and emulates a boot-keyboard peripheral
on the other over the loop cable (J7 HS host <-> J11 FS device), one image
running both USB stacks.

The device side is a ThreadX + USBX HID class advertising the standard
boot-keyboard report descriptor (interface subclass 1 / protocol 1) and queueing
8-byte boot reports. The host side is a polled host on `ra8_usb_host_*`: it
enumerates the keyboard, opens the interrupt-IN endpoint, polls reports, and
decodes the keycodes back to ASCII.

The boot descriptor plus the keycode decode is what separates this from
`usb_selftest_hid`, which carries a generic vendor report. Pinout and VID match
that app. The original version needed a real USB keyboard in J7; the self-loop
stands in for it, so this runs unattended.
