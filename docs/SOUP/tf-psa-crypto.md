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

- PSA Crypto API implementation (key store, hashes, AEAD, asymmetric
  signature) consumed via `libs/ra8_psa_crypto/` and indirectly by
  `libs/ra8_tls/`, `libs/ra8_ota/`, and the secure-side substrate at
  `src/secure_app/`.
- Integrity claim category: control-flow (signature verification gates
  the OTA acceptance decision and TLS handshake completion).

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

- PSA Crypto key handles are owned by the secure-side key vault
  (`src/secure_app/`) and surfaced to the non-secure side only through
  the NSC veneers in `libs/ra8_nsc/`.
- All callers go through the `libs/ra8_psa_crypto/` shim, never the raw
  `psa/crypto.h` API directly.

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
