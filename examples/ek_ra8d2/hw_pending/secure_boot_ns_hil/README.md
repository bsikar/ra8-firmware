# secure_boot_ns_hil -- TrustZone BLXNS root-of-trust proof (#172)

Proves, on the EK-RA8D2, that the Secure world **authenticates the Non-Secure
image before BLXNS** and **default-denies** a tampered one. This is the BLXNS
half of issue #172 (copy-to-run is already silicon-proven by `secure_boot_hil`).

## What it exercises

The Secure side (`RA8_ENABLE_ROOT_OF_TRUST`) no longer hardcodes the NS trailer
address. Instead:

1. The NS linker (`ns_image.ld`) emits a `.ns_rot_header` at a fixed offset
   `ns_base + 0x40` (right after the 16-slot vector table) holding
   `{ magic="NSR1", body_len }`, where `body_len = end_of_signed_body -
   ns_run_start`.
2. `ra8_tz_ns_signed_body_len()` reads that header; `ra8_rot_trailer_after(ns_base,
   body_len)` locates the `ra8_rot_trailer_t` the signing tool appended at
   `ns_base + body_len` -- the exact `[ body ][ trailer ]` layout copy-to-run
   uses.
3. `ra8_rot_verify_image()` re-computes SHA-256 + verifies the ECDSA-P256
   signature. BLXNS happens only on success.

A genuine signed NS image runs (`g_sbns_ns_alive` climbs); a one-byte-tampered
image is rejected (`g_sbns_denied == 1`, NS never runs).

## Build + sign

`make` builds the Secure ELF + the NS ELF and, via `sign_and_merge.py`, signs
the NS image and merges it with the Secure hex into two artifacts:

- `build/secure_boot_ns_hil.hex` -- genuine (NS runs)
- `build/secure_boot_ns_hil_tampered.hex` -- one body byte flipped (NS denied)

Signing needs the held-out RoT private key. If it is not present the build still
succeeds and prints the exact sign command. To sign explicitly:

```
RA8_ROT_KEY=$HOME/ra8d2-rot-signing-key.pem make
# or run sign_and_merge.py by hand (the build prints the full invocation)
```

## Bench (memprobe)

Genuine:

```
flash build/secure_boot_ns_hil.hex
jlink_memprobe g_sbns_ns_alive   # expect: advances (>= 50 over ~3 s)
jlink_memprobe g_sbns_denied     # expect: 0
```

Tampered:

```
flash build/secure_boot_ns_hil_tampered.hex
jlink_memprobe g_sbns_ns_alive   # expect: 0 (no advance -- NS never ran)
jlink_memprobe g_sbns_denied     # expect: 1 (default-deny fired)
jlink_memprobe g_sbns_jump_err   # expect: k_ra8_err_checksum_mismatch
```

`hw_pending` until the bench flashes both artifacts and confirms the two
outcomes.
