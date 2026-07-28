# crypto_aes_demo

AES-128-GCM encrypt + decrypt round-trip on the bare EK-RA8D2 EVM.

Imports a fixed AES-128 key into the `ra8_psa_crypto` facade once per
second, AEAD-encrypts the 8-byte plaintext "RA8D2_OK" with a fixed
nonce + 4-byte AAD, decrypts the resulting ciphertext + tag, and
verifies the recovered plaintext matches the original byte-for-byte.

- LED1 toggles on every successful round-trip.
- LED2 toggles on any failure (key import, AEAD, or compare mismatch).
- The result is logged over SCI8 (115200 8N1, J-Link OB CDC port) as
  `aes: round-trip OK` or `aes: round-trip FAIL`.

The per-app CMake forces `RA8_OFF_TARGET` so `ra8_psa_crypto` uses
its in-tree soft-fallback AEAD implementation -- no RSIP keys, no
Mbed TLS in the link. Once `RA8_USE_MBEDTLS=ON` ships, the same
source rebuilds against real AES-GCM with no edits.

Build / flash:

```
make crypto_aes_demo
make -C examples/ek_ra8d2/crypto_aes_demo flash
```
