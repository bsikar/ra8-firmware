# SOUP Justification: TF-PSA-Crypto

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting TF-PSA-Crypto into this
firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: TF-PSA-Crypto (TrustedFirmware PSA Crypto)
- **Version**: 1.1.0 (per `ChangeLog` head: "TF-PSA-Crypto 1.1.0 branch
  released 2026-03-31").
- **Upstream URL**: https://github.com/Mbed-TLS/TF-PSA-Crypto
- **Local path**: `libs/third_party/tf-psa-crypto/`

## Provenance

- **Origin**: TrustedFirmware.org Mbed TLS project; the PSA Crypto core
  was split out of Mbed TLS into its own repository starting with the
  Mbed TLS 4.x line.
- **License**: Dual Apache-2.0 OR GPL-2.0-or-later (`LICENSE`); we
  consume it under Apache-2.0.
- **How it entered our tree**: Vendored snapshot of the upstream
  TF-PSA-Crypto `development` branch shortly after the 1.1.0 release.
  Resolved (#548) to commit `bbf1eaf5f4a72bcc3e0cfe854e0313c93b75cd77`
  (2026-04-29): 217 of the 222 vendored files are byte-identical to it.
  The remaining five are upstream's build-generated sources, enumerated
  under "Deviations / patches" below.
  - **Precision of the pin.** Eleven commits past `v1.1.0` carry this exact
    set of 217 blobs (files outside the vendored subset changed between them,
    files inside it did not), so the subset cannot single one out.
    `bbf1eaf5` is the newest of them dated on or before the 2026-05-01
    vendor-in. The recorded claim -- every vendored file is byte-identical to
    upstream at `bbf1eaf5` -- is exact; what the evidence does not establish
    is that no other commit would satisfy it too.

## Use case in this firmware

- **Primary consumer: the Root-of-Trust secure-boot chain.** The
  `libs/ra8_dfu/src/ra8_rot.c` ECDSA-P256 signature verification used by
  `dfu_bootloader`, `secure_boot_hil`,
  `secure_boot_ns_hil` and `rot_verify_hil`. This is the silicon-validated
  path: a fresh board refuses an unsigned image because of it.
- PSA Crypto API implementation (key store, hashes, AEAD, asymmetric
  signature) also consumed via `libs/ra8_psa_crypto/` by `psa_crypto_hil`
  and `crypto_aes_demo`, by the host KAT suite, and -- when
  `RA8_USE_MBEDTLS=ON` -- underneath `libs/ra8_tls/`.
- **Not** consumed by `libs/ra8_ota/`, which links no crypto library at all
  (see `docs/SOUP/mbedtls.md`).
- Integrity claim category: control-flow (signature verification gates the
  secure-boot accept/reject decision).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: Inherits the Mbed TLS / PolarSSL crypto
  primitives' track record (in continuous production use since 2009).
- **Vendor qualification data**: PSA Crypto API conformance was held by
  the predecessor crypto core inside Mbed TLS; cited for context only.
- **Open-source community process**: TrustedFirmware.org governance and
  shared security disclosure pipeline with Mbed TLS.
- **Bug tracker review**: Advisories at
  https://github.com/Mbed-TLS/TF-PSA-Crypto/security/advisories
  reviewed; 1.1.0 includes all currently published fixes affecting our
  build options.

## Risk mitigation

- Signature verification is the gate on secure boot, and the RoT public key
  it verifies against is held on the secure side under `libs/ra8_secure_app/`.
- Two boundaries that this record previously claimed do NOT exist, and are
  recorded here so the gap is visible rather than assumed away:
  - PSA key handles are **not** brokered by the key vault or the NSC
    veneers. `libs/ra8_secure_app/src/key_vault.c` and `libs/ra8_nsc/` contain zero
    PSA references; keys are imported transiently at the call site.
  - `libs/ra8_psa_crypto/` is **not** a mandatory chokepoint. It is one
    shim among several callers; `psa_crypto_hil`, `threadx_https_client`
    and the PSA KAT shims include `psa/crypto.h` directly.
  Making either boundary real is open work, not a shipped mitigation.

## Deviations / patches

Five files, all upstream build-generated sources vendored because the cross
build does not run upstream's generators; they have no upstream counterpart at
any revision. Declared in `scripts/gen/sbom_registry.py` and pinned by content
in `docs/sbom/upstream/tf-psa-crypto.manifest`; the other 217 vendored files
are verified byte-identical to the upstream pin on every CI run (#548).

- `core/psa_crypto_driver_wrappers.h` and
  `core/psa_crypto_driver_wrappers_no_static.c` -- emitted from the driver
  description JSON by upstream's `generate_driver_wrappers.py`.
- `core/tf_psa_crypto_config_check_before.h`,
  `core/tf_psa_crypto_config_check_final.h` and
  `core/tf_psa_crypto_config_check_user.h` -- generated config-check headers.

No source file of TF-PSA-Crypto itself is modified.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
