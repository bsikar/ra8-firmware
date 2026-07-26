# rng_demo

`ra8_psa_crypto_random()` dump for the bare EK-RA8D2 EVM.

Brings up SCI8 (115200 8N1, TXD8 = PD_02 / RXD8 = PD_03) and the
`ra8_psa_crypto` facade. Once a second pulls 32 bytes from
`ra8_psa_crypto_random()` and emits them as `rng: <64 hex chars>\r\n`
on the on-board J-Link OB CDC channel. LED1 toggles on every emit so
the heartbeat is also visible without a serial terminal.

> **Not secure entropy.** The RSIP-E50D TRNG has no working register interface
> on this silicon (`ra8_rsip_trng_read` fails closed), so `ra8_psa_crypto_random`
> here returns a **deterministic software stub** -- the same stream every boot.
> The verdict line is `rng: PRNG stub OK (deterministic, NOT entropy)`; a green
> means only that the API path runs and the stub is not stuck. Real random bytes
> need an FSP-derived RSIP TRNG procedure.

No external transceiver, board, or harness is required.

Build / flash:

```
make rng_demo
make hil-flash APP=rng_demo
```
