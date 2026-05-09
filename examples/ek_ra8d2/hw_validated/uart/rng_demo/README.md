# rng_demo

PSA TRNG dump for the bare EK-RA8D2 EVM.

Brings up SCI8 (115200 8N1, TXD8 = PD_02 / RXD8 = PD_03) and the
`ra_psa_crypto` facade. Once a second pulls 32 bytes from
`ra_psa_crypto_random()` and emits them as `trng: <64 hex chars>\r\n`
on the on-board J-Link OB CDC channel. LED1 toggles on every emit so
the heartbeat is also visible without a serial terminal.

No external transceiver, board, or harness is required.

Build / flash:

```
make rng_demo
make -C examples/ek_ra8d2/rng_demo flash
```
