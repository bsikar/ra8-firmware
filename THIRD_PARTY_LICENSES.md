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
> provenance-pinning half, **T5-09** (SOUP-1), is closed too: every vendored
> component is pinned to an upstream revision and verified against it file by
> file (#538, #548). The toolchain-pinning prerequisite **T5-02** is called out
> below where it bears on a component.

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
| Apache NimBLE | 1.10.0 (tag `nimble_1_10_0_tag`, git `a7a156f2`) | Apache-2.0 | `libs/third_party/nimble/` | <https://github.com/apache/mynewt-nimble> |
| litehtml | git `8836bc1b` (post-v0.9 dev snapshot) | BSD-3-Clause | `libs/third_party/litehtml/` | <https://github.com/litehtml/litehtml> |
| miniz | 11.0.2 (`MZ_VERSION`; release artifact `miniz-3.0.2.zip`) | MIT (zlib-style) | `libs/third_party/miniz/` | <https://github.com/richgel999/miniz> |
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
| Literata (**bundled font**) | 3.103 | OFL-1.1 | `libs/ra8_fonts/Literata-Regular.ttf` | <https://github.com/googlefonts/literata> |

Counts: **20 vendored source components** + **1 bundled font asset**. One of
the twenty (protobuf-c) is *nested*: upstream esp-hosted carries it as a git
submodule, so it is pinned and licensed in its own right rather than folded
into its parent. Ten of the twenty-one carry a declared deviation from
their upstream pin -- TinyXML-2, libwebp and stb each an in-tree code patch, the
five Eclipse ThreadX trees a `.gitattributes` edit, and Mbed TLS /
TF-PSA-Crypto their build-generated sources -- so those are *modified* SOUP;
the other eleven are byte-identical to their pin. Every deviation is
enumerated in the component's `docs/SOUP/*.md` and machine-checked (see
"Provenance and integrity" below). Separately, **Arm Ethos-U
Vela** is a build-time host tool (pinned at `tools/vela/requirements.txt`),
linked into nothing -- see the build-tools note below and
[`docs/SOUP/vela.md`](docs/SOUP/vela.md).

---

## Provenance and integrity

**All twenty-one vendored components are now pinned to an upstream revision and
verified against it file by file.** Ten of them had no upstream pin at all
until #548 -- their version was read out of a header in our own tree, which
says what the code calls itself, not where it came from. Each was resolved by
fingerprinting the vendored files against the upstream project's published
history, and the resolved revision is recorded in
`scripts/gen/sbom_registry.py` and re-verified on every CI run.

The "verified" column is *upstream-identical files / vendored files*. Where
they differ, the difference is a deliberate deviation enumerated in that
component's `docs/SOUP/*.md` and declared in the registry; nothing else is
permitted to differ.

| Component | Upstream revision | Verified | Deviations |
|-----------|-------------------|----------|------------|
| ThreadX | tag `v6.5.0.202601_rel` `3726d7906b4808bfec7855fc088e073199df9120` | 4757/4758 | 1 patched (`.gitattributes`) |
| NetX Duo | tag `v6.5.0.202601_rel` `8b6e03ac30ab688bec02c69d42f2304b7f72a202` | 1226/1227 | 1 patched (`.gitattributes`) |
| FileX | tag `v6.5.0.202601_rel` `bb6e295af079f3cd903272982106b0ddd9537422` | 266/267 | 1 patched (`.gitattributes`) |
| USBX | tag `v6.5.0.202601_rel` `6dc0cf233d5b7ee6e1a7434581964975f8d8d37b` | 1035/1036 | 1 patched (`.gitattributes`) |
| LevelX | tag `v6.5.0.202601_rel` `a46b74fb8aa133796ccbc13e7902cb8bb818e12f` | 89/90 | 1 patched (`.gitattributes`) |
| Mbed TLS | `development` `d12fbb991c0822f347bbc569badef904629ce605` | 252/256 | 1 patched, 3 generated |
| TF-PSA-Crypto | `development` `bbf1eaf5f4a72bcc3e0cfe854e0313c93b75cd77` | 217/222 | 5 generated |
| Apache NimBLE | tag `nimble_1_10_0_tag` `a7a156f28954819e158b62dd613008f22f9cf73b` | 827/827 | none |
| litehtml | `8836bc1bc35ca0cfd71dc0386ef841d5cbc3bd5e` | 215/215 | none |
| miniz | release artifact `miniz-3.0.2.zip`, SHA-256 `ada38db0...5332c5` | 3/3 | none |
| XZ Embedded | tag `v2024-12-30` `ae63ae3a36ed01724674e8f3d750dc47bf125410` | 11/11 | none (8 relocated) |
| stb | `31c1ad37456438565541f4919958214b6e762fb4` | 1/4 | 1 patched, 2 first-party |
| libwebp (decode-only) | tag `v1.5.0` `a4d7a715337ded4451fec90ff8ce79728e04126c` | 101/102 | 1 patched (arena allocator) |
| TinyXML-2 | tag `11.0.0` `9148bdf719e997d1f474be6bcc7943881046dba1` | 2/3 | 1 patched (#151) |
| TFLite-micro | `fddd3707a3c5733af4cb866f18650441e6712504` | 278/278 | none |
| FlatBuffers | tag `v25.9.23` `187240970746d00bbd26b0f5873ed54d2477f9f3` | 31/31 | none |
| gemmlowp | `719139ce755a0f31cbf1c37f7f98adcc7fc9f425` | 7/7 | none |
| ruy | `d37128311b445e758136b8602d1bbd2a755e115d` | 2/2 | none |
| esp-hosted host driver | `949bb30612747a3bd9e402eda8d01fbfa1f8503e` | 77/77 | none |
| protobuf-c (nested) | `abc67a11c6db271bedbb9f58be85d6f4e2ea8389` | 3/3 | none |
| Literata | tag `3.103` `0c2761b727a1b3a7cffd313c37f0f5163dfc7a63` | 1/1 | none (1 relocated) |

**Totals: 21 components, 9420 vendored files, 9401 byte-identical to their
pinned upstream revision, 19 declared deviations.**

### Why there are no hash values in this table

Two different machine checks stand behind the table above, and both re-derive
their evidence rather than reading a number written here.

1. **Tamper-verifiability** (#538). `scripts/gen/gen_sbom.py` re-derives a
   SHA-256 over each vendored component's whole tree on every run -- sorted
   component-relative paths, git file mode and file content, each
   length-framed -- and publishes it in the SBOM alongside the file count that
   went into it. `gen_sbom.py --check` runs in the `sbom` gate, so a single
   mutated byte under `libs/third_party/` fails CI and names the component.
2. **Upstream identity** (#548). `scripts/checks/check_soup_upstream.py`
   compares every vendored file against the git blob SHA-1 its *upstream
   project* publishes for the pinned revision, recorded per component in
   `docs/sbom/upstream/*.manifest` by a real fetch of that project. The
   `soup-upstream` gate runs it offline on every push; the weekly
   `soup-upstream-refresh` job re-fetches and fails if a pinned ref has moved.

The integrity column used to say `none` for nineteen of the twenty-three
components and carry a hand-transcribed `aggregate SHA-256` for four -- and
**nothing had ever computed any of them**. `--check` compared a constant
against itself, so appending a line to a vendored source still printed `SBOM
matches the tree` with status 0. Transcribing values back into this table would
recreate exactly that: a number written once, true once, with no mechanism that
would notice when it stopped being true. Read the digests from
`docs/sbom/ra8-firmware.cdx.json` and the per-file upstream hashes from
`docs/sbom/upstream/`, both regenerated from their sources.

This closes **T5-09** in full. Applying the upstream check for the first time
found five undeclared deviations this document had described as unmodified: the
`[attr]` edit to five Eclipse ThreadX `.gitattributes` files, CRLF-converted
USBX `.inf` templates, and a vendor-in formatter sweep over miniz, TinyXML-2
and `stb_image.h`. Everything restorable was restored to upstream's bytes; what
remains is enumerated above and justified per component.

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

1. **litehtml is a dev-branch snapshot, not a tagged release (SOUP-4).**
   It is pinned to its exact upstream commit (byte-identical tree
   fingerprint; see the provenance table), which closes the unpinned half
   of the finding, but re-vendoring at a tagged release remains preferable
   -- litehtml is on the untrusted-EPUB path (linked via
   `libs/ra8_reflow`). NimBLE was the other half of this finding and is
   now resolved: it is vendored at the `nimble_1_10_0_tag` release tag
   (#508).
2. **stb has no standalone `LICENSE` file (SOUP-5).** The `MIT OR Unlicense`
   text exists only in the header tails. A standalone
   `libs/third_party/stb/LICENSE` would make the attribution self-contained.
3. **CVE monitoring resolves only for commit-pinned components (SOUP-3).** The
   weekly [`osv-scan.yml`](.github/workflows/osv-scan.yml) workflow runs the
   pinned `osv-scanner` release against the SBOM and against every recorded
   upstream commit (`scripts/checks/osv_scan.sh`). OSV.dev resolves C/C++
   advisories by GIT commit range only -- GitHub purls do not resolve. Every
   vendored component now carries a commit pin (#548), so the ten that used to
   be version-only are commit-queried too; the one remaining gap is miniz,
   whose amalgamation is pinned by release-artifact SHA-256 rather than by a
   commit and therefore has nothing for OSV to range-query. It keeps the
   manual <=12-month re-review cadence (NetX Duo CVE notes live in
   `docs/SOUP/netxduo.md`).

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
- [`docs/VENDOR_BLOBS.md`](docs/VENDOR_BLOBS.md) -- the vendor-blob
  procurement route: what is deliberately not in this tree, and why.
- [`LICENSE.txt`](LICENSE.txt) -- the MIT license for all first-party code.
