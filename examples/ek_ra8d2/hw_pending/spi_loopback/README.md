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

## HIL plan

**HIL-able now -- proposed mode: `uart_scrape`.** Loopback is internal
(SPCR2.SPLP = 1) so no external wiring is required. The firmware
prints `spi: pass` every second on the J-Link OB CDC port and only
prints `spi: FAIL` on a byte-mismatch. A tight `uart_scrape` config
would look like:

```
HIL_MODE=uart_scrape
HIL_EXPECT="spi: pass"
HIL_EXPECT_NEGATIVE="spi: FAIL|HardFault"
HIL_TIMEOUT_S=10
```

This is the same idiom as `i2c_loopback` and `eth_loopback` -- a real
assertion that the peripheral moved bytes through internal loopback,
not just that the firmware did not fault. Alternative is the
`jlink_memprobe` pattern used by `can_classic_loopback` /
`canfd_loopback`, which requires instrumenting main.c with
`g_spi_match` / `g_spi_mismatch` globals first.

Relocated from `hw_validated/manual/` on 2026-05-19 -- author has not
yet bench-confirmed the loopback PASS path.
