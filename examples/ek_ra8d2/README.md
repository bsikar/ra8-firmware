# examples/ek_ra8d2/

Apps in this tier are validated on a stock **EK-RA8D2 v1** evaluation kit
(Renesas part 968-K7EKA8D2S01001BE) with no extra peripherals beyond what
the board ships with:

- 3 user LEDs, 2 user switches
- 7.0-inch 1024x600 parallel TFT
- OV5640 5 MP camera
- 64 MB Octo-SPI flash, 64 MB SDRAM
- J7 Ethernet
- J10 J-Link OB (SCI8 VCOM)
- J11 USB Type-C Full-Speed
- J12 USB Type-C High-Speed
- MIPI-DSI / Pmod / Arduino headers (no shields)

These are the apps the project hardware-validates every release. The
pre-commit hook and CI run smoke tests against this set; new
infrastructure changes are smoke-tested by re-flashing one app from each
sub-category (LED, UART, USB, Ethernet, ThreadX, GUIX, OTA, etc.).

A handful of host-USB demos (`usb_host_*`) need a cheap USB device
plugged into J12 (a CDC-ACM adapter, a USB keyboard, or any USB mass-
storage stick). Those are still in this tier because the required
device is trivial commodity hardware and not a vendor blob.

To build any app: `make <appname>` from the repo root, e.g. `make blink`.
The bare app name works regardless of which tier it lives in -- the
top-level Makefile auto-discovers apps under `examples/<tier>/<app>/`.

## Companion tier

See [`examples/_unsupported/`](../_unsupported/README.md) for apps that
require hardware we do not have on hand and therefore cannot
hardware-validate.
