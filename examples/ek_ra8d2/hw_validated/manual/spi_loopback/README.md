# spi_loopback

SPI_B internal-loopback smoke test for the EK-RA8D2. Brings up
`ra_spi` on channel 0 at 1 MHz mode-0 MSB-first, then stamps
`SPCR2.SPLP = 1` (HUM Ch 43.2.5 p 2889 -- `k_ra_spcr2_mask_splp`)
so COPI is fed back into CIPO inside the chip. No external CIPO ?
COPI wiring required.

Each cycle walks a 16-byte test pattern (`0xA0..0xAF`) through
`ra_spi_xfer8` and verifies RX matches TX byte-for-byte. SCI8 prints

```
spi: pass
spi: pass
...
```

once a second on the J-Link OB CDC port (115200 8N1). LED1 toggles
on each successful round-trip; LED2 latches ON if any byte
mismatches.

## Build + flash

```sh
make spi_loopback
make -C examples/ek_ra8d2/spi_loopback flash
```
