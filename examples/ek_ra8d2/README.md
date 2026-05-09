# examples/ek_ra8d2/

Apps in this tier target a stock **EK-RA8D2 v1** evaluation kit
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

Apps are split into two subtiers based on hardware sign-off status:

| Subtier | Description |
|---------|-------------|
| [`hw_validated/`](hw_validated/README.md) | Flashed and confirmed working on the EVM |
| [`hw_pending/`](hw_pending/README.md) | Compiles and passes CI, but not yet flashed or has a known gap |

To build any app: `make <appname>` from the repo root, e.g. `make blink`.
The bare app name works regardless of which subtier it lives in.

## Companion tier

See [`examples/_unsupported/`](../_unsupported/README.md) for apps that
require hardware we do not have on hand.
