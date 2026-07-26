# psa_crypto_hil

On-silicon NIST/RFC known-answer test for the **real** software crypto backend
(TF-PSA-Crypto) on the EK-RA8D2 -- the hardware counterpart of
`tests/test_psa_real_kat.c`.

`ra8_rot` needs SHA-256 + ECDSA-P256 verify to authenticate images, and the RSIP
crypto *hardware* is non-functional on this silicon (see `libs/ra8_hal/src/ra8_rsip.c`).
This app proves the software backend the root of trust actually uses works on the
M85, by hashing/verifying published vectors on real hardware:

| primitive                    | vector                              |
|------------------------------|-------------------------------------|
| SHA-256("abc")               | FIPS 180-4                          |
| AES-128-GCM encrypt+decrypt  | GCM spec (McGrew/Viega) Test Case 3 |
| ECDSA-P256/SHA-256 verify    | RFC 6979 A.2.5 ("sample") + tamper  |

`ra8_add_app` builds the app; the per-app `CMakeLists.txt` compiles the vendored
TF-PSA-Crypto sources into a `tfpsa_arm` static library (software-only; the port
config sets `MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG` with no hardware-accelerator
dispatch) and links it. `src/psa_kat_shim.c` supplies a **test-only** deterministic
RNG + `mbedtls_ms_time` so `psa_crypto_init` succeeds -- the KAT ops draw no
randomness. A production build needs a real entropy source.

- **LED1** toggles on every all-vectors-pass cycle; **LED2** latches on failure.
- `psa crypto: KAT OK` / `psa crypto: KAT FAIL` over SCI8 (115200 8N1); `hil.conf`
  scrapes it.

```
make psa_crypto_hil
make hil-flash APP=psa_crypto_hil
```
