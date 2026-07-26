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
| Apache NimBLE   | 1.9.0+dev git `8b6f3e81` | Apache Software Foundation  | [nimble.md](nimble.md)             |
| litehtml        | 0.9+dev git `8836bc1b` | Yuri Kobets / community     | [litehtml.md](litehtml.md)         |
| miniz           | 11.0.2   | Rich Geldreich / RAD        | [miniz.md](miniz.md)               |
| XZ Embedded (decode) | tag `v2024-12-30` git `ae63ae3a` | Lasse Collin / Tukaani | [xz_embedded.md](xz_embedded.md) |
| stb             | image v2.30 / truetype v1.26 | Sean Barrett | [stb.md](stb.md)              |
| libwebp (decode) | 1.5.0   | Google / WebM Project       | [libwebp.md](libwebp.md)           |
| TinyXML-2       | 11.0.0   | Lee Thomason / community    | [tinyxml2.md](tinyxml2.md)         |
| TFLite-micro    | git `fddd3707` | Google / TensorFlow   | [tflite-micro.md](tflite-micro.md) |
| FlatBuffers     | 25.9.23  | Google                      | [flatbuffers.md](flatbuffers.md)   |
| gemmlowp        | git `719139ce` | Google                | [gemmlowp.md](gemmlowp.md)         |
| ruy             | git `d3712831` | Google                | [ruy.md](ruy.md)                   |
| RSIP-E50D firmware (`r_sce_AMC`) | FSP TBD | Renesas / FSP            | [r_sce_AMC_firmware.md](r_sce_AMC_firmware.md) |

Host build tool (not vendored source, not linked into firmware): **Arm Ethos-U
Vela** -- [vela.md](vela.md) (pinned at `tools/vela/requirements.txt`).

Co-processor firmware (not vendored source, not linked into firmware; built
from a pinned upstream and flashed onto the companion ESP32-C6):
**Espressif esp-hosted-mcu** -- [esp-hosted.md](esp-hosted.md) (pinned in
`coprocessor/esp32c6/pins.env`; recipe in `coprocessor/esp32c6/`).

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
  `libs/ra8_tls/` and `libs/ra8_ota/`.
- **TF-PSA-Crypto** -- PSA Crypto API implementation backing TLS, OTA
  signature checks, and the secure-side key vault.
- **Apache NimBLE** -- Bluetooth 5.4 host + controller stack staged for
  future BLE bring-up; not yet linked to an example.
- **litehtml** -- HTML/CSS layout engine for the EPUB reader.
- **miniz** -- Deflate / inflate / ZIP support for EPUB unpacking.
- **XZ Embedded** (decode-only) -- XZ/LZMA2 decoding for wrapped archive
  content (`.tar.xz`) behind the bounded `libs/ra8_unarch` wrapper.
- **stb** -- PNG / JPEG decoding (`stb_image`) and TTF rasterization
  (`stb_truetype`) for the EPUB reader.
- **libwebp** (decode-only) -- WebP (VP8 / VP8L) decoding for longstrip / manga
  raster content, reached through the `libs/ra8_webp/` facade. Not yet wired
  into the raster decode dispatch (that is #289).
- **TinyXML-2** -- XML parser for EPUB container metadata.
- **TFLite-micro** -- On-device neural-network inference runtime
  (MicroInterpreter + a lean reference-kernel set) for the RA8P1 Ethos-U55 NPU.
- **FlatBuffers** -- Zero-copy serialization headers for the `.tflite` model
  format TFLite-micro reads.
- **gemmlowp** -- Fixed-point math headers TFLite-micro's quantized reference
  kernels depend on.
- **ruy** -- A single profiler-instrumentation stub header included by
  TFLite-micro kernel utilities (no ruy GEMM backend).
- **Vela** (host tool) -- Arm's offline Ethos-U model compiler; runs at build
  time, links nothing into firmware. See [vela.md](vela.md).
- **Espressif esp-hosted-mcu** -- ESP32-C6 wireless co-processor firmware
  (Wi-Fi/BLE) built from a pinned upstream commit and flashed onto the C6.
  Not vendored and not linked into the RA8 image; recipe in `coprocessor/esp32c6/`.
- **RSIP-E50D firmware (`r_sce_AMC`)** -- Renesas Secure IP protected
  procedures (key install / wrap / unwrap) consumed by
  `libs/ra8_hal/src/ra8_rsip*.c` and the secure-side key vault.
  Vendored from `renesas/fsp` -- see
  `libs/third_party/fsp_blobs/README.md`.

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
[`../../scripts/gen/gen_sbom.py`](../../scripts/gen/gen_sbom.py)
(`make sbom` / `make sbom-check`); its component registry is the single
source of truth for the version / license / purl / provenance fields. When
you bump or re-vendor a component here, update that registry and run
`make sbom` so the SBOM and inventory do not drift (enforced in CI and the
pre-commit hook).

Commit-pinned components are additionally scanned for published CVEs every
week: `.github/workflows/osv-scan.yml` downloads a pinned `osv-scanner`
release and runs [`../../scripts/checks/osv_scan.sh`](../../scripts/checks/osv_scan.sh),
which queries OSV.dev both with the SBOM purls and with each recorded
upstream commit (the form OSV actually resolves for git-vendored C/C++).

## Review cadence

Each document is re-reviewed at most 12 months after its "Last review
date". Re-review must update version numbers, re-check the upstream
issue / advisory tracker, and bump both dates.
