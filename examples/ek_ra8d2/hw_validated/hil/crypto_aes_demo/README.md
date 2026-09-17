# crypto_aes_demo

Round-trips a fixed plaintext through AES-128-GCM via the `ra8_psa_crypto`
facade once a second -- import key, AEAD-encrypt with a fixed nonce and AAD,
decrypt, compare byte-for-byte. LED1 toggles on a clean round-trip, LED2 on any
failure along the way, and the verdict is logged over the J-Link OB console.

The per-app CMake forces `RA8_OFF_TARGET`, so the facade links its in-tree soft
AEAD: no RSIP key handles and no third-party crypto in the image. That makes
this a test of the facade's contract rather than of any accelerator, and it is
deliberate -- the same source rebuilds against a hardware-backed provider with
no edits once one is selectable.
