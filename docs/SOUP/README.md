# SOUP (Software Of Unknown Provenance) Catalog

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, every
third-party library shipped in this firmware must have a written
qualification basis. This directory holds one Markdown justification per
direct subdirectory of `libs/third_party/`.

The exemption that admits these libraries to the build (no source-level
MC/DC re-test required in this repository) is recorded in the
"Exempt Code" subsection of `CLAUDE.md`. The per-component documents
below are the evidence that justifies that exemption on a case-by-case
basis.

## Index

| Library         | Version  | Origin                      | Doc                                |
| --------------- | -------- | --------------------------- | ---------------------------------- |
| ThreadX         | 6.5.0    | Eclipse Foundation          | [threadx.md](threadx.md)           |
| NetX Duo        | 6.5.0    | Eclipse Foundation          | [netxduo.md](netxduo.md)           |
| FileX           | 6.5.0    | Eclipse Foundation          | [filex.md](filex.md)               |
| USBX            | 6.5.0    | Eclipse Foundation          | [usbx.md](usbx.md)                 |
| LevelX          | 6.5.0    | Eclipse Foundation          | [levelx.md](levelx.md)             |
| Mbed TLS        | 4.1.0    | TrustedFirmware.org         | [mbedtls.md](mbedtls.md)           |
| TF-PSA-Crypto   | 1.1.0    | TrustedFirmware.org         | [tf-psa-crypto.md](tf-psa-crypto.md) |
| Apache NimBLE   | 1.9.0    | Apache Software Foundation  | [nimble.md](nimble.md)             |
| litehtml        | unknown  | Yuri Kobets / community     | [litehtml.md](litehtml.md)         |
| miniz           | 11.0.2   | Rich Geldreich / RAD        | [miniz.md](miniz.md)               |
| stb             | image v2.30 / truetype v1.26 | Sean Barrett | [stb.md](stb.md)              |
| TinyXML-2       | 11.0.0   | Lee Thomason / community    | [tinyxml2.md](tinyxml2.md)         |
| RSIP-E50D firmware (`r_sce_AMC`) | FSP TBD | Renesas / FSP            | [r_sce_AMC_firmware.md](r_sce_AMC_firmware.md) |
| BLE controller patch image       | FSP TBD | Renesas / FSP            | [ble_patch_image.md](ble_patch_image.md)       |

## One-line summaries

- **ThreadX** -- Cooperative + preemptive RTOS kernel under every
  `threadx_*` example.
- **NetX Duo** -- Dual IPv4/IPv6 TCP/IP + TLS stack used by the NetX
  echo demo and OTA download path.
- **FileX** -- FAT / exFAT file system used by the FileX demos and OTA
  staging.
- **USBX** -- USB host / device stack for the CDC, HID, and MSC demos.
- **LevelX** -- NOR-flash wear-leveling layer under FileX on Octo-SPI.
- **Mbed TLS** -- TLS record layer and X.509 handling consumed via
  `libs/ra_tls/` and `libs/ra_ota/`.
- **TF-PSA-Crypto** -- PSA Crypto API implementation backing TLS, OTA
  signature checks, and the secure-side key vault.
- **Apache NimBLE** -- Bluetooth 5.4 host + controller stack staged for
  future BLE bring-up; not yet linked to an example.
- **litehtml** -- HTML/CSS layout engine for the EPUB reader.
- **miniz** -- Deflate / inflate / ZIP support for EPUB unpacking.
- **stb** -- PNG / JPEG decoding (`stb_image`) and TTF rasterization
  (`stb_truetype`) for the EPUB reader.
- **TinyXML-2** -- XML parser for EPUB container metadata.
- **RSIP-E50D firmware (`r_sce_AMC`)** -- Renesas Secure IP protected
  procedures (key install / wrap / unwrap) consumed by
  `libs/ra_hal/src/ra_rsip*.c` and the secure-side key vault.
  Vendored from `renesas/fsp` -- see
  `libs/third_party/fsp_blobs/README.md`.
- **BLE controller patch image** -- encrypted firmware payload
  uploaded to the on-chip BLE radio at boot by
  `libs/ra_hal/src/ra_ble_patch.c`. Required for any non-stub BLE
  example. Vendored from `renesas/fsp`.

## Aggregated license inventory and SBOM

This catalog is the per-component *qualification* record. Two aggregated
artifacts are derived from the same `libs/third_party/` tree and must be kept
in sync with it:

- [`../../THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md) -- the
  repo-root aggregated license inventory (attribution, the Apache-2.0
  election for the dual-licensed crypto, and open compliance items).
- [`../sbom/ra8-firmware.cdx.json`](../sbom/ra8-firmware.cdx.json) -- the
  machine-readable CycloneDX 1.5 SBOM (feed to `osv-scanner`).

Both are generated and validated by
[`../../scripts/utils/gen_sbom.py`](../../scripts/utils/gen_sbom.py)
(`make sbom` / `make sbom-check`); its component registry is the single
source of truth for the version / license / purl / provenance fields. When
you bump or re-vendor a component here, update that registry and run
`make sbom` so the SBOM and inventory do not drift (enforced in CI and the
pre-commit hook).

## Review cadence

Each document is re-reviewed at most 12 months after its "Last review
date". Re-review must update version numbers, re-check the upstream
issue / advisory tracker, and bump both dates.
