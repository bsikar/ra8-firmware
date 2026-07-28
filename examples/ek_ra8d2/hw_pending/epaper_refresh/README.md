# epaper_refresh -- IT8951 e-paper full + partial refresh via the display PAL

Minimal end-to-end example for the e-ink display path (#256). It drives an
IT8951-compatible e-paper panel **entirely through the display PAL** e-ink
backend (`k_display_backend_eink_it8951`), which layers on the `ra8_epaper` HAL
driver over an injected `ra8_io_spi_bus` seam. The app itself never names
`ra8_epaper_*`; it paints a canonical RGB565 framebuffer and calls the same six
PAL entry points the LCD demo (`display_pal_animation`) uses -- so swapping the
LCD panel for e-ink is a one-line `iface` change.

## What it does

1. CGC + MSTP + SysTick + SCI8 console + LED bring-up.
2. Configure the panel's `/RESET` (output) and `HRDY` (input) GPIOs, bring up
   SPI_B channel 0 (mode 0, 10 MHz -- under the IT8951's 24 MHz ceiling), and
   bind it into the `ra8_epaper` bus seam via `ra8_io_spi_bus_bind_spi_b` +
   `ra8_io_spi_bus_as_ops`.
3. `display_init` with the e-ink backend and the IT8951 descriptor
   (`ra8_epaper_cfg_t`) carried in `display_cfg_t.panel_timing`.
4. **Full refresh:** paint a 128x128 checkerboard test pattern and
   `display_flush` the whole framebuffer with `k_display_refresh_quality`
   (GC16 waveform), timing the flush over SysTick.
5. **Partial refresh:** blacken a 32x32 sub-region at (48,48), bounds-check it,
   then `display_flush` just that rectangle with `k_display_refresh_fast`
   (A2 waveform), again timing it.
6. `display_deinit` sleeps the panel.

On a clean run the app prints:

```
epaper: init ok panel=128x128
epaper: full-refresh ms=<t> px=16384
epaper: partial-refresh ms=<t> px=1024
epaper: PASS
```

Any failing stage prints `epaper: FAIL` and parks the CPU with the red LED on.

The `ms=` timing is **informational**: on silicon a GC16 full refresh is
hundreds of milliseconds and an A2 partial tens, but under `ra8_emulator` the
modelled controller completes instantly, so the numbers are near zero. The PASS
verdict never depends on a timing value.

## ra8_emulator gate

`tools/ra8_emulator` models the IT8951 as an SPI device attached with `--eink`
(`board_periph_eink.c`). The model self-frames off the IT8951 SPI preambles
(`0x6000` command / `0x0000` data write / `0x1000` data read), answers the
`HRDY` "ready" GPIO, drains `GET_DEV_INFO`, and returns `0` for the `LUTAFSR`
"LUT idle" poll -- so the firmware's genuine
`ra8_epaper` -> display-PAL e-ink -> `ra8_io_spi_bus` path runs to its PASS
banner with no panel. The gate command is:

```
bash scripts/emu/smoke.sh epaper_refresh
```

Expected: `OK (IT8951 e-paper: epaper: PASS)`. The emulator's end-of-run summary
also prints an IT8951 line, e.g.
`IT8951 e-ink  : 17408 pixel(s) loaded, 2 refresh(es), last wf=0x4`
(16384 full + 1024 partial pixels, two refreshes, last waveform A2 = 0x4),
confirming the load + display path actually ran.

**The model is load-bearing (EIL == HIL):** run without `--eink` and the panel's
`HRDY` never asserts, so `ra8_epaper_init` times out and the app honestly prints
`epaper: FAIL`. The fake only passes because the modelled controller responds
exactly as silicon would.

## Why this is in hw_pending

The EK-RA8D2 ships a 7.0-inch **parallel-RGB TFT**, not e-paper, so there is no
IT8951 panel on the bench. `ra8_emulator` proves the whole firmware path headlessly,
but on-panel HIL is pending a real IT8951 carrier (a custom board or a Waveshare
IT8951-AP HAT on the Pmod/SPI header). Until that hardware exists this app can
compile, pass every CI gate, and pass the ra8_emulator gate, but cannot be
hardware-confirmed -- so it lives here rather than in `hw_validated/hil/`.

## Pin map (hypothetical carrier)

These are **not** stock EK-RA8D2 board facts, so they live in the app rather than
the board layer, and they match the pins the ra8_emulator IT8951 model drives:

| Signal        | Pin   | Direction | Notes                                  |
|---------------|-------|-----------|----------------------------------------|
| IT8951 /RESET | P4_00 | output    | pulsed low->high at init               |
| IT8951 HRDY   | P4_01 | input     | panel "ready" -- polled before each op |
| SPI_B SCK/COPI/CIPO | SPI_B ch0 defaults | -- | routed by the carrier; ra8_emulator needs no routing |
| Chip select   | SPI_B SSL0 | output | hardware chip select (not managed by `ra8_epaper`) |

## On-silicon bench plan

1. Wire an IT8951 e-paper controller to SPI_B ch0 with `/RESET` on P4_00 and
   `HRDY` on P4_01 (adjust the `k_ep_reset_pin` / `k_ep_hrdy_pin` enums in
   `main.c` and the matching `k_eink_hrdy_*` enums in
   `tools/ra8_emulator/src/periph/board_periph_eink.c` if a different pinout is used).
2. `make epaper_refresh` from the repo root, then:
   ```
   make -C examples/ek_ra8d2/hw_pending/epaper_refresh flash
   ```
3. Open a serial terminal at 115200 baud on the J-Link OB CDC port.
4. Confirm the checkerboard appears, the 32x32 region turns black on the partial
   refresh, and `epaper: PASS` prints.
5. Move the app to `hw_validated/hil/` with `git mv` and add a `hil.conf`.

## Registers (HUM R01UH1065EJ0130 Rev.1.30)

This app touches no MMIO directly: SPI_B sequencing lives in `ra8_spi`, GPIO in
`ra8_port_utils`, and the IT8951 SPI protocol in `ra8_epaper` -- the
register-level HUM citations are in those library files. The IT8951 wire
protocol follows the IT8951 datasheet rev 0.2 chapter 3.4 "SPI Interface" +
chapter 4 "Application Note" (cited in `libs/ra8_hal/src/ra8_epaper.c`).
