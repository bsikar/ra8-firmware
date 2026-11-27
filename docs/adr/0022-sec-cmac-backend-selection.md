# ADR-0022: Retain in-tree constant-time AES-CMAC backend for sec_cmac seam

## Status

Accepted -- 2026-11-26. Records the sec_cmac cryptographic backend decision
under RA8FW-283.

## Context

`libs/ra8_secure_app` implements the TrustZone Secure partition runtime and the
secure-side key importer (`key_import.c`). To authenticate wrapped-key blobs
before admitting them into the secure key vault, the key importer computes and
verifies an AES-CMAC (NIST SP 800-38B) tag over the blob using the Key
Authentication Key (KAK) loaded from the secure vault.

The seam is declared in `libs/ra8_secure_app/src/sec_cmac_internal.h` via
`priv_ra8_sec_cmac_compute()` and `priv_ra8_sec_cmac_verify()`.

Historically, the header docstring contemplated two possible backends:
1. An opt-in `RA8_KEY_IMPORT_PSA_CMAC` definition routing calls to the
   vendored TF-PSA-Crypto stack (`psa_mac_compute` / `psa_mac_verify` with
   `PSA_ALG_CMAC`).
2. A freestanding, self-contained in-tree AES-128 / AES-256 CMAC implementation
   authored in Zig (`libs/ra8_secure_app/src/internal/cmac.zig`).

In practice, `RA8_KEY_IMPORT_PSA_CMAC` was never defined in any firmware target
or test harness. As part of consolidating the TF-PSA-Crypto build infrastructure
across the repository (RA8FW-283), the architectural choice between routing
`sec_cmac` through TF-PSA-Crypto or permanently retaining the dedicated in-tree
backend must be resolved.

## Decision

1. **Retain the in-tree constant-time AES-CMAC backend:**
   The self-contained in-tree AES-CMAC implementation in `libs/ra8_secure_app`
   is retained as the sole, canonical backend for `sec_cmac`.

2. **Reject TF-PSA-Crypto routing for sec_cmac:**
   The hypothetical `RA8_KEY_IMPORT_PSA_CMAC` path is formally rejected and
   omitted.

3. **Rationale:**
   - **Footprint and isolation:** `libs/ra8_secure_app` executes within the
     constrained TrustZone Secure domain. Pulling the TF-PSA-Crypto core,
     driver wrappers, and dispatch tables into `ra8_secure_app` solely to
     compute a 16-byte CMAC would substantially increase code size and require
     a static/dynamic memory buffer pool (`mbedtls_calloc`) inside the secure
     partition.
   - **Freestanding safety compliance:** The in-tree implementation in
     `libs/ra8_secure_app/src/internal/cmac.zig` is constant-time, zero-allocation,
     and strictly bounded to messages up to `k_ra8_sec_cmac_max_msg_bytes`
     (256 bytes), adhering to NASA Power of 10 Rule 2.
   - **Verification rigour:** The in-tree backend is verified on the host via
     `tests/security/src/test_secure_app_sec_cmac.c` against all official
     NIST SP 800-38B known-answer test vectors (AES-128 and AES-256 across
     0, 16, 40, and 64 byte message lengths), along with 100% MC/DC coverage
     on compound verification decisions.
   - **Single implementation surface:** Maintaining exactly one verified
     backend prevents semantic drift between target firmware and off-target
     test configurations.

## Consequences

- `libs/ra8_secure_app` remains lightweight, self-contained, and completely
  independent of TF-PSA-Crypto.
- Applications requiring general-purpose asymmetric or symmetric cryptography
  (e.g., secure boot verification, TLS) continue using the consolidated
  `cmake/tf_psa_crypto.cmake` recipe without coupling to `ra8_secure_app`.
- The `libs/ra8_secure_app/src/sec_cmac_internal.h` documentation is updated
  to reflect this single-backend architecture.

## References

- NIST SP 800-38B: Recommendation for Block Cipher Modes of Operation: The CMAC Mode for Authentication
- FIPS 197: Advanced Encryption Standard (AES)
- RA8FW-283: Shared tf-psa-crypto recipe and crypto consolidation
- `libs/ra8_secure_app/src/sec_cmac_internal.h`
- `libs/ra8_secure_app/src/internal/cmac.zig`
- `tests/security/src/test_secure_app_sec_cmac.c`
