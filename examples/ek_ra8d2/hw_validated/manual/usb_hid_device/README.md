# usb_hid_device

USBX HID device demo. The board enumerates on USB-FS as a three-button
boot-protocol mouse -- the canonical descriptor from USB HID 1.11 sec E.10 --
and pushes a four-pixel cursor jiggle to the host once a second on the
interrupt-IN pipe, with LED1 toggling per report sent.

It is manual because the proof is on the host: the pointer has to actually move,
which only happens once the host's HID driver has bound and opened the device.

## VBUSEN must be a GPIO driven low

Routing VBUSEN to its peripheral function forces the pin high, which is host
mode, and blocks device enumeration outright. The app configures it as a GPIO
output at low instead.

## Multi-packet EP0 IN is where this class breaks

The HID report descriptor is larger than one control packet, so the host fetches
it in several EP0 IN chunks, and EP0 is single-buffered: after a chunk is
pushed, FRDY does not reassert until the host has actually pulled it. The wait
therefore has to observe the clear-then-reassert transition, and its bound has
to cover host latency. Get that wrong and the symptom is a clean chapter-9
enumeration followed by the host timing out on the descriptor fetch --
`usbhid` reports `-110` -- which looks nothing like a FIFO problem.

`g_usb_hid_match` and `g_usb_hid_mismatch` are readable over SWD. They only
advance once the host has opened the HID device, which cannot happen until the
report-descriptor handshake completes, so they double as a pass indicator on a
run with no screen attached.
