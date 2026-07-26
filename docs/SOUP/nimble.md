# SOUP Justification: Apache NimBLE

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting Apache NimBLE into this
firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: Apache NimBLE
- **Version**: 1.9.0+dev, pinned at upstream commit
  `8b6f3e819118a1839e5f238bfe1797d64878dc3d` (default branch, 2026-04-28;
  42 commits past the `nimble_1_9_0_tag` release tag). `RELEASE_NOTES.md`
  prose still reads "15 December 2025 - Apache NimBLE v1.9.0"; the
  `version.yml` file is "0.0.0" because that file is reserved for the
  upstream default-branch placeholder.
- **Upstream URL**: https://github.com/apache/mynewt-nimble
- **Local path**: `libs/third_party/nimble/`

## Provenance

- **Origin**: Apache Software Foundation, Apache Mynewt project.
- **License**: Apache-2.0 (`LICENSE` and `NOTICE`).
- **How it entered our tree**: Vendored snapshot of the upstream default
  branch shortly after the 1.9.0 release. The commit was recovered by
  fingerprinting: all 859 vendored files (858 regular files plus the one
  `porting/npl/riot` symlink) are byte-identical to upstream commit
  `8b6f3e81`, the single exact match among the 5567 commits reachable
  from the upstream default branch. The vendored subset drops the
  upstream `apps/` directory (163 files) and nothing else.

## Use case in this firmware

- Bluetooth 5.4 host stack, consumed DIRECTLY by applications via
  `port/nimble/` (the ThreadX + HCI-over-`ra8_ble` port). The ESP32-C6
  companion runs the BLE controller; NimBLE runs the host. The
  first-party BLE-host facade that previously wrapped NimBLE
  has been removed -- applications call the NimBLE host APIs
  (`host/ble_gap.h`, `host/ble_hs.h`, ...) directly.
- `examples/_unsupported/threadx_nimble_peripheral` demonstrates the
  direct-NimBLE path; it is HW-blocked on the ESP32-C6 radio companion,
  so no in-tree example is HW-validated yet.
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

- All BLE access is mediated through the NimBLE host via the
  `port/nimble/` port and the `ra8_ble` HCI transport seam, keeping a
  single integration boundary.
- Stack is not yet wired to any production-track example; introduction
  will require a fresh integration test pass.

## Deviations / patches

None. The vendored tree is unmodified (byte-identical to the pinned
upstream commit; `apps/` omitted).

## CVE monitoring

The pinned commit is queried against OSV.dev weekly by
`.github/workflows/osv-scan.yml` (commit-range GIT queries via
`scripts/checks/osv_scan.sh`); a published advisory affecting the pin
fails the scheduled run.

## Last review date

- Reviewed: 2026-07-15 (commit pin recovered and recorded)
- Expected re-review by: 2027-05-02
