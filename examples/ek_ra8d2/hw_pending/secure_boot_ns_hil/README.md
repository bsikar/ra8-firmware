# secure_boot_ns_hil

Proves that the Secure world **authenticates the Non-Secure image before BLXNS**
and **default-denies** a tampered one. This is the BLXNS half of #172;
copy-to-run is already silicon-proven by `secure_boot_hil`.

The Secure side no longer hardcodes the NS trailer address:

1. The NS linker emits a `.ns_rot_header` at a fixed offset of `ns_base + 0x40`
   -- right after the 16-slot vector table -- holding a magic and the signed body
   length.
2. `ra8_tz_ns_signed_body_len()` reads that header, and
   `ra8_rot_trailer_after()` locates the `ra8_rot_trailer_t` the signing tool
   appended at `ns_base + body_len`. That is the exact `[ body ][ trailer ]`
   layout copy-to-run uses.
3. `ra8_rot_verify_image()` re-computes SHA-256 and verifies the ECDSA-P256
   signature. BLXNS happens **only** on success.

The build produces two artifacts from one command: the genuine merged image, and
one with a single body byte flipped. Signing needs the held-out RoT private key;
without it the build still succeeds and prints the exact sign command to run.

## Blocked on

A bench run flashing **both** artifacts and confirming the two outcomes by
memprobe. Genuine: `g_sbns_ns_alive` advances and `g_sbns_denied` stays 0.
Tampered: `g_sbns_ns_alive` never advances because NS never ran, `g_sbns_denied`
reads 1, and `g_sbns_jump_err` reads `k_ra8_err_checksum_mismatch`.
