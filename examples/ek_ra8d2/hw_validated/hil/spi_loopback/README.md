# spi_loopback

SPI_B internal-loopback test. Brings up `ra8_spi` on channel 0 at 1 MHz, mode 0,
MSB first, then sets SPCR2.SPLP (HUM Ch 43.2.5 p 2889,
`k_ra8_spcr2_mask_splp`) so COPI is fed back into CIPO inside the chip -- no
external wiring, no second device.

Each cycle walks a 16-byte pattern through `ra8_spi_xfer8` and compares RX
against TX byte for byte. LED1 toggles on a clean round-trip, LED2 latches on a
mismatch. The assertion is that the peripheral moved bytes, not merely that the
firmware did not fault -- the same idiom `i2c_loopback` uses.
