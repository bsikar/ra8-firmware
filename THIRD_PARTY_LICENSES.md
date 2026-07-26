# Third-Party License Inventory

This file is the aggregated attribution artifact for every piece of
third-party code and data shipped in the `ra8-firmware` tree. It exists to
satisfy the redistribution obligations the vendored components carry --
Apache-2.0 section 4(d) (propagate `NOTICE` contents) and BSD-3-Clause /
zlib (reproduce the copyright and license text) -- which the MIT root
[`LICENSE.txt`](LICENSE.txt) alone does not discharge. It is the human
companion to the machine-readable SBOM at
[`docs/sbom/ra8-firmware.cdx.json`](docs/sbom/ra8-firmware.cdx.json) and to
the per-component qualification catalog under [`docs/SOUP/`](docs/SOUP/).

Both this file and the SBOM are generated/checked from one registry in
[`scripts/gen/gen_sbom.py`](scripts/gen/gen_sbom.py); when you re-vendor
a component, update that registry and run `make sbom`.

> Closes the aggregation half of recon seed **T5-14** (SOUP-5). The
> provenance-pinning half is **T5-09** (SOUP-1) and the toolchain-pinning
> prerequisite is **T5-02**; both are called out below where they bear on a
> component.

---

## Scope

- **Covered here:** the vendored Software Of Unknown Provenance (SOUP) under
  `libs/third_party/` and the one bundled font data asset under `libs/ra8_fonts/`.
- **NOT covered (first-party, MIT):** all hand-written code under `libs/`,
  `src/`, `examples/`, `port/`, `tools/`, `tests/`, and `scripts/` is
  first-party and licensed under the root MIT `LICENSE.txt`. In particular
  `port/` is the project's own RA8D2 glue for the middleware (project Ring /
  World tags, project copyright) -- it is NOT vendored SOUP.
- **Generated font tables** (`libs/ra8_fonts/literata_latin1.h` and the `.ttf`
  bytes it bakes) are derived data, not hand-authored; their license follows
  the source font (see the Literata entry -- SIL OFL 1.1, redistributable).

---

## License verdict and elections

The source tree is license-clean: every component is **MIT**, **BSD-2-Clause**,
**BSD-3-Clause**, **0BSD**, **zlib**, **Apache-2.0**, **OFL-1.1** (the bundled
font), or **public domain (MIT OR Unlicense)**. All are permissive; there is no
copyleft contamination of the MIT firmware.

**Dual-license elections (recorded per the license terms):**

- **Mbed TLS 4.1.0** is offered under `Apache-2.0 OR GPL-2.0-or-later`.
  `ra8-firmware` **elects Apache-2.0**. The GPL-2.0 option is NOT taken.
- **TF-PSA-Crypto 1.1.0** is offered under `Apache-2.0 OR GPL-2.0-or-later`.
  `ra8-firmware` **elects Apache-2.0**. The GPL-2.0 option is NOT taken.

**Apache-2.0 NOTICE:** Apache NimBLE ships its own
[`libs/third_party/nimble/NOTICE`](libs/third_party/nimble/NOTICE); its
contents must be reproduced in any binary distribution that links NimBLE.
Mbed TLS and TF-PSA-Crypto carry no separate `NOTICE` beyond their `LICENSE`.

---

## Inventory

| Component | Version | License (SPDX) | In-tree path | Upstream |
|-----------|---------|----------------|--------------|----------|
| Eclipse ThreadX | 6.5.0 | MIT | `libs/third_party/threadx/` | <https://github.com/eclipse-threadx/threadx> |
| Eclipse NetX Duo | 6.5.0 | MIT | `libs/third_party/netxduo/` | <https://github.com/eclipse-threadx/netxduo> |
| Eclipse FileX | 6.5.0 | MIT | `libs/third_party/filex/` | <https://github.com/eclipse-threadx/filex> |
| Eclipse USBX | 6.5.0 | MIT | `libs/third_party/usbx/` | <https://github.com/eclipse-threadx/usbx> |
| Eclipse LevelX | 6.5.0 | MIT | `libs/third_party/levelx/` | <https://github.com/eclipse-threadx/levelx> |
| Mbed TLS | 4.1.0 | Apache-2.0 (elected; dual w/ GPL-2.0) | `libs/third_party/mbedtls/` | <https://github.com/Mbed-TLS/mbedtls> |
| TF-PSA-Crypto | 1.1.0 | Apache-2.0 (elected; dual w/ GPL-2.0) | `libs/third_party/tf-psa-crypto/` | <https://github.com/Mbed-TLS/TF-PSA-Crypto> |
| Apache NimBLE | git `8b6f3e81` (post-1.9.0 dev snapshot) | Apache-2.0 | `libs/third_party/nimble/` | <https://github.com/apache/mynewt-nimble> |
| litehtml | git `8836bc1b` (post-v0.9 dev snapshot) | BSD-3-Clause | `libs/third_party/litehtml/` | <https://github.com/litehtml/litehtml> |
| miniz | 11.0.2 | MIT (zlib-style) | `libs/third_party/miniz/` | <https://github.com/richgel999/miniz> |
| XZ Embedded (decode-only) | tag `v2024-12-30` (git `ae63ae3a`) | 0BSD | `libs/third_party/xz_embedded/` | <https://github.com/tukaani-project/xz-embedded> |
| stb (stb_image + stb_truetype) | image 2.30 / truetype 1.26 | MIT OR Unlicense (public domain) | `libs/third_party/stb/` | <https://github.com/nothings/stb> |
| libwebp (decode-only, **patched**) | 1.5.0 | BSD-3-Clause (+ PATENTS grant) | `libs/third_party/libwebp/` | <https://chromium.googlesource.com/webm/libwebp> |
| TinyXML-2 (**patched**) | 11.0.0 | Zlib | `libs/third_party/tinyxml2/` | <https://github.com/leethomason/tinyxml2> |
| TFLite-micro | git `fddd3707` | Apache-2.0 | `libs/third_party/tflite-micro/` | <https://github.com/tensorflow/tflite-micro> |
| FlatBuffers | 25.9.23 | Apache-2.0 | `libs/third_party/flatbuffers/` | <https://github.com/google/flatbuffers> |
| gemmlowp | git `719139ce` | Apache-2.0 | `libs/third_party/gemmlowp/` | <https://github.com/google/gemmlowp> |
| ruy | git `d3712831` | Apache-2.0 | `libs/third_party/ruy/` | <https://github.com/google/ruy> |
| esp-hosted host driver | 2.12.11 (git `949bb30`) | Apache-2.0 | `libs/third_party/esp-hosted/` | <https://github.com/espressif/esp-hosted-mcu> |
| protobuf-c (nested in esp-hosted) | 1.4.1 (git `abc67a11`) | BSD-2-Clause | `libs/third_party/esp-hosted/common/protobuf-c/` | <https://github.com/protobuf-c/protobuf-c> |
| Renesas RSIP-E50D fw (`r_sce_AMC`) | FSP @ `40bbaa11` | BSD-3-Clause | `libs/third_party/fsp_blobs/r_sce_AMC/` | <https://github.com/renesas/fsp> |
| Renesas BLE controller patch (**not vendored**) | FSP (Renesas SLA) | Renesas SLA | `libs/third_party/fsp_blobs/ble_patch/` (absent) | <https://github.com/renesas/fsp> |
| Literata (**bundled font**) | 3.103 | OFL-1.1 | `libs/ra8_fonts/Literata-Regular.ttf` | <https://github.com/googlefonts/literata> |

Counts: **20 vendored source components** + **1 blob tree** (`fsp_blobs/`, holding
the vendored RSIP-E50D firmware and the absent BLE patch) + **1 bundled font
asset**. One of the twenty (protobuf-c) is *nested*: upstream esp-hosted carries
it as a git submodule, so it is pinned and licensed in its own right rather than
folded into its parent. TinyXML-2 and libwebp each carry a local in-tree patch
(see below), so both are *modified* SOUP. The four ML-stack components
(TFLite-micro, FlatBuffers, gemmlowp, ruy), the two dev-branch snapshots
(Apache NimBLE, litehtml) and the two esp-hosted components are commit-pinned
and unmodified; libwebp is commit-pinned (release tag
`v1.5.0`) but modified (one allocator-fronting patch). Separately, **Arm Ethos-U
Vela** is a build-time host tool (pinned at `tools/vela/requirements.txt`),
linked into nothing -- see the build-tools note below and
[`docs/SOUP/vela.md`](docs/SOUP/vela.md).

The esp-hosted host driver is vendored but **not yet compiled by any target**:
it needs the first-party port under `port/esp-hosted/` before it can build, and
the port plus the CMake wiring are a follow-on change. See
[`docs/SOUP/esp-hosted-host.md`](docs/SOUP/esp-hosted-host.md).

---

## Provenance and integrity

Four components are pinned to an upstream commit *with* an integrity hash: the
Renesas RSIP blob, XZ Embedded, and the two esp-hosted components. Eleven
components carry an upstream commit pin (the ML
stack, the RSIP blob, the NimBLE / litehtml dev snapshots and XZ Embedded --
those three recovered by fingerprinting the vendored trees against their
upstream histories, each a byte-identical single-commit match -- libwebp,
pinned to release tag `v1.5.0` and byte-identical except its one
allocator-fronting patch, and the esp-hosted host driver plus its nested
protobuf-c, both pinned at vendor-in and verified file-by-file). The remaining ten source components'
versions are *inferred from an in-tree header* with no upstream commit or
`SHA256SUMS` manifest recorded -- the open **T5-09** finding; those trees
are not independently reproducible or tamper-verifiable yet.

| Component | Version source | Upstream commit | Integrity hash | Pinned? |
|-----------|----------------|-----------------|----------------|---------|
| ThreadX / NetX Duo / FileX / USBX / LevelX | `*_MAJOR/MINOR/PATCH_VERSION` macros | none | none | version only |
| Mbed TLS | `MBEDTLS_VERSION_STRING_FULL` | none | none | version only |
| TF-PSA-Crypto | `TF_PSA_CRYPTO_VERSION_STRING_FULL` | none | none | version only |
| miniz | `MZ_VERSION` | none | none | version only |
| TinyXML-2 | `TIXML2_*_VERSION` | none | none | version only (+patch) |
| stb | header-tail version comments | none | none | version only |
| libwebp (decode-only) | release tag `v1.5.0` (byte-identical except 1 patched TU) | `a4d7a715337ded4451fec90ff8ce79728e04126c` | none | **commit-pinned** (+patch) |
| Apache NimBLE | tree fingerprint vs upstream (859/859 files byte-identical) | `8b6f3e819118a1839e5f238bfe1797d64878dc3d` | none | **commit-pinned** |
| litehtml | tree fingerprint vs upstream (215/215 files byte-identical) | `8836bc1bc35ca0cfd71dc0386ef841d5cbc3bd5e` | none | **commit-pinned** |
| XZ Embedded | tree fingerprint vs upstream (11/11 files byte-identical, tag `v2024-12-30`) | `ae63ae3a36ed01724674e8f3d750dc47bf125410` | aggregate SHA-256 `9dc6c2af...c95dd9` | **fully pinned (gold standard)** |
| TFLite-micro | commit pin (lean subset) | `fddd3707a3c5733af4cb866f18650441e6712504` | none | **commit-pinned** |
| FlatBuffers | tag `v25.9.23` (+ `FLATBUFFERS_VERSION_*`) | `edbe17738352418245d7228e7fd9f12c3ddc34c4` | none | **commit-pinned** |
| gemmlowp | commit pin | `719139ce755a0f31cbf1c37f7f98adcc7fc9f425` | none | **commit-pinned** |
| ruy | commit pin | `d37128311b445e758136b8602d1bbd2a755e115d` | none | **commit-pinned** |
| esp-hosted host driver | commit pin (77/77 files byte-identical) | `949bb30612747a3bd9e402eda8d01fbfa1f8503e` | aggregate SHA-256 `79ae0497...d29cc0` | **fully pinned (gold standard)** |
| protobuf-c (nested) | submodule pin + `PROTOBUF_C_VERSION` probe (3/3 files byte-identical) | `abc67a11c6db271bedbb9f58be85d6f4e2ea8389` | aggregate SHA-256 `67da2264...784d7f` | **fully pinned (gold standard)** |
| RSIP-E50D (`r_sce_AMC`) | FSP release | `40bbaa11b1a1b87e0ee0675e401aea6351f90d14` | aggregate SHA-256 `718e4d45...037064` | **fully pinned (gold standard)** |
| BLE patch | not vendored | n/a | n/a | absent |
| Literata | TTF `name` table (3.103) | n/a | none | version only; SIL OFL 1.1 |

The RSIP aggregate SHA-256 (sorted per-file hashes, excluding
`UPSTREAM_LICENSE.md`) and commit are recorded in
[`docs/SOUP/r_sce_AMC_firmware.md`](docs/SOUP/r_sce_AMC_firmware.md). Adopt
that pattern for the other components to close T5-09.

---

## Attribution (copyright and license text)

Each component's full license text ships beside its source (path column
below); this section reproduces the copyright line and points to that text.

- **ThreadX, NetX Duo, FileX, USBX, LevelX** -- MIT.
  "Copyright (c) 2024 - present Microsoft Corporation." Text:
  `libs/third_party/<component>/LICENSE.txt`. Origin: Eclipse Foundation
  (Eclipse ThreadX).
- **Mbed TLS** -- Apache-2.0 (elected). TrustedFirmware.org / Arm. Text:
  `libs/third_party/mbedtls/LICENSE`.
- **TF-PSA-Crypto** -- Apache-2.0 (elected). TrustedFirmware.org. Text:
  `libs/third_party/tf-psa-crypto/LICENSE`.
- **Apache NimBLE** -- Apache-2.0. Apache Software Foundation. Text:
  `libs/third_party/nimble/LICENSE`; **NOTICE:**
  `libs/third_party/nimble/NOTICE` (must be propagated).
- **litehtml** -- BSD-3-Clause. "Copyright (c) 2013, Yuri Kobets (tordex).
  All rights reserved." Text: `libs/third_party/litehtml/LICENSE`.
- **miniz** -- MIT. "Copyright 2013-2014 RAD Game Tools and Valve Software;
  Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC." Text:
  `libs/third_party/miniz/LICENSE`.
- **XZ Embedded** (decode-only) -- 0BSD. "Copyright (C) The XZ Embedded
  authors and contributors" (Lasse Collin; parts based on Igor Pavlov's
  LZMA SDK). Text: `libs/third_party/xz_embedded/COPYING`; contributor
  list: `libs/third_party/xz_embedded/AUTHORS`. Unmodified SOUP -- the
  allocator / mode porting layer is the first-party
  `libs/ra8_unarch/inc/xz_config.h` (see
  [`docs/SOUP/xz_embedded.md`](docs/SOUP/xz_embedded.md)).
- **stb** -- public domain, dual `MIT OR Unlicense`. Sean Barrett
  (nothings.org). The license text lives ONLY in the tails of
  `stb_image.h` / `stb_truetype.h` -- there is no standalone `LICENSE` file
  in `libs/third_party/stb/` (see Open items).
- **libwebp** (decode-only) -- BSD-3-Clause. "Copyright (c) 2010, Google Inc.
  All rights reserved." Text: `libs/third_party/libwebp/COPYING`; the
  additional-IP-rights grant is `libs/third_party/libwebp/PATENTS` and the
  contributor list is `libs/third_party/libwebp/AUTHORS`. Modified SOUP: one
  allocator-fronting patch in `src/utils/utils.c` (see
  [`docs/SOUP/libwebp.md`](docs/SOUP/libwebp.md)).
- **TinyXML-2** -- zlib. Lee Thomason. Text:
  `libs/third_party/tinyxml2/LICENSE.txt`.
- **TFLite-micro** -- Apache-2.0. "Copyright The TensorFlow Authors."
  (Google / TensorFlow). Text: `libs/third_party/tflite-micro/LICENSE`.
- **FlatBuffers** -- Apache-2.0. Copyright Google Inc. Text:
  `libs/third_party/flatbuffers/LICENSE`.
- **gemmlowp** -- Apache-2.0. Copyright The Gemmlowp Authors (Google). Text:
  `libs/third_party/gemmlowp/LICENSE`.
- **ruy** -- Apache-2.0. Copyright The ruy Authors (Google). Text:
  `libs/third_party/ruy/LICENSE`.
- **esp-hosted host driver** -- Apache-2.0. "Copyright Espressif Systems
  (Shanghai) CO LTD" (per-file `SPDX-FileCopyrightText`). Text:
  `libs/third_party/esp-hosted/LICENSE`. Upstream ships no separate `NOTICE`,
  so there is nothing beyond the license text to propagate. Unmodified SOUP;
  the RA8 port that makes it buildable is first-party (see
  [`docs/SOUP/esp-hosted-host.md`](docs/SOUP/esp-hosted-host.md)).
- **protobuf-c** -- BSD-2-Clause. "Copyright (c) 2008-2022, Dave Benson and
  the protobuf-c authors. All rights reserved." Text:
  `libs/third_party/esp-hosted/common/protobuf-c/LICENSE`. Vendored *inside*
  the esp-hosted tree because upstream embeds it there as a git submodule;
  it is a separate upstream project under a separate license and is pinned
  and attributed as one.
- **RSIP-E50D firmware (`r_sce_AMC`)** -- BSD-3-Clause, per-file SPDX.
  Renesas Electronics Corporation. Upstream `LICENSE.md` mirrored at
  `libs/third_party/fsp_blobs/r_sce_AMC/UPSTREAM_LICENSE.md`.
- **BLE controller patch** -- Renesas Software License Agreement; an
  encrypted binary that is NOT in the public BSD-3-Clause FSP and is NOT
  present in this tree. Not linked into MIT code. See
  [`docs/SOUP/ble_patch_image.md`](docs/SOUP/ble_patch_image.md).
- **Literata** -- "Copyright 2017 The Literata Project Authors
  (https://github.com/googlefonts/literata)." Licensed under the SIL Open Font
  License, Version 1.1; the full license text ships at
  `libs/ra8_fonts/Literata-OFL.txt`.

### TinyXML-2 local modification

TinyXML-2 is *not* pristine: a single behaviour-preserving in-TU patch (#151)
generalizes the `XMLDocument::Identify` `PEDANTIC_WHITESPACE` branch so the
on-device `.rabook` compiler round-trips byte-identically. The default parser
behaviour is unchanged. Full description:
[`docs/SOUP/tinyxml2.md`](docs/SOUP/tinyxml2.md) ("Deviations / patches").
Because the tree is modified, its zlib obligation to "not misrepresent the
original software" is met by this disclosure.

### Co-processor firmware (not linked into firmware)

This runs on the companion **ESP32-C6** wireless co-processor, not on the
RA8D2, and ships no code into the RA8 firmware image, so it is carried in the
SBOM component list with `scope: excluded` rather than as linked SOUP:

- **Espressif esp-hosted-mcu** -- Apache-2.0. Espressif Systems. The
  `network_adapter` co-processor firmware that gives the RA8D2 Wi-Fi and
  Bluetooth over a SPI link. This **C6 image** is NOT vendored into the tree:
  it is built from the
  pinned upstream commit `949bb30` with esp-idf `v5.5.4` and flashed onto
  the C6 by `coprocessor/esp32c6/build.sh` / `flash.sh`. Recipe and pins in
  `coprocessor/esp32c6/`; qualification in [`docs/SOUP/esp-hosted.md`](docs/SOUP/esp-hosted.md).
  Do not confuse this with the **host driver** from the same upstream
  repository, which *is* vendored (`libs/third_party/esp-hosted/`), is linked
  into the RA8 image, and appears in the inventory table above.

### Build-time host tools (not linked into firmware)

These run on the developer / CI host at build time and ship no code into the
firmware image, so they are not in the SBOM component list, but they go through
the vendor process (owner requirement) and are recorded here:

- **Arm Ethos-U Vela** -- Apache-2.0. Arm Limited. The offline
  `.tflite -> Ethos-U command-stream` compiler. Pinned at
  `tools/vela/requirements.txt` (`ethos-u-vela==5.1.0`); usage in
  `tools/vela/README.md`; qualification in
  [`docs/SOUP/vela.md`](docs/SOUP/vela.md). Its output command stream is a build
  input consumed on-device by the vendored TFLite-micro `ethos-u` operator.

---

## Open compliance items

These are real gaps, tracked so they are not forgotten. None blocks internal
development (the project is unreleased), but each is a live obligation the
moment a binary is shared.

1. **No commit pins / integrity manifest for 10 source components (T5-09).**
   Versions are inferred from headers only. Adopt the `fsp_blobs` pattern
   (commit SHA + per-component SHA-256) or convert to submodules / a
   vendoring lockfile.
2. **litehtml and NimBLE are dev-branch snapshots, not tagged releases
   (SOUP-4).** Both are now pinned to their exact upstream commits
   (byte-identical tree fingerprints; see the provenance table), which
   closes the unpinned half of the finding. Re-vendoring at tagged releases
   remains preferable; litehtml in particular is on the untrusted-EPUB path
   (linked via `libs/ra8_reflow`).
3. **stb has no standalone `LICENSE` file (SOUP-5).** The `MIT OR Unlicense`
   text exists only in the header tails. A standalone
   `libs/third_party/stb/LICENSE` would make the attribution self-contained.
4. **CVE monitoring only covers commit-pinned components (SOUP-3).** The
   weekly [`osv-scan.yml`](.github/workflows/osv-scan.yml) workflow runs the
   pinned `osv-scanner` release against the SBOM and against every recorded
   upstream commit (`scripts/checks/osv_scan.sh`). OSV.dev resolves C/C++
   advisories by GIT commit range only -- GitHub purls do not resolve -- so
   the ten version-only components (ThreadX family, Mbed TLS, TF-PSA-Crypto,
   miniz, TinyXML-2, stb) are NOT commit-queried until they gain pins under
   item 1, and keep the manual <=12-month re-review cadence (NetX Duo CVE
   notes live in `docs/SOUP/netxduo.md`).

The bundled reading font is no longer an open item: the proprietary Adobe
Arno Pro face was replaced with **Literata** (SIL OFL 1.1) -- open and cleared
for redistribution.

---

## Regenerating and verifying

The SBOM and the registry that backs this inventory are kept in sync by one
generator:

```sh
make sbom          # regenerate docs/sbom/ra8-firmware.cdx.json from the tree
make sbom-check    # fail if the committed SBOM is stale or the tree drifted
```

`gen_sbom.py --check` (invoked by `make sbom-check`, the pre-commit hook, and
the CI pre-commit gate suite) cross-checks the registry against the vendored
tree: it fails on an uncatalogued `libs/third_party/` directory or a version
macro that disagrees with the recorded version, so a newly vendored or bumped
component cannot ship without updating both artifacts.

---

## Cross-references

- [`docs/sbom/ra8-firmware.cdx.json`](docs/sbom/ra8-firmware.cdx.json) -- the
  CycloneDX 1.5 SBOM (machine-readable; feed to `osv-scanner`).
- [`scripts/gen/gen_sbom.py`](scripts/gen/gen_sbom.py) -- the generator /
  validator; the component registry it renders is the sibling module
  [`scripts/gen/sbom_registry.py`](scripts/gen/sbom_registry.py).
- [`scripts/checks/osv_scan.sh`](scripts/checks/osv_scan.sh) +
  [`.github/workflows/osv-scan.yml`](.github/workflows/osv-scan.yml) -- the
  weekly OSV CVE scan (SBOM purl leg + pinned-commit leg).
- [`docs/SOUP/`](docs/SOUP/) -- per-component qualification (service history,
  CVE notes, integration seams, re-review cadence).
- [`libs/third_party/fsp_blobs/README.md`](libs/third_party/fsp_blobs/README.md)
  -- the gold-standard pinned-blob provenance pattern.
- [`LICENSE.txt`](LICENSE.txt) -- the MIT license for all first-party code.
