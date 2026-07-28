# SOUP Justification: Eclipse NetX Duo

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting Eclipse NetX Duo into this
firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: Eclipse NetX Duo (formerly Azure RTOS NetX Duo)
- **Version**: 6.5.0 (per `common/inc/nx_api.h` NETXDUO_MAJOR / MINOR /
  PATCH macros). `CHANGELOG.md` documents 6.4.3 history including
  CVE-2025-2258 / 2259 / 2260 fixes.
- **Upstream URL**: https://github.com/eclipse-threadx/netxduo
- **Local path**: `libs/third_party/netxduo/`

## Provenance

- **Origin**: Eclipse Foundation, Eclipse ThreadX top-level project
  (donated by Microsoft from Azure RTOS in 2024).
- **License**: MIT (`LICENSE.txt`, "Copyright (c) 2024 - present Microsoft
  Corporation").
- **How it entered our tree**: Vendored snapshot of the upstream Eclipse
  NetX Duo repository. Resolved (#548) to release tag
  `v6.5.0.202601_rel`, commit `8b6e03ac30ab688bec02c69d42f2304b7f72a202`:
  1226 of the 1227 vendored files are byte-identical to it, the exception
  being the `.gitattributes` edit recorded under "Deviations / patches".

## Use case in this firmware

- Dual IPv4/IPv6 TCP/IP stack, plus NetX Secure (TLS), used by
  `examples/ek_ra8d2/threadx_netx_tcp_echo` and the OTA demo's update
  download path (`threadx_ota_demo` via `libs/ra8_ota`).
- Integrity claim category: data-handling (frame parsing, TLS record
  layer).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: Express Logic / Microsoft NetX Duo has shipped
  alongside ThreadX since the early 2000s in industrial and IoT
  deployments.
- **Open-source community process**: Eclipse Foundation governance,
  active CVE response (3 CVEs were fixed and shipped within the 6.4.3
  cycle).
- **Bug tracker review**: Issues at
  https://github.com/eclipse-threadx/netxduo/issues and the Eclipse
  ThreadX GitHub Security Advisory page reviewed; CVE-2025-2258,
  CVE-2025-2259, CVE-2025-2260 are fixed in the in-tree version line
  (6.4.3 and later).
- **Vendor qualification data**: Pre-Eclipse, NetX Duo carried
  pre-certifications under SGS-TUV Saar for IEC 61508, IEC 62304, ISO
  26262, and EN 50128; cited for context only.

## Risk mitigation

- All board-specific Ethernet/PHY plumbing is isolated in
  `libs/ra8_net_pal/` (NetX Duo PAL) so the SOUP boundary is a single
  driver shim.
- TLS record handling is wrapped behind `libs/ra8_tls/` for use-case
  policy enforcement.
- Demo-only use in this revision; no safety-critical control loop runs
  over the network.

## Deviations / patches

One file, `.gitattributes`, and it is a repository-hygiene edit rather than a
change to any shipped source. Commit `368072a1a` dropped its two `[attr]`
attribute-macro blocks (`our-c-style`, `generated`) from all five vendored
Eclipse ThreadX trees: git honours `[attr]` definitions only in the top-level
`.gitattributes` and printed a "not allowed" warning for each on EVERY git
operation. The macro *uses* left behind reference undefined attributes, which
git ignores silently, so no vendored file's checkout behaviour changes.

Declared in `scripts/gen/sbom_registry.py` as `patched_files` and pinned by
content in `docs/sbom/upstream/netxduo.manifest`; every other file in this
component is verified byte-identical to the upstream pin on each CI run
(#548).

The edit is from 2026-07-13 and went unrecorded here until #548 found it two
weeks later, which is the point: "the vendored tree is unmodified" was prose,
and prose does not notice a tree-wide sweep reaching into `libs/third_party/`.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
