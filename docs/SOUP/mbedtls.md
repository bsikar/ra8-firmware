# SOUP Justification: Mbed TLS

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting Mbed TLS into this firmware
as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: Mbed TLS
- **Version**: 4.1.0 (per `include/mbedtls/build_info.h`
  MBEDTLS_VERSION_STRING_FULL = "Mbed TLS 4.1.0"; ChangeLog confirms the
  4.1.0 branch released 2026-03-31).
- **Upstream URL**: https://github.com/Mbed-TLS/mbedtls
- **Local path**: `libs/third_party/mbedtls/`

## Provenance

- **Origin**: TrustedFirmware.org / Linaro project (originally PolarSSL,
  acquired by ARM as Mbed TLS, then transferred to TrustedFirmware).
- **License**: Dual Apache-2.0 OR GPL-2.0-or-later (`LICENSE`); we
  consume it under Apache-2.0.
- **How it entered our tree**: Vendored snapshot of the upstream
  Mbed TLS 4.1.0 release. Upstream commit hash unknown.

## Use case in this firmware

- TLS record layer and X.509 handling consumed via `libs/ra_tls/` and
  the OTA verification path in `libs/ra_ota/`.
- Crypto primitives live in the sibling `tf-psa-crypto` package
  (separated upstream as of 4.x); see `docs/SOUP/tf-psa-crypto.md`.
- Integrity claim category: data-handling (TLS framing, certificate
  parsing) and control-flow (signature-verification gate on OTA
  acceptance).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: Mbed TLS / PolarSSL has been in continuous
  production use across embedded TLS deployments since 2009.
- **Open-source community process**: TrustedFirmware.org governance,
  documented Long-Term-Support branches, and a published security
  policy in `SECURITY.md`.
- **Vendor qualification data**: Mbed TLS has historically held PSA
  Certified Crypto API conformance evidence. Cited for context only;
  this project does not claim PSA Certified status.
- **Bug tracker review**: Advisories at
  https://github.com/Mbed-TLS/mbedtls/security/advisories reviewed;
  4.1.0 includes all currently published fixes affecting our build
  options.

## Risk mitigation

- All TLS calls are wrapped in `libs/ra_tls/` so policy (cipher suites,
  certificate pinning) is centrally enforced.
- OTA signature checks are verified against keys held in the
  ring-5 secure-side key vault under `src/secure_app/`.

## Deviations / patches

None. The vendored tree is unmodified.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
