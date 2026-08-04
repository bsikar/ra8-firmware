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
| ThreadX         | 6.5.0 tag `v6.5.0.202601_rel` | Eclipse Foundation  | [threadx.md](threadx.md)           |
| NetX Duo        | 6.5.0 tag `v6.5.0.202601_rel` | Eclipse Foundation  | [netxduo.md](netxduo.md)           |
| FileX           | 6.5.0 tag `v6.5.0.202601_rel` | Eclipse Foundation  | [filex.md](filex.md)               |
| USBX            | 6.5.0 tag `v6.5.0.202601_rel` | Eclipse Foundation  | [usbx.md](usbx.md)                 |
| LevelX          | 6.5.0 tag `v6.5.0.202601_rel` | Eclipse Foundation  | [levelx.md](levelx.md)             |
| Mbed TLS        | 4.1.0+dev git `d12fbb99` | TrustedFirmware.org     | [mbedtls.md](mbedtls.md)           |
| TF-PSA-Crypto   | 1.1.0+dev git `bbf1eaf5` | TrustedFirmware.org     | [tf-psa-crypto.md](tf-psa-crypto.md) |
| Apache NimBLE   | 1.10.0 tag `nimble_1_10_0_tag` git `a7a156f2` | Apache Software Foundation  | [nimble.md](nimble.md)             |
| litehtml        | 0.9+dev git `8836bc1b` | Yuri Kobets / community     | [litehtml.md](litehtml.md)         |
| miniz           | 11.0.2 release zip `miniz-3.0.2.zip` | Rich Geldreich / RAD | [miniz.md](miniz.md)   |
| XZ Embedded (decode) | tag `v2024-12-30` git `ae63ae3a` | Lasse Collin / Tukaani | [xz_embedded.md](xz_embedded.md) |
| stb             | image v2.30 / truetype v1.26, base git `31c1ad37` | Sean Barrett | [stb.md](stb.md)     |
| libwebp (decode) | 1.5.0   | Google / WebM Project       | [libwebp.md](libwebp.md)           |
| TinyXML-2       | 11.0.0 tag `11.0.0` | Lee Thomason / community  | [tinyxml2.md](tinyxml2.md)         |
| TFLite-micro    | git `fddd3707` | Google / TensorFlow   | [tflite-micro.md](tflite-micro.md) |
| FlatBuffers     | 25.9.23 git `18724097` | Google                  | [flatbuffers.md](flatbuffers.md)   |
| gemmlowp        | git `719139ce` | Google                | [gemmlowp.md](gemmlowp.md)         |
| ruy             | git `d3712831` | Google                | [ruy.md](ruy.md)                   |
| esp-hosted host driver | 2.12.11 git `949bb30` | Espressif Systems | [esp-hosted-host.md](esp-hosted-host.md) |
| protobuf-c (nested in esp-hosted) | 1.4.1 git `abc67a11` | protobuf-c authors | [esp-hosted-host.md](esp-hosted-host.md) |

Host build tool (not vendored source, not linked into firmware): **Arm Ethos-U
Vela** -- [vela.md](vela.md) (pinned at `tools/vela/requirements.txt`).

Co-processor firmware (not vendored source, not linked into firmware; built
from a pinned upstream and flashed onto the companion ESP32-C6):
**Espressif esp-hosted-mcu** -- [esp-hosted.md](esp-hosted.md) (pinned in
`coprocessor/esp32c6/pins.env`; recipe in `coprocessor/esp32c6/`). Its
complementary **host driver** IS vendored and is in the table above --
see [esp-hosted-host.md](esp-hosted-host.md) for how the two halves differ.

## One-line summaries

- **ThreadX** -- Preemptive RTOS kernel under 45 example applications (39 of
  them hw_validated), the vendored middleware, and the e-reader NS image.
- **NetX Duo** -- Dual IPv4/IPv6 TCP/IP stack over wired Ethernet and over the
  ESP32-C6 Wi-Fi link. TCP/IP core only: NetX Secure is compiled by nothing
  and TLS comes from Mbed TLS.
- **FileX** -- FAT12/16/32 file system used by two demo applications. No
  exFAT: the vendored snapshot ships none, and this firmware's exFAT is the
  first-party `libs/ra8_fs/`.
- **USBX** -- USB host / device stack for the CDC, HID, and MSC demos.
- **LevelX** -- NOR-flash wear-levelling on Octo-SPI, both under FileX and
  standalone (no ThreadX, no FileX) beneath `libs/ra8_cache_store/`.
- **Mbed TLS** -- TLS record layer and X.509 handling consumed via
  `libs/ra8_tls/` and `libs/ra8_ota/`.
- **TF-PSA-Crypto** -- PSA Crypto API implementation backing TLS, OTA
  signature checks, and the secure-side key vault.
- **Apache NimBLE** -- Bluetooth 5.4 host + controller stack staged for
  future BLE bring-up; not yet linked to an example.
- **litehtml** -- HTML/CSS layout engine for the EPUB reader.
- **miniz** -- Deflate / inflate / ZIP support behind five decode paths: EPUB,
  CBZ, PNG, gzip, and the `ra8_io` compress-on-write fabric seam.
- **XZ Embedded** (decode-only) -- XZ/LZMA2 decoding for wrapped archive
  content (`.tar.xz`) behind the bounded `libs/ra8_unarch` wrapper.
- **stb** -- JPEG / PNG / GIF / BMP decoding (`stb_image`, via
  `libs/ra8_reflow/` and `libs/ra8_rabook_compile/`) and TTF rasterization
  (`stb_truetype`) for the EPUB reader.
- **libwebp** (decode-only) -- WebP (VP8 / VP8L) decoding for longstrip / manga
  raster content, reached through the `libs/ra8_webp/` facade. Wired for band
  tiles via the JOF producer; the `ra8_reflow` inline small-image path is
  still `stb_image`-only (#637).
- **TinyXML-2** -- XML parser for EPUB container metadata.
- **TFLite-micro** -- On-device neural-network inference runtime
  (MicroInterpreter + a lean reference-kernel set) for the RA8P1 Ethos-U55 NPU.
- **FlatBuffers** -- Zero-copy serialization headers for the `.tflite` model
  format TFLite-micro reads.
- **gemmlowp** -- Fixed-point math headers TFLite-micro's quantized reference
  kernels depend on.
- **ruy** -- A single profiler-instrumentation stub header included by
  TFLite-micro kernel utilities (no ruy GEMM backend).
- **esp-hosted host driver** -- The RA8D2-side driver for the ESP32-C6
  wireless co-processor: transport framing, RPC codec and the serial channel.
  Vendored without the upstream ESP-IDF port; the first-party port at
  `port/esp-hosted/` supplies that seam. Eight translation units compile
  behind `RA8_USE_ESP_HOSTED` into five hw_validated applications, and the
  protocol round-trip is proven on silicon.
- **protobuf-c** -- Protocol Buffers C runtime backing the esp-hosted RPC
  codec; a git submodule upstream, so it is pinned and licensed separately
  (BSD-2-Clause) inside `libs/third_party/esp-hosted/common/protobuf-c/`.
- **Vela** (host tool) -- Arm's offline Ethos-U model compiler; runs at build
  time, links nothing into firmware. See [vela.md](vela.md).
- **Espressif esp-hosted-mcu** -- ESP32-C6 wireless co-processor firmware
  (Wi-Fi today; BLE planned) built from a pinned upstream commit and flashed
  onto the C6.
  Not vendored and not linked into the RA8 image; recipe in `coprocessor/esp32c6/`.

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
(`make sbom` / `make sbom-check`); the component registry it renders lives in
the sibling module
[`../../scripts/gen/sbom_registry.py`](../../scripts/gen/sbom_registry.py)
and is the single
source of truth for the version / license / purl / provenance fields. When
you bump or re-vendor a component here, update that registry and run
`make sbom` so the SBOM and inventory do not drift (enforced in CI and the
pre-commit hook).

Commit-pinned components are additionally scanned for published CVEs every
week: `.github/workflows/osv-scan.yml` downloads a pinned `osv-scanner`
release and runs [`../../scripts/checks/osv_scan.sh`](../../scripts/checks/osv_scan.sh),
which queries OSV.dev both with the SBOM purls and with each recorded
upstream commit (the form OSV actually resolves for git-vendored C/C++).

## The pins are checked, not asserted

Every document in this directory makes the same load-bearing claim -- this
tree is what upstream published -- and until #548 nothing verified it. The
SBOM's integrity digest (#538) is re-derived from `libs/third_party/` on every
run, which proves the tree has not changed since the SBOM was regenerated; it
cannot prove the tree was right when it was vendored, because a bad copy is
hashed just as faithfully as a good one.

So each component's upstream revision is now recorded in the registry
(`upstream_ref` / `upstream_commit`, or a SHA-256-pinned release artifact for
miniz's amalgamation), and
[`../sbom/upstream/`](../sbom/upstream/) holds one manifest per component
listing **the git blob SHA-1 upstream publishes for every file we vendor**.
Those hashes are written by a real fetch of the upstream project
(`scripts/checks/check_soup_upstream.py --refresh`), never derived from our own
tree, so the offline `soup-upstream` gate compares two independently produced
hashes rather than a value against itself. The weekly
`soup-upstream-refresh` job re-fetches and fails if a pinned ref has moved
under us.

Deliberate deviations are declared in the registry (`patched_files`,
`local_files`) with a justification, and `--refresh` REFUSES to record a
deviation the registry has not declared -- otherwise a corrupted file would be
quietly re-recorded as "modified on purpose" and the gate would go green having
absorbed it. As of 2026-08-04: 21 components, 9420 vendored files, 9401
byte-identical to their pinned upstream revision, 19 declared deviations.

Applying that check for the first time found five undeclared deviations that
this catalog described as "unmodified": the `[attr]` edit to all five Eclipse
ThreadX `.gitattributes` files, CRLF-converted USBX `.inf` templates, and a
vendor-in formatter sweep over miniz, TinyXML-2 and `stb_image.h`. Everything
that could be restored to upstream's bytes was; the rest is enumerated in the
component's "Deviations / patches" section.

## Review cadence

Each document is re-reviewed at most 12 months after its "Last review
date". Re-review must update version numbers, re-check the upstream
issue / advisory tracker, and bump both dates.
