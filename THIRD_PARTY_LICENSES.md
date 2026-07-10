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
[`scripts/utils/gen_sbom.py`](scripts/utils/gen_sbom.py); when you re-vendor
a component, update that registry and run `make sbom`.

> Closes the aggregation half of recon seed **T5-14** (SOUP-5). The
> provenance-pinning half is **T5-09** (SOUP-1) and the toolchain-pinning
> prerequisite is **T5-02**; both are called out below where they bear on a
> component.

---

## Scope

- **Covered here:** the vendored Software Of Unknown Provenance (SOUP) under
  `libs/third_party/` and the one bundled font data asset under `libs/fonts/`.
- **NOT covered (first-party, MIT):** all hand-written code under `libs/`,
  `src/`, `examples/`, `port/`, `tools/`, `tests/`, and `scripts/` is
  first-party and licensed under the root MIT `LICENSE.txt`. In particular
  `port/` is the project's own RA8D2 glue for the middleware (project Ring /
  World tags, project copyright) -- it is NOT vendored SOUP.
- **Generated font tables** (`libs/fonts/literata_latin1.h` and the `.ttf`
  bytes it bakes) are derived data, not hand-authored; their license follows
  the source font (see the Literata entry -- SIL OFL 1.1, redistributable).

---

## License verdict and elections

The source tree is license-clean: every component is **MIT**, **BSD-3-Clause**,
**zlib**, **Apache-2.0**, **OFL-1.1** (the bundled font), or **public domain
(MIT OR Unlicense)**. There is no copyleft contamination of the MIT firmware.

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
| Apache NimBLE | 1.9.0 (`version.yml`=0.0.0) | Apache-2.0 | `libs/third_party/nimble/` | <https://github.com/apache/mynewt-nimble> |
| litehtml | unpinned dev snapshot (CMake 0.0.0) | BSD-3-Clause | `libs/third_party/litehtml/` | <https://github.com/litehtml/litehtml> |
| miniz | 11.0.2 | MIT (zlib-style) | `libs/third_party/miniz/` | <https://github.com/richgel999/miniz> |
| stb (stb_image + stb_truetype) | image 2.30 / truetype 1.26 | MIT OR Unlicense (public domain) | `libs/third_party/stb/` | <https://github.com/nothings/stb> |
| TinyXML-2 (**patched**) | 11.0.0 | Zlib | `libs/third_party/tinyxml2/` | <https://github.com/leethomason/tinyxml2> |
| TFLite-micro | git `fddd3707` | Apache-2.0 | `libs/third_party/tflite-micro/` | <https://github.com/tensorflow/tflite-micro> |
| FlatBuffers | 25.9.23 | Apache-2.0 | `libs/third_party/flatbuffers/` | <https://github.com/google/flatbuffers> |
| gemmlowp | git `719139ce` | Apache-2.0 | `libs/third_party/gemmlowp/` | <https://github.com/google/gemmlowp> |
| ruy | git `d3712831` | Apache-2.0 | `libs/third_party/ruy/` | <https://github.com/google/ruy> |
| Renesas RSIP-E50D fw (`r_sce_AMC`) | FSP @ `40bbaa11` | BSD-3-Clause | `libs/third_party/fsp_blobs/r_sce_AMC/` | <https://github.com/renesas/fsp> |
| Renesas BLE controller patch (**not vendored**) | FSP (Renesas SLA) | Renesas SLA | `libs/third_party/fsp_blobs/ble_patch/` (absent) | <https://github.com/renesas/fsp> |
| Literata (**bundled font**) | 3.103 | OFL-1.1 | `libs/fonts/Literata-Regular.ttf` | <https://github.com/googlefonts/literata> |

Counts: **16 vendored source components** + **1 blob tree** (`fsp_blobs/`, holding
the vendored RSIP-E50D firmware and the absent BLE patch) + **1 bundled font
asset**. TinyXML-2 carries a local in-tree patch (see below), so it is
*modified* SOUP. The four ML-stack components (TFLite-micro, FlatBuffers,
gemmlowp, ruy) are commit-pinned and unmodified. Separately, **Arm Ethos-U
Vela** is a build-time host tool (pinned at `tools/vela/requirements.txt`),
linked into nothing -- see the build-tools note below and
[`docs/SOUP/vela.md`](docs/SOUP/vela.md).

---

## Provenance and integrity

Only the Renesas RSIP blob is pinned to an upstream commit with an integrity
hash. Every source component's version is *inferred from an in-tree header*
and no upstream commit or `SHA256SUMS` manifest is recorded -- this is the
open **T5-09** finding; it means the trees are not independently reproducible
or tamper-verifiable yet.

| Component | Version source | Upstream commit | Integrity hash | Pinned? |
|-----------|----------------|-----------------|----------------|---------|
| ThreadX / NetX Duo / FileX / USBX / LevelX | `*_MAJOR/MINOR/PATCH_VERSION` macros | none | none | version only |
| Mbed TLS | `MBEDTLS_VERSION_STRING_FULL` | none | none | version only |
| TF-PSA-Crypto | `TF_PSA_CRYPTO_VERSION_STRING_FULL` | none | none | version only |
| miniz | `MZ_VERSION` | none | none | version only |
| TinyXML-2 | `TIXML2_*_VERSION` | none | none | version only (+patch) |
| stb | header-tail version comments | none | none | version only |
| Apache NimBLE | `RELEASE_NOTES.md` prose; `version.yml`=0.0.0 | none | none | **unpinned (dev snapshot)** |
| litehtml | none (CMake project 0.0.0) | none | none | **unpinned (dev snapshot)** |
| TFLite-micro | commit pin (lean subset) | `fddd3707a3c5733af4cb866f18650441e6712504` | none | **commit-pinned** |
| FlatBuffers | tag `v25.9.23` (+ `FLATBUFFERS_VERSION_*`) | `edbe17738352418245d7228e7fd9f12c3ddc34c4` | none | **commit-pinned** |
| gemmlowp | commit pin | `719139ce755a0f31cbf1c37f7f98adcc7fc9f425` | none | **commit-pinned** |
| ruy | commit pin | `d37128311b445e758136b8602d1bbd2a755e115d` | none | **commit-pinned** |
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
- **stb** -- public domain, dual `MIT OR Unlicense`. Sean Barrett
  (nothings.org). The license text lives ONLY in the tails of
  `stb_image.h` / `stb_truetype.h` -- there is no standalone `LICENSE` file
  in `libs/third_party/stb/` (see Open items).
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
  `libs/fonts/Literata-OFL.txt`.

### TinyXML-2 local modification

TinyXML-2 is *not* pristine: a single behaviour-preserving in-TU patch (#151)
generalizes the `XMLDocument::Identify` `PEDANTIC_WHITESPACE` branch so the
on-device `.rabook` compiler round-trips byte-identically. The default parser
behaviour is unchanged. Full description:
[`docs/SOUP/tinyxml2.md`](docs/SOUP/tinyxml2.md) ("Deviations / patches").
Because the tree is modified, its zlib obligation to "not misrepresent the
original software" is met by this disclosure.

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

1. **No commit pins / integrity manifest for 12 source components (T5-09).**
   Versions are inferred from headers only. Adopt the `fsp_blobs` pattern
   (commit SHA + per-component SHA-256) or convert to submodules / a
   vendoring lockfile.
2. **litehtml and NimBLE are unreleased 0.0.0 dev snapshots (T5-09 / SOUP-4).**
   Re-vendor at tagged releases. litehtml is the weakest provenance and is on
   the untrusted-EPUB path (linked via `libs/ra_reflow`).
3. **stb has no standalone `LICENSE` file (SOUP-5).** The `MIT OR Unlicense`
   text exists only in the header tails. A standalone
   `libs/third_party/stb/LICENSE` would make the attribution self-contained.
4. **No automated CVE/SBOM scanning yet (T5-09 / SOUP-3).** The SBOM this file
   accompanies is the input for `osv-scanner`; wiring a weekly scan closes the
   monitoring gap (currently a manual <=12-month re-review cadence).

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
- [`scripts/utils/gen_sbom.py`](scripts/utils/gen_sbom.py) -- the generator /
  validator and its component registry.
- [`docs/SOUP/`](docs/SOUP/) -- per-component qualification (service history,
  CVE notes, integration seams, re-review cadence).
- [`libs/third_party/fsp_blobs/README.md`](libs/third_party/fsp_blobs/README.md)
  -- the gold-standard pinned-blob provenance pattern.
- [`LICENSE.txt`](LICENSE.txt) -- the MIT license for all first-party code.
