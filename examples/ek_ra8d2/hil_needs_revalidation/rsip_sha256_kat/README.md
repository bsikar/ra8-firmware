# rsip_sha256_kat

On-silicon FIPS 180-4 known-answer test for `ra8_rsip_sha256`. `ra8_rot`
re-computes an image digest through this function, so its correctness is
security-critical; the app hashes three published NIST vectors on the real M85
-- the empty string, `"abc"`, and a two-block message -- and checks the digests.

## The RSIP-E50D is not register-mapped

This was the first app ever to drive the RSIP hardware on a board, and what it
found is the durable fact worth keeping. HUM Ch 52 "Renesas Secure IP
(RSIP-E50D)" is a six-page feature overview (p 3302-3307) with **no register
interface at all**; Renesas drives the engine through FSP's opaque procedural
"primitive" command sequences. The register map that had been written into
`ra8_rsip.c` was invented, and on silicon it behaved accordingly: the BIST
self-test never passed, the hash returned `k_ra8_err_hw_timeout` with an
all-zero digest because the `HASH_STATUS` poll never saw its bit assert, and a
J-Link read of the RSIP window at `0x403B0000` found every register reading zero
with none of the driver's writes sticking.

Nothing on the host could have caught it: the host tests all run
`RA8_OFF_TARGET`, which takes the software fallback.

So `ra8_rsip_sha256` computes the digest with the in-tree software SHA-256
(`RA8_RSIP_SOFTWARE_BACKEND`, enabled unconditionally). The invented hardware
sequence is retained but never compiled, behind `RA8_RSIP_HASH_HARDWARE`, as a
starting point for a future FSP-derived port. `ra8_rot`'s on-silicon image
digest works because of the software path, not despite it.
