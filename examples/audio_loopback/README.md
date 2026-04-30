# audio_loopback

SSIE0 I2S audio loopback demo for the EK-RA8D2. Brings the chip up,
configures SSIE0 in master full-duplex I2S mode (16-bit data inside
a 32-bit system word), and copies samples from the RX FIFO into the
TX FIFO so anything coming in on **AUDIO IN** comes back out on
**AUDIO OUT**.

## Status: untested on hardware

The application compiles clean and exercises the full driver API
end-to-end (`ra_ssie_init`, `ra_ssie_start`, `ra_ssie_get_status`,
`ra_ssie_read_sample`, `ra_ssie_write_sample`,
`ra_ssie_start_recovery`) but the EK-RA8D2 v1 audio jack to SSIE0 pin
fan-out was **not** confirmed against the User's Manual at authoring
time. Update `k_audio_loopback_pin_*` in `main.c` once the manual
table is checked.

## Where to plug in

The EK-RA8D2 board has two 3.5 mm phone jacks labelled:

- **AUDIO IN**  -- line-level stereo input that feeds the codec
  IN_L / IN_R after AC coupling. The codec digitises and presents
  the samples on SDI of SSIE0.
- **AUDIO OUT** -- line-level stereo output coming from the codec's
  DAC. The codec consumes samples from SDO of SSIE0.

Wire a smartphone or signal generator into AUDIO IN, headphones or
a powered speaker into AUDIO OUT, and the firmware should pass the
signal through with one sample of latency per side.

## Pinout (placeholder)

```
TXD8       -- PD_02   (PFS PSEL = k_ra_psel_sci_async, 0x04)
RXD8       -- PD_03   (PFS PSEL = k_ra_psel_sci_async, 0x04)
AUDIO_MCK  -- P5_00   (PFS PSEL = 0x0E)   /* TODO: confirm */
SSIBCK0    -- P5_01   (PFS PSEL = 0x0E)   /* TODO: confirm */
SSILRCK0   -- P5_02   (PFS PSEL = 0x0E)   /* TODO: confirm */
SSIRXD0    -- P5_03   (PFS PSEL = 0x0E)   /* TODO: confirm */
SSITXD0    -- P5_04   (PFS PSEL = 0x0E)   /* TODO: confirm */
LED1       -- P6_00   (GPIO output, low at reset)
```

The PSEL code 0x0E for SSIE pins is from HUM Ch 20.4 "Peripheral I/O
Table". Once `ra_gpio_constants.h` grows a `k_ra_psel_ssie` constant
the placeholder cast in `main.c` should be replaced with the named
enumerator.

## Build + flash

From the repo root:

```sh
make audio_loopback                       # cross-compile
make -C examples/audio_loopback flash     # flash via on-board J-Link OB
```

Or standalone:

```sh
cd examples/audio_loopback/
make
make flash
make clean
```

## What the firmware does

1. `ra_cgc_init()` brings up XTAL + PLL1 (CPUCLK0 = 1 GHz, PCLKA =
   125 MHz).
2. `ra_pfs_route_peripheral()` routes `PD02 / PD03` to SCI8 and
   the five SSIE0 pins (placeholder mapping).
3. `ra_sci_init(8, 115200 8N1)` opens the J-Link diagnostic stream.
4. `ra_mstp_init()` then `ra_ssie_init(0, &cfg)` opens SSIE0 in
   master full-duplex I2S mode with `data_word = 16`,
   `system_word = 32`, AUDIO_MCK / 4 bit-clock, FIFO TX/RX
   thresholds at half-full (4 samples each).
5. `ra_ssie_start(0, k_ra_ssie_dir_tx_rx)` enables both directions.
6. Loop:
   - `ra_ssie_get_status` -- snapshot SSISR + SSIFSR.
   - If `rx_count > 0`, pop one sample, push it back to the TX
     FIFO, increment counter.
   - Every 1000 samples, print `"audio: <N> samples processed\r\n"`
     on SCI8 and toggle LED1.
   - If `status.error` is set, call `ra_ssie_start_recovery` to
     issue an SSIRST, clear flags, and resume.

## Connecting a terminal to SCI8

```sh
picocom -b 115200 /dev/cu.usbmodem...
```

You should see one line every ~21 ms when 48 kHz audio is flowing.

## Debugging

```sh
make -C examples/audio_loopback ozone
make -C examples/audio_loopback debug
```

Useful SWD probes (HUM Ch 46 register window):

```
mem32 0x4005D000 1   # SSIE0 SSICR
mem32 0x4005D004 1   # SSIE0 SSISR  -- check ROIRQ/RUIRQ/TOIRQ/TUIRQ
mem32 0x4005D008 1   # SSIE0 SSIFCR -- TIE/RIE/AUCKE/BSW
mem32 0x4005D00C 1   # SSIE0 SSIFSR -- RDC/TDC
mem32 0x4005D014 1   # SSIE0 SSIOFR -- OMOD/LRCONT
mem32 0x4005D018 1   # SSIE0 SSISCR -- TDES/RDFS thresholds
```
