# usb_host_keyboard (USB host HID boot-keyboard over the self-loop)

Validates the first-party USB **host** HID keyboard path with no real keyboard:
the board hosts on one jack and **emulates a boot-keyboard peripheral** on the
other over the loop cable (J7 HS host <-> J11 FS device). One image runs both
USB stacks.

- **USBFS (J11) = device (emulated keyboard):** a ThreadX + USBX HID class
  advertising the standard boot-keyboard report descriptor (interface subclass 1
  / protocol 1). A worker queues the 8-byte boot report
  `[modifier][reserved][keycode x6]` with the keycodes for "RA8D2".
- **USBHS (J7) = host:** a polled host on `ra8_usb_host_*`. It enumerates the
  keyboard, opens the interrupt-IN endpoint, polls reports, verifies the body,
  and **decodes the keycodes (bytes 2..) back to ASCII**.

The boot-keyboard descriptor + keycode decode is what distinguishes this from
`usb_selftest_hid` (a generic vendor report). The original version needed a real
USB keyboard in J7; the self-loop stands in for it.

## Result (validated 2026-06-15 on real hardware)

```
host up on USB-HS, probing the loop...
enumerated pid=0x0018
host decoded keys "RA8D2" over 8 reports -- USB HOST KEYBOARD PASS
```

## HIL

`uart_scrape` gate on `USB HOST KEYBOARD PASS`. Board-only (loop cable fitted);
no external keyboard. Pinout / VID identical to `usb_selftest_hid`.
