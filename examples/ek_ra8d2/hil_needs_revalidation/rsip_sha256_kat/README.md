# rsip_sha256_kat

On-silicon qualification and FIPS 180-4 known-answer test for `ra8_rsip_sha256`.
`ra8_rot` re-computes an image digest through this function, so its correctness is
security-critical; the app hashes three published NIST vectors on the real M85
-- the empty string, `"abc"`, and a two-block message -- and validates the digests.

## Backend Observability and Qualification Status

Under RA8FW-1208, cryptographic backend selection is made observable in test output:
no software fallback may be reported as a hardware KAT pass.

1. **Observable Backend Query:**
   The test queries `ra8_rsip_get_backend(k_ra8_rsip_op_sha256)`. When running the
   in-tree software implementation, it emits:
   ```
   rsip sha256: backend=software KAT OK
   ```
   It will never emit a bare hardware KAT pass when executing software fallbacks.

2. **TRNG Fail-Closed Verification:**
   The RSIP-E50D TRNG has no documented public register interface; `ra8_rsip_trng_read`
   fails closed with `k_ra8_err_not_supported`. The test verifies that this error code
   is returned and that the output buffer remains unmutated, rather than accepting
   canned or predictable entropy.

3. **Hardware Qualification Status:**
   Hardware qualification remains incomplete pending unencumbered driver primitives.
   Renesas drives RSIP-E50D via closed-source FSP binary libraries (`librsip.a`),
   which cannot be linked into open-source MIT firmware. Direct MMIO attempts to
   invented register offsets at `0x403B0000` fail on silicon (reads return zero,
   writes do not stick, and BIST never asserts).

The root-of-trust image digest in `ra8_rot` works reliably on silicon because it
deliberately uses the verified, constant-time software backend.
