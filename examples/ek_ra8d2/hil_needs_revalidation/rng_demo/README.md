# rng_demo

Pulls bytes from `ra8_psa_crypto_random()` once a second and dumps them as hex
over the console, toggling LED1 on each emit so the heartbeat is visible without
a serial terminal.

> **This is not entropy.** The RSIP-E50D TRNG has no working register interface
> on this silicon -- `ra8_rsip_trng_read` fails closed -- so
> `ra8_psa_crypto_random` here returns a deterministic software stub that
> produces the same stream every boot. The verdict line says so in as many
> words. A green result proves only that the API path runs and the stub is not
> stuck; it says nothing about the quality of the output. Real random bytes need
> an FSP-derived RSIP TRNG procedure.

No external transceiver, board or harness required.
