# psa_crypto_hil

On-silicon known-answer test for the real software crypto backend
(TF-PSA-Crypto) on the M85 -- the hardware counterpart of the host
`test_psa_real_kat.c`.

`ra8_rot` needs SHA-256 and ECDSA-P256 verify to authenticate images, and the
RSIP crypto hardware is non-functional on this silicon (see `ra8_rsip.c`), so
the software backend is not a fallback here -- it is the thing the root of trust
actually runs. This app proves it works on the real part against published
vectors:

| primitive | vector |
|---|---|
| SHA-256("abc") | FIPS 180-4 |
| AES-128-GCM encrypt + decrypt | GCM spec (McGrew/Viega) Test Case 3 |
| ECDSA-P256/SHA-256 verify | RFC 6979 A.2.5 ("sample"), plus a tamper case |

The per-app `CMakeLists.txt` compiles the vendored TF-PSA-Crypto sources into a
software-only static library with no hardware-accelerator dispatch, configured
for an external RNG.

**The RNG shim is test-only.** `src/psa_kat_shim.c` supplies a deterministic RNG
and an `mbedtls_ms_time` purely so `psa_crypto_init` succeeds; the KAT
operations draw no randomness, so this is sound here and nowhere else. A
production build needs a real entropy source.
