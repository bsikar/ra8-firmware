# epaper_refresh

End-to-end example for the e-ink display path (#256). It drives an
IT8951-compatible e-paper panel **entirely through the display PAL** e-ink
backend, which layers on the `ra8_epaper` HAL driver over an injected
`ra8_io_spi_bus` seam. The app itself never names `ra8_epaper_*`: it paints a
canonical RGB565 framebuffer and calls the same six PAL entry points the LCD
demo uses, so swapping an LCD panel for e-ink is a one-line `iface` change.

It performs a full refresh of a checkerboard with the GC16 waveform, then a
partial refresh of a small sub-region with the fast A2 waveform, timing both.
The SPI bus runs at 10 MHz, under the IT8951's 24 MHz ceiling. The timings are
**informational only** -- on silicon a GC16 full refresh is hundreds of
milliseconds and an A2 partial tens, while a modelled controller completes
instantly -- so the verdict never depends on a timing value.

## Blocked on

The EK-RA8D2 ships a 7.0-inch parallel-RGB TFT, not e-paper, so there is no
IT8951 panel on the bench. On-panel validation needs a real IT8951 carrier (a
custom board, or a Waveshare IT8951-AP HAT on the Pmod/SPI header).

`ra8_emulator` models the IT8951 as an SPI device: it self-frames off the SPI
preambles, answers the `HRDY` ready GPIO, drains `GET_DEV_INFO` and returns
LUT-idle, so the genuine `ra8_epaper` -> display-PAL -> `ra8_io_spi_bus` path
runs to its verdict with no panel. **That model is load-bearing.** Run without
it and `HRDY` never asserts, `ra8_epaper_init` times out, and the app honestly
fails -- the fake passes only because the modelled controller responds as
silicon would.

## Pin map (hypothetical carrier)

These are **not** stock EK-RA8D2 board facts, which is why they live in the app
rather than the board layer. They match the pins the emulator's IT8951 model
drives; change the `k_ep_reset_pin` / `k_ep_hrdy_pin` enums (and the emulator's
matching enums) for a different carrier.

| Signal        | Pin        | Direction | Notes                                  |
|---------------|------------|-----------|----------------------------------------|
| IT8951 /RESET | P4_00      | output    | pulsed low->high at init               |
| IT8951 HRDY   | P4_01      | input     | panel "ready" -- polled before each op |
| SPI_B bus     | SPI_B ch0  | --        | routed by the carrier                  |
| Chip select   | SPI_B SSL0 | output    | hardware CS, not managed by `ra8_epaper` |

## Registers

This app touches no MMIO directly: SPI_B sequencing lives in `ra8_spi`, GPIO in
`ra8_port_utils`, and the IT8951 SPI protocol in `ra8_epaper`, which carry the
register-level citations. The wire protocol follows the IT8951 datasheet rev 0.2
Ch 3.4 "SPI Interface" and Ch 4 "Application Note".
