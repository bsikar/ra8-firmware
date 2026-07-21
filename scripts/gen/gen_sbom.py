#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate and validate the ra8-firmware Software Bill of Materials (SBOM).

This is the supply-chain provenance gate for the vendored third-party SOUP
(Software Of Unknown Provenance) under ``libs/third_party/`` plus the one
bundled font data asset under ``libs/fonts/``.  It emits a machine-readable
CycloneDX 1.5 JSON document at ``docs/sbom/ra8-firmware.cdx.json`` that
records, for every component: name, version, SPDX license (with the
Apache-2.0 election for the dual-licensed crypto), package URL (purl) where
one is meaningful, in-tree path, upstream URL, and provenance class.

The curated ``REGISTRY`` below is the single source of truth for the fields
that cannot be derived mechanically (license election, upstream URL, purl,
provenance).  Everything that CAN be cross-checked against the tree is:

  * **Directory drift** -- every direct child of ``libs/third_party/`` must
    have a registry entry, and every registry directory must exist on disk.
    A newly vendored component with no entry fails the gate.
  * **Version drift** -- for components whose in-tree headers carry a version
    macro (the ThreadX family, Mbed TLS, TF-PSA-Crypto, miniz, TinyXML-2,
    stb), the macro is re-read from source and compared to the recorded
    version.  Versions are never invented; a component with no upstream
    release tag (litehtml, NimBLE dev snapshots) is pinned to the exact
    upstream commit its vendored tree is byte-identical to (T5-09).
  * **License-file presence** -- each entry that names a LICENSE file must
    have it on disk.  stb ships no standalone LICENSE (text in header tails)
    and is reported as a known gap rather than a hard failure.

The emitted JSON is deterministic (no wall-clock timestamp, content-derived
serial number, ``ensure_ascii``) so ``--check`` can compare it byte-for-byte
against the committed file and so the SBOM is reproducible.

Run::

    gen_sbom.py            # regenerate the committed SBOM + print a summary
    gen_sbom.py --check    # fail if the committed SBOM is stale or the tree
                           #   drifted from the registry (the CI/hook gate)
    gen_sbom.py --print    # write nothing; print the SBOM JSON to stdout
    gen_sbom.py --commits  # print `<key> <upstream-commit>` per pinned
                           #   component (consumed by the weekly OSV scan)

Exit 0 if clean, 1 on drift / a catalogued-tree mismatch (including a version
macro that no longer parses). argparse exits 2 on a usage error.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import TextIO

REPO_ROOT = Path(__file__).resolve().parents[2]
THIRD_PARTY_DIR = Path("libs/third_party")
SBOM_REL_PATH = Path("docs/sbom/ra8-firmware.cdx.json")

PROJECT_NAME = "ra8-firmware"
BOM_FORMAT = "CycloneDX"
CYCLONEDX_SPEC = "1.5"
BOM_REVISION = 1
GENERATOR_NAME = "gen_sbom.py"

EXIT_OK = 0
EXIT_DRIFT = 1

# Provenance classes, most-to-least trustworthy.  Recorded per component in a
# CycloneDX property so an auditor can see, at a glance, which components are
# pinned versus inferred.
PROV_COMMIT_PINNED = "commit-pinned-sha256"  # upstream SHA + integrity hash
PROV_VERSION_HEADER = "version-header"  # version from an in-tree header macro
PROV_NOT_VENDORED = "not-vendored"  # documented but absent from the tree
PROV_PROPRIETARY = "proprietary-unresolved"  # license not cleared; see notes
PROV_OPEN_ASSET = "open-asset-versioned"  # cleared open asset (OFL font); version from name table


@dataclass(frozen=True)
class Component:
    """One SOUP or bundled-asset record.

    The required fields describe the component; the optional fields carry the
    provenance and license detail that the SBOM and the cross-checks consume.
    """

    key: str  # registry key; direct child dir name under libs/third_party
    name: str  # human-facing component name
    version: str  # recorded version string ("unpinned ..." when indeterminate)
    ctype: str  # CycloneDX component type: library / firmware / data
    url: str  # canonical upstream URL
    path: str  # in-tree path, repo-root relative
    provenance: str  # one of the PROV_* classes
    description: str  # one-line purpose
    group: str | None = None  # purl / vcs namespace (e.g. github owner)
    purl: str | None = None  # package URL, when one is meaningful
    spdx: str | None = None  # SPDX id or expression (elected license)
    license_name: str | None = None  # non-SPDX license label (proprietary)
    license_note: str | None = None  # free-text license clarification
    license_original: str | None = None  # pre-election dual expression
    license_election: str | None = None  # license we consume it under
    license_file: str | None = None  # LICENSE path (root-relative) if present
    upstream_commit: str | None = None  # pinned upstream commit SHA
    aggregate_sha256: str | None = None  # integrity hash of the vendored tree
    copyright: str | None = None  # copyright string (proprietary assets)
    modified: bool = False  # true if the vendored tree carries a local patch
    scope: str = "required"  # CycloneDX scope: required / excluded
    probe_file: str | None = None  # source file to re-read the version from
    probe_re: str | None = None  # single-capture version regex, group 1
    probe_prefix: str | None = None  # macro prefix for MAJOR/MINOR/PATCH triplet
    expected_version: str | None = None  # version the probe is expected to yield
    extra_notes: tuple[str, ...] = field(default_factory=tuple)  # extra properties


# --------------------------------------------------------------------------- #
# The curated component registry -- single source of truth.                   #
# --------------------------------------------------------------------------- #
# Versions are cross-checked against the tree where a header macro exists; do
# NOT edit a version here without the matching vendored source changing.  Per-
# component qualification lives under docs/SOUP/; the aggregated human license
# inventory is THIRD_PARTY_LICENSES.md.
REGISTRY: tuple[Component, ...] = (
    Component(
        key="threadx",
        name="Eclipse ThreadX",
        version="6.5.0",
        ctype="library",
        group="eclipse-threadx",
        url="https://github.com/eclipse-threadx/threadx",
        path="libs/third_party/threadx",
        provenance=PROV_VERSION_HEADER,
        description="Preemptive RTOS kernel under every threadx_* example.",
        purl="pkg:github/eclipse-threadx/threadx@6.5.0",
        spdx="MIT",
        license_file="libs/third_party/threadx/LICENSE.txt",
        probe_file="common/inc/tx_api.h",
        probe_prefix="THREADX",
        expected_version="6.5.0",
    ),
    Component(
        key="netxduo",
        name="Eclipse NetX Duo",
        version="6.5.0",
        ctype="library",
        group="eclipse-threadx",
        url="https://github.com/eclipse-threadx/netxduo",
        path="libs/third_party/netxduo",
        provenance=PROV_VERSION_HEADER,
        description="Dual IPv4/IPv6 TCP/IP + TLS stack (NetX echo / OTA path).",
        purl="pkg:github/eclipse-threadx/netxduo@6.5.0",
        spdx="MIT",
        license_file="libs/third_party/netxduo/LICENSE.txt",
        probe_file="common/inc/nx_api.h",
        probe_prefix="NETXDUO",
        expected_version="6.5.0",
        extra_notes=(
            "CVE tracking is manual: netxduo.md hand-records "
            "CVE-2025-2258/2259/2260 as fixed (see T5-09 / SOUP-3).",
        ),
    ),
    Component(
        key="filex",
        name="Eclipse FileX",
        version="6.5.0",
        ctype="library",
        group="eclipse-threadx",
        url="https://github.com/eclipse-threadx/filex",
        path="libs/third_party/filex",
        provenance=PROV_VERSION_HEADER,
        description="FAT / exFAT file system (FileX demos + OTA staging).",
        purl="pkg:github/eclipse-threadx/filex@6.5.0",
        spdx="MIT",
        license_file="libs/third_party/filex/LICENSE.txt",
        probe_file="common/inc/fx_api.h",
        probe_prefix="FILEX",
        expected_version="6.5.0",
    ),
    Component(
        key="usbx",
        name="Eclipse USBX",
        version="6.5.0",
        ctype="library",
        group="eclipse-threadx",
        url="https://github.com/eclipse-threadx/usbx",
        path="libs/third_party/usbx",
        provenance=PROV_VERSION_HEADER,
        description="USB host / device stack (CDC, HID, MSC demos).",
        purl="pkg:github/eclipse-threadx/usbx@6.5.0",
        spdx="MIT",
        license_file="libs/third_party/usbx/LICENSE.txt",
        probe_file="common/core/inc/ux_api.h",
        probe_prefix="USBX",
        expected_version="6.5.0",
    ),
    Component(
        key="levelx",
        name="Eclipse LevelX",
        version="6.5.0",
        ctype="library",
        group="eclipse-threadx",
        url="https://github.com/eclipse-threadx/levelx",
        path="libs/third_party/levelx",
        provenance=PROV_VERSION_HEADER,
        description="NOR-flash wear-leveling layer under FileX on Octo-SPI.",
        purl="pkg:github/eclipse-threadx/levelx@6.5.0",
        spdx="MIT",
        license_file="libs/third_party/levelx/LICENSE.txt",
        probe_file="common/inc/lx_api.h",
        probe_prefix="LEVELX",
        expected_version="6.5.0",
    ),
    Component(
        key="mbedtls",
        name="Mbed TLS",
        version="4.1.0",
        ctype="library",
        group="Mbed-TLS",
        url="https://github.com/Mbed-TLS/mbedtls",
        path="libs/third_party/mbedtls",
        provenance=PROV_VERSION_HEADER,
        description="TLS record layer + X.509, consumed via ra8_tls / ra8_ota.",
        purl="pkg:github/Mbed-TLS/mbedtls@4.1.0",
        spdx="Apache-2.0",
        license_original="Apache-2.0 OR GPL-2.0-or-later",
        license_election="Apache-2.0",
        license_note="Dual-licensed; ra8-firmware elects the Apache-2.0 option.",
        license_file="libs/third_party/mbedtls/LICENSE",
        probe_file="include/mbedtls/build_info.h",
        probe_re=r'MBEDTLS_VERSION_STRING_FULL\s+"Mbed TLS ([0-9.]+)"',
        expected_version="4.1.0",
    ),
    Component(
        key="tf-psa-crypto",
        name="TF-PSA-Crypto",
        version="1.1.0",
        ctype="library",
        group="Mbed-TLS",
        url="https://github.com/Mbed-TLS/TF-PSA-Crypto",
        path="libs/third_party/tf-psa-crypto",
        provenance=PROV_VERSION_HEADER,
        description="PSA Crypto API backing TLS, OTA signatures, key vault.",
        purl="pkg:github/Mbed-TLS/TF-PSA-Crypto@1.1.0",
        spdx="Apache-2.0",
        license_original="Apache-2.0 OR GPL-2.0-or-later",
        license_election="Apache-2.0",
        license_note="Dual-licensed; ra8-firmware elects the Apache-2.0 option.",
        license_file="libs/third_party/tf-psa-crypto/LICENSE",
        probe_file="include/tf-psa-crypto/build_info.h",
        probe_re=r'TF_PSA_CRYPTO_VERSION_STRING_FULL\s+"TF-PSA-Crypto ([0-9.]+)"',
        expected_version="1.1.0",
    ),
    Component(
        key="nimble",
        name="Apache NimBLE",
        version="1.9.0+git.8b6f3e81 (2026-04-28 default-branch snapshot)",
        ctype="library",
        group="apache",
        url="https://github.com/apache/mynewt-nimble",
        path="libs/third_party/nimble",
        provenance=PROV_COMMIT_PINNED,
        description="Bluetooth 5.4 host + controller stack (staged, not linked).",
        purl="pkg:github/apache/mynewt-nimble@8b6f3e819118a1839e5f238bfe1797d64878dc3d",
        spdx="Apache-2.0",
        license_note="Ships its own NOTICE (Apache-2.0 section 4(d)).",
        license_file="libs/third_party/nimble/LICENSE",
        upstream_commit="8b6f3e819118a1839e5f238bfe1797d64878dc3d",
        extra_notes=(
            "Pinned by tree fingerprint: all 859 vendored files (including "
            "the one symlink) are byte-identical to upstream commit 8b6f3e81, "
            "the single exact match among the 5567 commits reachable from the "
            "upstream default branch (42 commits past nimble_1_9_0_tag). The "
            "vendored subset drops upstream apps/ only.",
            "version.yml records repo.version 0.0.0 (upstream default-branch "
            "placeholder); RELEASE_NOTES.md prose still reads 1.9.0.",
            "Second attribution file: libs/third_party/nimble/NOTICE.",
        ),
    ),
    Component(
        key="litehtml",
        name="litehtml",
        version="0.9+git.8836bc1b (2026-01-10 default-branch snapshot)",
        ctype="library",
        group="litehtml",
        url="https://github.com/litehtml/litehtml",
        path="libs/third_party/litehtml",
        provenance=PROV_COMMIT_PINNED,
        description="HTML/CSS layout engine; linked by libs/ra8_reflow (EPUB).",
        purl="pkg:github/litehtml/litehtml@8836bc1bc35ca0cfd71dc0386ef841d5cbc3bd5e",
        spdx="BSD-3-Clause",
        license_file="libs/third_party/litehtml/LICENSE",
        upstream_commit="8836bc1bc35ca0cfd71dc0386ef841d5cbc3bd5e",
        extra_notes=(
            "Pinned by tree fingerprint: all 215 vendored files are "
            "byte-identical to upstream commit 8836bc1b, the single exact "
            "match among the 1040 commits reachable from the upstream "
            "default branch (340 commits past the v0.9 tag). The vendored "
            "subset drops doc/, support/, README.md and the MSVC project "
            "files.",
            "Feeds untrusted EPUB HTML/CSS from a dev-branch snapshot; "
            "prefer a tagged release at the next re-vendor (SOUP-4).",
        ),
    ),
    Component(
        key="miniz",
        name="miniz",
        version="11.0.2",
        ctype="library",
        group="richgel999",
        url="https://github.com/richgel999/miniz",
        path="libs/third_party/miniz",
        provenance=PROV_VERSION_HEADER,
        description="Deflate / inflate / ZIP support for EPUB unpacking.",
        purl="pkg:github/richgel999/miniz@11.0.2",
        spdx="MIT",
        license_note="MIT text (upstream describes it as zlib-style).",
        license_file="libs/third_party/miniz/LICENSE",
        probe_file="miniz.h",
        probe_re=r'MZ_VERSION\s+"([0-9.]+)"',
        expected_version="11.0.2",
        extra_notes=(
            "Untrusted-ZIP decoder built with -w + -fno-strict-aliasing; a "
            "toolchain-version-specific miscompile is documented (T5-02 / "
            "T5-03).",
        ),
    ),
    Component(
        key="xz_embedded",
        name="XZ Embedded (decode-only)",
        version="v2024-12-30 (git.ae63ae3a)",
        ctype="library",
        group="tukaani-project",
        url="https://github.com/tukaani-project/xz-embedded",
        path="libs/third_party/xz_embedded",
        provenance=PROV_COMMIT_PINNED,
        description="XZ/LZMA2 decoder behind libs/ra8_unarch (.tar.xz content).",
        purl="pkg:github/tukaani-project/xz-embedded@ae63ae3a36ed01724674e8f3d750dc47bf125410",
        spdx="0BSD",
        license_note="0BSD (upstream COPYING; SPDX headers per file).",
        license_file="libs/third_party/xz_embedded/COPYING",
        upstream_commit="ae63ae3a36ed01724674e8f3d750dc47bf125410",
        aggregate_sha256=("9dc6c2af6988af773cdf64d383a450f70c31a6df467ec5bcf827901c21c95dd9"),
        extra_notes=(
            "Pinned by tree fingerprint: all 11 vendored files are "
            "byte-identical to upstream tag v2024-12-30 (commit ae63ae3a), "
            "the single exact match among the 169 commits reachable from "
            "the upstream default branch. The vendored subset is the decode "
            "core only (linux/lib/xz + linux/include/linux/xz.h flattened, "
            "plus AUTHORS/COPYING/README); no encoder, no BCJ filters, no "
            "MicroLZMA callers.",
            "Aggregate SHA-256 is over the sorted per-file hashes of the whole vendored directory.",
            "Built decode-only via the first-party porting header "
            "libs/ra8_unarch/inc/xz_config.h: XZ_PREALLOC mode only "
            "(dictionary allocated once from a caller scratch through the "
            "zero-heap pool), CRC32 + CRC64 verification, no XZ_DEC_DYNALLOC "
            "(hostile headers must not size allocations). See "
            "docs/SOUP/xz_embedded.md.",
        ),
    ),
    Component(
        key="stb",
        name="stb (stb_image + stb_truetype)",
        version="stb_image 2.30 / stb_truetype 1.26",
        ctype="library",
        group="nothings",
        url="https://github.com/nothings/stb",
        path="libs/third_party/stb",
        provenance=PROV_VERSION_HEADER,
        description="PNG/JPEG decode (stb_image) + TTF/OTF raster (stb_truetype).",
        purl="pkg:github/nothings/stb",
        spdx="MIT OR Unlicense",
        license_note=(
            "Public-domain dual license; text lives only in the header "
            "tails -- no standalone LICENSE file in the directory (SOUP-5)."
        ),
        license_file=None,
        probe_file="stb_image.h",
        probe_re=r"stb_image - v([0-9.]+)",
        expected_version="2.30",
        extra_notes=(
            "stb_truetype.h version 1.26 (tracked separately; probe covers stb_image only).",
        ),
    ),
    Component(
        key="libwebp",
        name="libwebp (WebP decoder)",
        version="1.5.0",
        ctype="library",
        group="webmproject",
        url="https://chromium.googlesource.com/webm/libwebp",
        path="libs/third_party/libwebp",
        provenance=PROV_COMMIT_PINNED,
        description=(
            "WebP (VP8 / VP8L) decode-only codec for longstrip/manga raster (via ra8_webp)."
        ),
        purl="pkg:github/webmproject/libwebp@v1.5.0",
        spdx="BSD-3-Clause",
        license_note="BSD-3-Clause plus an additional PATENTS grant (both mirrored in-tree).",
        license_file="libs/third_party/libwebp/COPYING",
        upstream_commit="a4d7a715337ded4451fec90ff8ce79728e04126c",
        modified=True,
        extra_notes=(
            "DECODE-ONLY subset (#290): upstream's libwebpdecoder source set "
            "(src/dec + the decode subset of src/dsp + src/utils COMMON) plus the "
            "headers those TUs include; the encoder, mux/demux, sharpyuv and CLI "
            "tools are not vendored. Byte-identical to release tag v1.5.0.",
            "MODIFIED SOUP: src/utils/utils.c carries one RA8 LOCAL PATCH that, "
            "under -DRA8_WEBP_USE_ARENA, routes WebPSafe{Malloc,Calloc,Free} "
            "through the heap-free ra8_webp bump arena (NASA P10 Rule 3). See "
            "docs/SOUP/libwebp.md.",
            "Additional attribution files: libs/third_party/libwebp/PATENTS "
            "(IP-rights grant) and libs/third_party/libwebp/AUTHORS.",
            "Ships v1.5.0 which carries the CVE-2023-4863 VP8L fix.",
        ),
    ),
    Component(
        key="tinyxml2",
        name="TinyXML-2",
        version="11.0.0",
        ctype="library",
        group="leethomason",
        url="https://github.com/leethomason/tinyxml2",
        path="libs/third_party/tinyxml2",
        provenance=PROV_VERSION_HEADER,
        description="XML parser for EPUB container metadata + chapter DOM.",
        purl="pkg:github/leethomason/tinyxml2@11.0.0",
        spdx="Zlib",
        license_file="libs/third_party/tinyxml2/LICENSE.txt",
        modified=True,
        probe_file="tinyxml2.h",
        probe_prefix="TIXML2",
        expected_version="11.0.0",
        extra_notes=(
            "MODIFIED SOUP: in-TU patch #151 generalizes the "
            "XMLDocument::Identify PEDANTIC_WHITESPACE branch for the "
            "on-device .rabook compiler (see docs/SOUP/tinyxml2.md).",
        ),
    ),
    Component(
        key="tflite-micro",
        name="TensorFlow Lite for Microcontrollers",
        version="git fddd3707 (2026 default branch); no upstream release tag",
        ctype="library",
        group="tensorflow",
        url="https://github.com/tensorflow/tflite-micro",
        path="libs/third_party/tflite-micro",
        provenance=PROV_COMMIT_PINNED,
        description=(
            "On-device inference runtime (MicroInterpreter + lean "
            "reference-kernel set) for the RA8P1 Ethos-U55 NPU."
        ),
        purl="pkg:github/tensorflow/tflite-micro@fddd3707a3c5733af4cb866f18650441e6712504",
        spdx="Apache-2.0",
        license_file="libs/third_party/tflite-micro/LICENSE",
        upstream_commit="fddd3707a3c5733af4cb866f18650441e6712504",
        extra_notes=(
            "LEAN subset (#228): MicroInterpreter / MicroAllocator / op-resolver "
            "core + reference kernels CONV_2D, DEPTHWISE_CONV_2D, "
            "FULLY_CONNECTED, ADD, MUL, RESHAPE, SOFTMAX, AVERAGE_POOL_2D + the "
            "Ethos-U custom-op stub. Audio/FFT (signal/, kissfft), the "
            "CMSIS-NN/Xtensa/ARC optimized kernel ports, tests, benchmarks and "
            "examples are omitted.",
            "Build deps FlatBuffers, gemmlowp and ruy are vendored as sibling "
            "libs/third_party components (not nested).",
            "Phase 2 (#228) replaces the Ethos-U op stub with an ra8_npu adapter; "
            "see docs/SOUP/tflite-micro.md.",
        ),
    ),
    Component(
        key="flatbuffers",
        name="FlatBuffers",
        version="25.9.23",
        ctype="library",
        group="google",
        url="https://github.com/google/flatbuffers",
        path="libs/third_party/flatbuffers",
        provenance=PROV_COMMIT_PINNED,
        description="Serialization headers for the .tflite model format read by TFLite-micro.",
        purl="pkg:github/google/flatbuffers@v25.9.23",
        spdx="Apache-2.0",
        license_file="libs/third_party/flatbuffers/LICENSE",
        upstream_commit="edbe17738352418245d7228e7fd9f12c3ddc34c4",
        extra_notes=(
            "Headers only (include/flatbuffers/*.h) -- the read/verify path "
            "TFLite-micro needs; no flatc compiler or codegen vendored.",
            "Version matches the flatbuffers pin in TFLite-micro's "
            "tools/make/flatbuffers_download.sh (v25.9.23).",
        ),
    ),
    Component(
        key="gemmlowp",
        name="gemmlowp (fixed-point headers)",
        version="git 719139ce (2018-09-04); no upstream release tag",
        ctype="library",
        group="google",
        url="https://github.com/google/gemmlowp",
        path="libs/third_party/gemmlowp",
        provenance=PROV_COMMIT_PINNED,
        description=(
            "Fixed-point math headers the quantized TFLite-micro reference kernels depend on."
        ),
        purl="pkg:github/google/gemmlowp@719139ce755a0f31cbf1c37f7f98adcc7fc9f425",
        spdx="Apache-2.0",
        license_file="libs/third_party/gemmlowp/LICENSE",
        upstream_commit="719139ce755a0f31cbf1c37f7f98adcc7fc9f425",
        extra_notes=(
            "Header-only subset: fixedpoint/*.h + internal/detect_platform.h "
            "(the files the reference kernels include).",
            "Commit matches the gemmlowp pin in TFLite-micro's "
            "tools/make/third_party_downloads.inc.",
        ),
    ),
    Component(
        key="ruy",
        name="ruy (profiler instrumentation stub)",
        version="git d3712831 (2021-05-11); no upstream release tag",
        ctype="library",
        group="google",
        url="https://github.com/google/ruy",
        path="libs/third_party/ruy",
        provenance=PROV_COMMIT_PINNED,
        description=(
            "Profiler instrumentation stub header included by TFLite-micro kernel utilities."
        ),
        purl="pkg:github/google/ruy@d37128311b445e758136b8602d1bbd2a755e115d",
        spdx="Apache-2.0",
        license_file="libs/third_party/ruy/LICENSE",
        upstream_commit="d37128311b445e758136b8602d1bbd2a755e115d",
        extra_notes=(
            "Single header vendored: ruy/profiler/instrumentation.h (a no-op "
            "profiler stub); no ruy GEMM backend.",
            "Commit matches the ruy pin in TFLite-micro's tools/make/third_party_downloads.inc.",
        ),
    ),
    Component(
        key="fsp_blobs/r_sce_AMC",
        name="Renesas RSIP-E50D firmware (r_sce_AMC)",
        version="FSP default branch @ 40bbaa11 (2026-05-02); no tag pinned",
        ctype="firmware",
        group="renesas",
        url="https://github.com/renesas/fsp",
        path="libs/third_party/fsp_blobs/r_sce_AMC",
        provenance=PROV_COMMIT_PINNED,
        description="RSIP-E50D protected crypto procedures (key install/wrap).",
        purl="pkg:github/renesas/fsp@40bbaa11b1a1b87e0ee0675e401aea6351f90d14",
        spdx="BSD-3-Clause",
        license_note="Per-file SPDX-BSD-3-Clause; upstream LICENSE.md mirrored.",
        license_file="libs/third_party/fsp_blobs/r_sce_AMC/UPSTREAM_LICENSE.md",
        upstream_commit="40bbaa11b1a1b87e0ee0675e401aea6351f90d14",
        aggregate_sha256=("718e4d454033ce5481e4cd846eb4e585731e1be41cb36dff0e8e214842037064"),
        extra_notes=(
            "Gold-standard provenance: commit pin + aggregate SHA-256 of the "
            "sorted per-file hashes (excludes UPSTREAM_LICENSE.md). See "
            "docs/SOUP/r_sce_AMC_firmware.md.",
        ),
    ),
    Component(
        key="fonts/Literata",
        name="Literata (Literata-Regular.ttf)",
        version="3.103 (TTF name table)",
        ctype="data",
        group="googlefonts",
        url="https://github.com/googlefonts/literata",
        path="libs/fonts/Literata-Regular.ttf",
        provenance=PROV_OPEN_ASSET,
        description="Reading-body serif font, rasterized at runtime by ra8_reflow.",
        purl="pkg:github/googlefonts/literata",
        spdx="OFL-1.1",
        license_file="libs/fonts/Literata-OFL.txt",
        copyright=(
            "Copyright 2017 The Literata Project Authors (https://github.com/googlefonts/literata)."
        ),
        extra_notes=(
            "SIL Open Font License 1.1 -- open and redistributable. Static "
            "Regular instance from the googlefonts/literata upstream; the "
            "shipped OFL.txt is libs/fonts/Literata-OFL.txt.",
            "Also bundled: libs/fonts/literata_latin1.ttf and the baked "
            "libs/fonts/literata_latin1.h (Latin-1 subset). Same provenance.",
            "Replaces the previously bundled proprietary Adobe Arno Pro face "
            "(a redistribution blocker); see THIRD_PARTY_LICENSES.md.",
        ),
    ),
)


def _read_source(comp: Component) -> str | None:
    """Return the text of `comp`'s version-probe file, or None if unreadable."""
    if comp.probe_file is None:
        return None
    path = REPO_ROOT / comp.path / comp.probe_file
    if not path.is_file():
        return None
    return path.read_text(encoding="utf-8", errors="replace")


def probe_version(comp: Component) -> str | None:
    """Re-derive `comp`'s version from its in-tree source, or None.

    Supports two shapes: a single-capture regex (``probe_re``) or a
    MAJOR/MINOR/PATCH macro triplet identified by ``probe_prefix``.
    """
    text = _read_source(comp)
    if text is None:
        return None
    if comp.probe_re is not None:
        match = re.search(comp.probe_re, text)
        return match.group(1) if match else None
    if comp.probe_prefix is not None:
        parts = []
        for level in ("MAJOR", "MINOR", "PATCH"):
            pattern = rf"{comp.probe_prefix}_{level}_VERSION(?:\s+|\s*=\s*)(\d+)"
            match = re.search(pattern, text)
            if match is None:
                return None
            parts.append(match.group(1))
        return ".".join(parts)
    return None


def _third_party_dirs() -> set[str]:
    """Return the direct child directory names under libs/third_party/."""
    base = REPO_ROOT / THIRD_PARTY_DIR
    return {p.name for p in base.iterdir() if p.is_dir()} if base.is_dir() else set()


def _catalogued_top_dirs() -> set[str]:
    """Return the direct-child dir names of libs/third_party/ the registry covers.

    A nested key such as ``fsp_blobs/r_sce_AMC`` catalogues the ``fsp_blobs``
    top-level directory, so a blob tree with sub-components does not read as
    uncatalogued.
    """
    prefix = THIRD_PARTY_DIR.parts
    dirs: set[str] = set()
    for comp in REGISTRY:
        parts = Path(comp.path).parts
        if parts[: len(prefix)] == prefix and len(parts) > len(prefix):
            dirs.add(parts[len(prefix)])
    return dirs


def cross_check() -> tuple[list[str], list[str]]:
    """Cross-check the registry against the tree.

    Returns ``(errors, warnings)``.  Errors are hard failures (a directory
    the registry claims is missing, an uncatalogued directory, or a version
    macro that disagrees with the recorded version).  Warnings are advisory
    (a missing LICENSE file for a component that declares one is an error;
    stb's documented no-LICENSE gap is a warning).
    """
    errors: list[str] = []
    warnings: list[str] = []

    catalogued = _catalogued_top_dirs()
    on_disk = _third_party_dirs()
    errors.extend(
        f"libs/third_party/{extra}: on disk but not in REGISTRY (uncatalogued SOUP)"
        for extra in sorted(on_disk - catalogued)
    )
    errors.extend(
        f"libs/third_party/{missing}: in REGISTRY but not on disk"
        for missing in sorted(catalogued - on_disk)
    )

    for comp in REGISTRY:
        comp_path = REPO_ROOT / comp.path
        if comp.provenance == PROV_NOT_VENDORED:
            if comp_path.exists():
                warnings.append(f"{comp.key}: marked not-vendored but present on disk")
            continue
        if not comp_path.exists():
            errors.append(f"{comp.key}: recorded path '{comp.path}' does not exist")
            continue
        _check_version(comp, errors)
        _check_license_file(comp, errors, warnings)

    return errors, warnings


def _check_version(comp: Component, errors: list[str]) -> None:
    """Append an error if the probed version disagrees with the record."""
    if comp.expected_version is None:
        return
    probed = probe_version(comp)
    if probed is None:
        errors.append(f"{comp.key}: version probe found no version in '{comp.probe_file}'")
    elif probed != comp.expected_version:
        errors.append(
            f"{comp.key}: version drift -- source says {probed}, "
            f"registry says {comp.expected_version}"
        )


def _check_license_file(comp: Component, errors: list[str], warnings: list[str]) -> None:
    """Error on a declared-but-missing LICENSE; warn on the stb gap."""
    if comp.license_file is None:
        if comp.spdx is not None:
            warnings.append(f"{comp.key}: no standalone LICENSE file in-tree (license in headers)")
        return
    if not (REPO_ROOT / comp.license_file).is_file():
        errors.append(f"{comp.key}: declared LICENSE '{comp.license_file}' is missing")


def _licenses_block(comp: Component) -> list[dict] | None:
    """Build the CycloneDX ``licenses`` array for a component."""
    if comp.spdx is not None:
        if " OR " in comp.spdx or " AND " in comp.spdx:
            return [{"expression": comp.spdx}]
        return [{"license": {"id": comp.spdx}}]
    if comp.license_name is not None:
        return [{"license": {"name": comp.license_name}}]
    return None


def _properties_block(comp: Component) -> list[dict]:
    """Build the CycloneDX ``properties`` array for a component."""
    props: list[dict] = [
        {"name": "ra8:provenance", "value": comp.provenance},
        {"name": "ra8:path", "value": comp.path},
    ]
    if comp.upstream_commit is not None:
        props.append({"name": "ra8:upstreamCommit", "value": comp.upstream_commit})
    if comp.license_original is not None:
        props.append({"name": "ra8:licenseOriginal", "value": comp.license_original})
    if comp.license_election is not None:
        props.append({"name": "ra8:licenseElection", "value": comp.license_election})
    if comp.license_file is not None:
        props.append({"name": "ra8:licenseFile", "value": comp.license_file})
    if comp.copyright is not None:
        props.append({"name": "ra8:copyright", "value": comp.copyright})
    props.append({"name": "ra8:modified", "value": "true" if comp.modified else "false"})
    for i, note in enumerate(comp.extra_notes):
        props.append({"name": f"ra8:note{i}", "value": note})
    return props


def component_entry(comp: Component) -> dict:
    """Render one registry `Component` as a CycloneDX component object."""
    entry: dict = {"type": comp.ctype, "bom-ref": comp.key, "name": comp.name}
    if comp.group is not None:
        entry["group"] = comp.group
    entry["version"] = comp.version
    entry["description"] = comp.description
    entry["scope"] = comp.scope
    licenses = _licenses_block(comp)
    if licenses is not None:
        entry["licenses"] = licenses
    if comp.license_note is not None:
        entry["copyright"] = comp.license_note if comp.copyright is None else comp.copyright
    if comp.purl is not None:
        entry["purl"] = comp.purl
    if comp.aggregate_sha256 is not None:
        entry["hashes"] = [{"alg": "SHA-256", "content": comp.aggregate_sha256}]
    entry["externalReferences"] = [{"type": "vcs", "url": comp.url}]
    entry["properties"] = _properties_block(comp)
    return entry


def _serial_number() -> str:
    """Return a content-derived (deterministic) CycloneDX serial number."""
    canonical = "|".join(f"{c.key}={c.version}={c.spdx or c.license_name}" for c in REGISTRY)
    return f"urn:uuid:{uuid.uuid5(uuid.NAMESPACE_URL, canonical)}"


def build_bom() -> dict:
    """Assemble the full CycloneDX 1.5 BOM document as an ordered dict."""
    return {
        "bomFormat": BOM_FORMAT,
        "specVersion": CYCLONEDX_SPEC,
        "serialNumber": _serial_number(),
        "version": BOM_REVISION,
        "metadata": {
            "tools": [{"vendor": PROJECT_NAME, "name": GENERATOR_NAME}],
            "component": {
                "type": "application",
                "bom-ref": PROJECT_NAME,
                "name": PROJECT_NAME,
                "version": "unversioned",
                "description": "Renesas RA8D2 (Cortex-M85) bare-metal firmware.",
            },
            "properties": [
                {
                    "name": "ra8:sbomNote",
                    "value": (
                        "Generated by scripts/gen/gen_sbom.py from the "
                        "REGISTRY cross-checked against libs/third_party/. "
                        "Human inventory: THIRD_PARTY_LICENSES.md. Per-"
                        "component qualification: docs/SOUP/."
                    ),
                },
            ],
        },
        "components": [component_entry(c) for c in REGISTRY],
    }


def serialize(bom: dict) -> str:
    """Serialize the BOM deterministically (ASCII, 2-space indent, newline)."""
    return json.dumps(bom, indent=2, ensure_ascii=True) + "\n"


def _print_summary(warnings: list[str], stream: TextIO) -> None:
    """Print a one-line-per-class provenance summary to `stream`."""
    by_prov: dict[str, int] = {}
    for comp in REGISTRY:
        by_prov[comp.provenance] = by_prov.get(comp.provenance, 0) + 1
    print(f"{GENERATOR_NAME}: {len(REGISTRY)} components", file=stream)
    for prov in sorted(by_prov):
        print(f"  {prov:24s} {by_prov[prov]}", file=stream)
    for warn in warnings:
        print(f"  WARN {warn}", file=stream)


def run_write(to_stdout: bool) -> int:
    """Regenerate the SBOM; write it (or print it) and report cross-checks."""
    errors, warnings = cross_check()
    text = serialize(build_bom())
    # In --print mode stdout must stay pure JSON, so the summary goes to stderr.
    summary_stream = sys.stderr if to_stdout else sys.stdout
    if to_stdout:
        sys.stdout.write(text)
    else:
        out = REPO_ROOT / SBOM_REL_PATH
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8")
        print(f"{GENERATOR_NAME}: wrote {SBOM_REL_PATH}")
    _print_summary(warnings, summary_stream)
    if errors:
        for err in errors:
            print(f"  ERROR {err}", file=sys.stderr)
        return EXIT_DRIFT
    return EXIT_OK


def run_check() -> int:
    """Fail if the committed SBOM is stale or the tree drifted from the registry."""
    errors, warnings = cross_check()
    expected = serialize(build_bom())
    out = REPO_ROOT / SBOM_REL_PATH
    if not out.is_file():
        print(
            f"{GENERATOR_NAME}: {SBOM_REL_PATH} is missing; run gen_sbom.py",
            file=sys.stderr,
        )
        return EXIT_DRIFT
    actual = out.read_text(encoding="utf-8")
    if actual != expected:
        print(
            f"{GENERATOR_NAME}: {SBOM_REL_PATH} is stale; run gen_sbom.py to regenerate",
            file=sys.stderr,
        )
        errors = [*errors, "committed SBOM does not match the registry"]
    for warn in warnings:
        print(f"  WARN {warn}")
    if errors:
        for err in errors:
            print(f"  ERROR {err}", file=sys.stderr)
        return EXIT_DRIFT
    print(f"{GENERATOR_NAME}: SBOM matches the tree ({len(REGISTRY)} components).")
    return EXIT_OK


def run_commits() -> int:
    """Print one ``<key> <upstream-commit>`` line per commit-pinned component.

    This is the machine interface behind the weekly OSV CVE scan
    (``scripts/checks/osv_scan.sh``): OSV.dev indexes C/C++ advisories as GIT
    commit ranges queryable only by commit hash (GitHub purls do not
    resolve), so the scan materializes each pinned commit as a stub git
    checkout and lets ``osv-scanner`` issue the exact commit queries.
    Exits nonzero when the registry carries no pins at all, which would
    mean the scan is wired to nothing.
    """
    pinned = [comp for comp in REGISTRY if comp.upstream_commit is not None]
    for comp in pinned:
        print(f"{comp.key} {comp.upstream_commit}")
    if not pinned:
        print(f"{GENERATOR_NAME}: no commit-pinned component in REGISTRY", file=sys.stderr)
        return EXIT_DRIFT
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Parse arguments and dispatch to the write / check / print / commits action."""
    parser = argparse.ArgumentParser(description="Generate/validate the ra8-firmware SBOM.")
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if the committed SBOM is stale or the tree drifted",
    )
    parser.add_argument(
        "--print",
        dest="to_stdout",
        action="store_true",
        help="print the SBOM to stdout instead of writing the file",
    )
    parser.add_argument(
        "--commits",
        action="store_true",
        help="print `<key> <upstream-commit>` per commit-pinned component",
    )
    args = parser.parse_args(argv)
    if args.check:
        return run_check()
    if args.commits:
        return run_commits()
    return run_write(args.to_stdout)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
