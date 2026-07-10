# SOUP Justification: Apache NimBLE

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting Apache NimBLE into this
firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: Apache NimBLE
- **Version**: 1.9.0 (per `RELEASE_NOTES.md`: "15 December 2025 - Apache
  NimBLE v1.9.0"). The `version.yml` file is "0.0.0" because that file
  is reserved for the upstream default-branch placeholder.
- **Upstream URL**: https://github.com/apache/mynewt-nimble
- **Local path**: `libs/third_party/nimble/`

## Provenance

- **Origin**: Apache Software Foundation, Apache Mynewt project.
- **License**: Apache-2.0 (`LICENSE` and `NOTICE`).
- **How it entered our tree**: Vendored snapshot of the upstream Apache
  NimBLE 1.9.0 release. Upstream commit hash unknown.

## Use case in this firmware

- Bluetooth 5.4 host + controller stack consumed by `libs/ra_ble_host/`.
- No example app currently links NimBLE; reserved for future BLE
  bring-up.
- Integrity claim category: none (no BLE-driven safety signal in the
  current firmware).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: Apache NimBLE has shipped in production BLE
  devices since 2017 (Apache Mynewt 1.0).
- **Open-source community process**: Apache Software Foundation
  governance, including formal voting on releases and a security
  reporting channel at security@apache.org.
- **Bug tracker review**: Issues at
  https://github.com/apache/mynewt-nimble/issues reviewed; no open
  advisories affect the host-stack subset we plan to use.

## Risk mitigation

- All BLE access will be mediated through `libs/ra_ble_host/` to keep a
  single integration boundary.
- Stack is not yet wired to any production-track example; introduction
  will require a fresh integration test pass.

## Deviations / patches

None. The vendored tree is unmodified.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
