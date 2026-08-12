# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The curated third-party component registry behind the ra8-firmware SBOM.

This module holds the *data*: what each vendored SOUP component under
``libs/third_party/`` (plus the bundled font asset and the one not-vendored
co-processor firmware) is, where it came from, and under what license.
``gen_sbom.py`` holds the *logic* that renders and cross-checks it.

They are separate modules so the registry can grow with the tree without
dragging the generator over the project file-size cap, and so a component
edit reads as a data change in review rather than a change to the gate.

Editing rules:

  * Versions are cross-checked against the tree where a header macro exists;
    do NOT edit a version here without the matching vendored source changing.
  * Per-component qualification lives under ``docs/SOUP/``; the aggregated
    human license inventory is ``THIRD_PARTY_LICENSES.md``.  A registry edit
    that does not update those is half a change.
  * A newly vendored directory with no entry here fails the SBOM gate.
  * There is NO integrity-hash field, deliberately.  ``aggregate_sha256`` used
    to live here as a hand-transcribed literal on four of the twenty-three
    entries, which meant ``gen_sbom.py --check`` compared a constant against
    itself and a mutated vendored byte reported clean (#538).  The digest is
    now DERIVED from the tree on every run by ``gen_sbom.tree_digest()``.  Do
    not re-introduce a stored copy: a transcribed value never disagrees with
    itself.
  * The ``upstream_*`` / ``patched_files`` / ``local_files`` fields are the
    machine-readable form of what ``docs/SOUP/*.md`` states in prose: which
    upstream ref the tree claims to be, and every file that deliberately
    departs from it.  ``scripts/checks/check_soup_upstream.py`` compares the
    tree against upstream blob hashes fetched from the upstream project, so a
    subset rule or a patch that lives only in prose is not enough -- an
    undeclared deviation fails the gate (#548).
"""

from __future__ import annotations

from dataclasses import dataclass, field

# Provenance classes, most-to-least trustworthy.  Recorded per component in a
# CycloneDX property so an auditor can see, at a glance, which components are
# pinned versus inferred.
PROV_COMMIT_PINNED = "commit-pinned-sha256"  # upstream commit + per-file upstream blob hashes
PROV_ARCHIVE_PINNED = "archive-pinned-sha256"  # upstream release artifact pinned by SHA-256
PROV_VERSION_HEADER = "version-header"  # version from an in-tree header macro only
PROV_NOT_VENDORED = "not-vendored"  # documented but absent from the tree
PROV_PROPRIETARY = "proprietary-unresolved"  # license not cleared; see notes
PROV_OPEN_ASSET = "open-asset-versioned"  # cleared open asset (OFL font); version from name table

# How the pinned upstream revision is fetched by check_soup_upstream.py.
# GIT is a ref (tag or commit) in the upstream repository; ARCHIVE is a release
# artifact that never existed in the upstream git tree -- miniz publishes its
# single-file amalgamation only as a release zip, so a git ref cannot express
# what we actually vendored.
UPSTREAM_GIT = "git"
UPSTREAM_ARCHIVE = "archive"

# The five Eclipse ThreadX trees share one deliberate deviation, so its
# justification is written once.  Repeating a justification string is how five
# copies of it drift into four different claims.
GITATTRIBUTES_PATCH = (
    "Attribute-macro blocks ([attr]our-c-style, [attr]generated) removed: git "
    "honours [attr] definitions only in the top-level .gitattributes and printed "
    "a 'not allowed' warning on every git operation across five vendored trees. "
    "The macro USES left behind reference undefined attributes, which git ignores "
    "silently, so no vendored file's checkout behaviour changes."
)


@dataclass(frozen=True)
class Component:
    """One SOUP or bundled-asset record.

    The required fields describe the component; the optional fields carry the
    provenance and license detail that the SBOM and the cross-checks consume.

    The ``upstream_*`` group answers "what is this tree supposed to BE?", which
    the version and integrity fields cannot: a digest re-derived from our own
    tree proves only that we have not changed it since we last looked
    (#538/#548).  ``upstream_ref`` names the revision the vendored subset is
    claimed to come from; ``patched_files`` and ``local_files`` enumerate every
    file that deliberately departs from it, so "modified" and "corrupted" are
    distinguishable by a machine rather than by reading prose.
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
    upstream_transport: str = UPSTREAM_GIT  # UPSTREAM_GIT or UPSTREAM_ARCHIVE
    upstream_repo: str | None = None  # clonable URL, when it differs from `url`
    upstream_ref: str | None = None  # tag/branch/commit to fetch; None -> upstream_commit
    upstream_archive_url: str | None = None  # release artifact URL (UPSTREAM_ARCHIVE only)
    upstream_archive_sha256: str | None = None  # SHA-256 of that artifact
    upstream_archive_prefix: str = ""  # path prefix inside the archive to strip
    nested_paths: tuple[str, ...] = field(  # repo-relative sub-components to exclude
        default_factory=tuple
    )
    patched_files: tuple[tuple[str, str], ...] = field(  # (component-rel path, why)
        default_factory=tuple
    )
    local_files: tuple[tuple[str, str], ...] = field(  # (component-rel path, why) no upstream
        default_factory=tuple
    )
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
        provenance=PROV_COMMIT_PINNED,
        description="Preemptive RTOS kernel: 45 example apps, vendored middleware, NS image.",
        purl="pkg:github/eclipse-threadx/threadx@6.5.0",
        upstream_commit="3726d7906b4808bfec7855fc088e073199df9120",
        upstream_ref="v6.5.0.202601_rel",
        modified=True,
        patched_files=((".gitattributes", GITATTRIBUTES_PATCH),),
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
        provenance=PROV_COMMIT_PINNED,
        description="Dual IPv4/IPv6 TCP/IP stack (wired eth + C6 Wi-Fi); NetX Secure not compiled.",
        purl="pkg:github/eclipse-threadx/netxduo@6.5.0",
        upstream_commit="8b6e03ac30ab688bec02c69d42f2304b7f72a202",
        upstream_ref="v6.5.0.202601_rel",
        modified=True,
        patched_files=((".gitattributes", GITATTRIBUTES_PATCH),),
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
        key="usbx",
        name="Eclipse USBX",
        version="6.5.0",
        ctype="library",
        group="eclipse-threadx",
        url="https://github.com/eclipse-threadx/usbx",
        path="libs/third_party/usbx",
        provenance=PROV_COMMIT_PINNED,
        description="USB host / device stack (CDC, HID, MSC demos).",
        purl="pkg:github/eclipse-threadx/usbx@6.5.0",
        upstream_commit="6dc0cf233d5b7ee6e1a7434581964975f8d8d37b",
        upstream_ref="v6.5.0.202601_rel",
        modified=True,
        patched_files=((".gitattributes", GITATTRIBUTES_PATCH),),
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
        provenance=PROV_COMMIT_PINNED,
        description="NOR-flash wear-levelling on Octo-SPI: under ra8_fs, and standalone.",
        purl="pkg:github/eclipse-threadx/levelx@6.5.0",
        upstream_commit="a46b74fb8aa133796ccbc13e7902cb8bb818e12f",
        upstream_ref="v6.5.0.202601_rel",
        modified=True,
        patched_files=((".gitattributes", GITATTRIBUTES_PATCH),),
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
        provenance=PROV_COMMIT_PINNED,
        description="TLS record layer + X.509 via ra8_tls; two demo apps, none on hardware.",
        purl="pkg:github/Mbed-TLS/mbedtls@4.1.0",
        upstream_commit="d12fbb991c0822f347bbc569badef904629ce605",
        modified=True,
        patched_files=(
            (
                "library/.gitignore",
                "Upstream ignores library/mbedtls_config_check_*.h as build "
                "output; we vendor those three generated headers because the "
                "firmware build never runs upstream's generator, so the ignore "
                "block is dropped.",
            ),
        ),
        local_files=(
            (
                "library/mbedtls_config_check_before.h",
                "Generated by upstream's own generate_config_tests.py at "
                "configure time; vendored because the cross build does not run it.",
            ),
            (
                "library/mbedtls_config_check_final.h",
                "Generated config-check header, vendored for the same reason.",
            ),
            (
                "library/mbedtls_config_check_user.h",
                "Generated config-check header, vendored for the same reason.",
            ),
        ),
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
        provenance=PROV_COMMIT_PINNED,
        description="PSA Crypto API; RoT secure-boot ECDSA-P256 verify engine.",
        purl="pkg:github/Mbed-TLS/TF-PSA-Crypto@1.1.0",
        upstream_commit="bbf1eaf5f4a72bcc3e0cfe854e0313c93b75cd77",
        local_files=(
            (
                "core/psa_crypto_driver_wrappers.h",
                "Generated from the driver JSON by upstream's "
                "generate_driver_wrappers.py at configure time; vendored because "
                "the cross build does not run it.",
            ),
            (
                "core/psa_crypto_driver_wrappers_no_static.c",
                "Generated driver-wrapper TU, vendored for the same reason.",
            ),
            (
                "core/tf_psa_crypto_config_check_before.h",
                "Generated config-check header, vendored for the same reason.",
            ),
            (
                "core/tf_psa_crypto_config_check_final.h",
                "Generated config-check header, vendored for the same reason.",
            ),
            (
                "core/tf_psa_crypto_config_check_user.h",
                "Generated config-check header, vendored for the same reason.",
            ),
        ),
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
        version="1.10.0 (nimble_1_10_0_tag, 2026-07-10)",
        ctype="library",
        group="apache",
        url="https://github.com/apache/mynewt-nimble",
        path="libs/third_party/nimble",
        provenance=PROV_COMMIT_PINNED,
        description="Bluetooth 5.4 host + controller stack (staged, not linked).",
        purl="pkg:github/apache/mynewt-nimble@a7a156f28954819e158b62dd613008f22f9cf73b",
        spdx="Apache-2.0",
        license_note="Ships its own NOTICE (Apache-2.0 section 4(d)).",
        license_file="libs/third_party/nimble/LICENSE",
        upstream_commit="a7a156f28954819e158b62dd613008f22f9cf73b",
        upstream_ref="nimble_1_10_0_tag",
        extra_notes=(
            "Pinned to a release tag, not a default-branch snapshot: all 827 "
            "vendored files (826 regular plus the one symlink) are "
            "byte-identical to nimble_1_10_0_tag == commit a7a156f2. The "
            "vendored subset drops upstream apps/ (163 files) only.",
            "version.yml records repo.version 0.0.0 (upstream keeps that "
            "placeholder on its default branch, which the release tag points "
            "at); the 1.10.0 identity comes from the tag and "
            "RELEASE_NOTES.md.",
            "Bumped from 1.9.0+git.8b6f3e81, which OSV resolved into "
            "CVE-2026-45811 / -45815 / -45816 / -46452. All four are fixed in "
            "1.10.0 and OSV resolves this commit clean (#508). Moving to a "
            "tagged release also closes the NimBLE half of SOUP-4.",
            "1.10.0 removes the bundled ext/tinycrypt sub-component, dropping "
            "its BSD-2-Clause / BSD-3-Clause text from our redistribution "
            "surface.",
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
        upstream_ref="8836bc1bc35ca0cfd71dc0386ef841d5cbc3bd5e",
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
        provenance=PROV_ARCHIVE_PINNED,
        description="Deflate / inflate / ZIP behind EPUB, CBZ, PNG, gzip and the ra8_io seam.",
        purl="pkg:github/richgel999/miniz@11.0.2",
        upstream_transport=UPSTREAM_ARCHIVE,
        upstream_ref="3.0.2",
        upstream_archive_url=(
            "https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip"
        ),
        upstream_archive_sha256=(
            "ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5"
        ),
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
        upstream_ref="v2024-12-30",
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
        provenance=PROV_COMMIT_PINNED,
        description="JPEG/PNG/GIF/BMP decode (stb_image) + TTF/OTF raster (stb_truetype).",
        purl="pkg:github/nothings/stb",
        upstream_commit="31c1ad37456438565541f4919958214b6e762fb4",
        upstream_ref="31c1ad37456438565541f4919958214b6e762fb4",
        modified=True,
        patched_files=(
            (
                "stb_truetype.h",
                "Bounds-checked font parsing: every glyph/cmap/loca read is "
                "range-checked against the buffer length, plus a NULL-scanline "
                "guard on arena exhaustion; see docs/SOUP/stb.md.",
            ),
        ),
        local_files=(
            (
                "stb_image_impl.c",
                "First-party single TU that defines STB_IMAGE_IMPLEMENTATION "
                "with the project's build knobs; not an upstream file.",
            ),
            (
                "stb_truetype_impl.c",
                "First-party single TU that defines STB_TRUETYPE_IMPLEMENTATION; "
                "not an upstream file.",
            ),
        ),
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
        upstream_ref="v1.5.0",
        patched_files=(
            (
                "src/utils/utils.c",
                "RA8 LOCAL PATCH: under -DRA8_WEBP_USE_ARENA, "
                "WebPSafe{Malloc,Calloc,Free} route through the heap-free "
                "ra8_webp bump arena (NASA P10 Rule 3); see docs/SOUP/libwebp.md.",
            ),
        ),
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
        provenance=PROV_COMMIT_PINNED,
        description="XML parser for EPUB container metadata + chapter DOM.",
        purl="pkg:github/leethomason/tinyxml2@11.0.0",
        upstream_commit="9148bdf719e997d1f474be6bcc7943881046dba1",
        upstream_ref="11.0.0",
        patched_files=(
            (
                "tinyxml2.cpp",
                "RA8 LOCAL PATCH (#151): XMLDocument::Identify emits a text node "
                "for whitespace skipped before ANY element in PEDANTIC mode; see "
                "docs/SOUP/tinyxml2.md.",
            ),
        ),
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
        upstream_ref="fddd3707a3c5733af4cb866f18650441e6712504",
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
        upstream_ref="v25.9.23",
        spdx="Apache-2.0",
        license_file="libs/third_party/flatbuffers/LICENSE",
        upstream_commit="187240970746d00bbd26b0f5873ed54d2477f9f3",
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
        upstream_ref="719139ce755a0f31cbf1c37f7f98adcc7fc9f425",
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
        upstream_ref="d37128311b445e758136b8602d1bbd2a755e115d",
        extra_notes=(
            "Single header vendored: ruy/profiler/instrumentation.h (a no-op "
            "profiler stub); no ruy GEMM backend.",
            "Commit matches the ruy pin in TFLite-micro's tools/make/third_party_downloads.inc.",
        ),
    ),
    Component(
        key="esp-hosted",
        name="Espressif esp-hosted-mcu (host driver)",
        version="2.12.11 (host driver) @ git 949bb30",
        ctype="library",
        group="espressif",
        url="https://github.com/espressif/esp-hosted-mcu",
        path="libs/third_party/esp-hosted",
        provenance=PROV_COMMIT_PINNED,
        description=(
            "esp-hosted host driver + shared protocol for the ESP32-C6 Wi-Fi/BLE co-processor."
        ),
        purl="pkg:github/espressif/esp-hosted-mcu@949bb30612747a3bd9e402eda8d01fbfa1f8503e",
        spdx="Apache-2.0",
        license_note="Apache-2.0 (upstream LICENSE); no separate NOTICE file upstream.",
        nested_paths=("libs/third_party/esp-hosted/common/protobuf-c",),
        license_file="libs/third_party/esp-hosted/LICENSE",
        upstream_commit="949bb30612747a3bd9e402eda8d01fbfa1f8503e",
        extra_notes=(
            "HOST half of esp-hosted: this is the driver compiled INTO the RA8 "
            "image. The peripheral-side co-processor firmware that runs on the "
            "ESP32-C6 is the separate, not-vendored esp-hosted-mcu entry. Both "
            "halves are the same upstream commit 949bb30 and the same protocol "
            "version 2.12.11, which is what makes them wire-compatible.",
            "Vendored subset: upstream host/ (minus host/port/) and common/ "
            "(minus esp_hosted_lwip_src_port_hook.h). 77 files, all "
            "byte-identical to upstream 949bb30; aggregate SHA-256 is over the "
            "sorted per-file hashes of those 77 files (it excludes the "
            "separately-pinned nested protobuf-c subtree).",
            "host/port/ (the upstream ESP-IDF/FreeRTOS port) is deliberately "
            "NOT vendored: a first-party RA8/ThreadX port supplies the same 10 "
            "port_esp_hosted_host_*.h header contracts and fills the 72-entry "
            "hosted_osi_funcs_t vtable. See docs/SOUP/esp-hosted-host.md.",
            "Compiled: cmake/esp_hosted.cmake builds 8 of the vendored TUs into "
            "esp_hosted_objs behind RA8_USE_ESP_HOSTED, consumed by five "
            "applications under examples/ek_ra8d2/hw_validated/c6/. The "
            "first-party port (port/esp-hosted/) and driver (libs/ra8_c6link/) "
            "have landed and the protocol round-trip is proven on silicon.",
        ),
    ),
    Component(
        key="esp-hosted/protobuf-c",
        name="protobuf-c (runtime library)",
        version="1.4.1 (git abc67a11)",
        ctype="library",
        group="protobuf-c",
        url="https://github.com/protobuf-c/protobuf-c",
        path="libs/third_party/esp-hosted/common/protobuf-c",
        provenance=PROV_COMMIT_PINNED,
        description="Protocol Buffers C runtime backing the esp-hosted RPC codec.",
        purl="pkg:github/protobuf-c/protobuf-c@abc67a11c6db271bedbb9f58be85d6f4e2ea8389",
        spdx="BSD-2-Clause",
        license_note="BSD-2-Clause (upstream LICENSE); distinct upstream from esp-hosted.",
        license_file="libs/third_party/esp-hosted/common/protobuf-c/LICENSE",
        upstream_commit="abc67a11c6db271bedbb9f58be85d6f4e2ea8389",
        upstream_ref="abc67a11c6db271bedbb9f58be85d6f4e2ea8389",
        probe_file="protobuf-c/protobuf-c.h",
        probe_re=r"#\s*define\s+PROTOBUF_C_VERSION\s+\"([0-9.]+)\"",
        expected_version="1.4.1",
        extra_notes=(
            "Nested component: upstream esp-hosted-mcu carries protobuf-c as a "
            "git SUBMODULE at common/protobuf-c, pinned to abc67a11. A submodule "
            "is a separate upstream project under a separate license, so it gets "
            "its own registry entry and its own OSV commit query rather than "
            "hiding inside the esp-hosted aggregate hash.",
            "Runtime-only subset: protobuf-c/protobuf-c.c, protobuf-c/protobuf-c.h "
            "and LICENSE (3 files, byte-identical to abc67a11). The protoc-c code "
            "generator, build system and tests are a host-side C++ toolchain and "
            "are not vendored; the generated esp_hosted_rpc.pb-c.c ships "
            "pre-generated in the esp-hosted tree.",
            "Version is doubly evidenced: commit pin + aggregate SHA-256 + an "
            "in-header PROTOBUF_C_VERSION probe cross-checked by this generator.",
        ),
    ),
    Component(
        key="esp-hosted-mcu",
        name="Espressif esp-hosted-mcu (network_adapter co-processor firmware)",
        version="FW 2.12.11 (network_adapter) @ git 949bb30, esp-idf v5.5.4",
        ctype="firmware",
        group="espressif",
        url="https://github.com/espressif/esp-hosted-mcu",
        path="coprocessor/esp32c6/esp-hosted-mcu",
        provenance=PROV_NOT_VENDORED,
        description=(
            "ESP32-C6 wireless co-processor firmware; runs on the C6, not in the RA8 image."
        ),
        purl="pkg:github/espressif/esp-hosted-mcu@949bb30612747a3bd9e402eda8d01fbfa1f8503e",
        spdx="Apache-2.0",
        license_note=(
            "Built from source at flash time onto the ESP32-C6; "
            "not linked into the RA8 firmware binary."
        ),
        upstream_commit="949bb30612747a3bd9e402eda8d01fbfa1f8503e",
        scope="excluded",
        extra_notes=(
            "NOT vendored: coprocessor/esp32c6/build.sh fetches the pinned upstream "
            "commit at build time into the git-ignored coprocessor/esp32c6/esp-hosted-mcu/. "
            "The build recipe (pins + proven sdkconfig.defaults) is the record; "
            "see coprocessor/esp32c6/README.md and docs/SOUP/esp-hosted.md.",
            "Co-processor firmware: it runs on the ESP32-C6 (Wi-Fi/BLE), not on "
            "the RA8D2, and is not part of the RA8 linked image -- hence "
            "scope=excluded. Zero first-party code runs on the C6.",
            "esp-idf toolchain pinned at v5.5.4; firmware version 2.12.11.",
        ),
    ),
    Component(
        key="fonts/Literata",
        name="Literata (Literata-Regular.ttf)",
        version="3.103 (TTF name table)",
        ctype="data",
        group="googlefonts",
        url="https://github.com/googlefonts/literata",
        path="libs/ra8_fonts/Literata-Regular.ttf",
        provenance=PROV_OPEN_ASSET,
        description="Reading-body serif font, rasterized at runtime by ra8_reflow.",
        purl="pkg:github/googlefonts/literata",
        upstream_commit="0c2761b727a1b3a7cffd313c37f0f5163dfc7a63",
        upstream_ref="3.103",
        spdx="OFL-1.1",
        license_file="libs/ra8_fonts/Literata-OFL.txt",
        copyright=(
            "Copyright 2017 The Literata Project Authors (https://github.com/googlefonts/literata)."
        ),
        extra_notes=(
            "SIL Open Font License 1.1 -- open and redistributable. Static "
            "Regular instance from the googlefonts/literata upstream; the "
            "shipped OFL.txt is libs/ra8_fonts/Literata-OFL.txt.",
            "Also bundled: libs/ra8_fonts/literata_latin1.ttf and the baked "
            "libs/ra8_fonts/literata_latin1.h (Latin-1 subset). Same provenance.",
            "Replaces the previously bundled proprietary Adobe Arno Pro face "
            "(a redistribution blocker); see THIRD_PARTY_LICENSES.md.",
        ),
    ),
)
