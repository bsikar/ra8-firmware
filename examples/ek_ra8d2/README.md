# examples/ek_ra8d2/

Apps that need nothing beyond a stock **EK-RA8D2 v1** (Renesas
968-K7EKA8D2S01001BE): 3 user LEDs and 2 switches, a 7.0-inch 1024x600 parallel
TFT, an OV5640 camera, 64 MB Octo-SPI flash and 64 MB SDRAM, J7 Ethernet, the
J10 J-Link OB on SCI8 VCOM, J11 Full-Speed and J12 High-Speed USB Type-C, and
the MIPI-DSI / Pmod / Arduino headers with no shields fitted.

They are filed by hardware sign-off status:

| | |
|---|---|
| [`hw_validated/`](hw_validated/README.md) | Carries recorded EVM validation evidence; rerun the named harness before claiming the current commit passes. |
| [`hw_pending/`](hw_pending/README.md) | Compiles and passes CI; not yet confirmed on hardware. |
| [`hil_needs_revalidation/`](hil_needs_revalidation/README.md) | Was validated, then failed a recorded bench run and needs another. |

`just apps::build <appname>` from the repo root builds any of them -- the name works
whichever subtier the app lives in. Apps needing hardware this project does not
own are in [`../_unsupported/`](../_unsupported/README.md).
