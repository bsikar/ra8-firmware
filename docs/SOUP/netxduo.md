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

- Dual IPv4/IPv6 TCP/IP stack. **The TCP/IP core only** -- see "What is not
  built" below. Two driver bindings carry it:
  - Wired, over the on-chip Ethernet MAC:
    `port/netxduo/src/nx_ether_driver_ra8_eth.c`, used by
    `examples/ek_ra8d2/hw_validated/hil/threadx_netx_tcp_echo` and by the two
    TLS apps (`examples/ek_ra8d2/hw_pending/tls_client`,
    `examples/_unsupported/threadx_https_client`), which get their TLS from
    Mbed TLS, not from this component.
  - Wireless, over the ESP32-C6 co-processor:
    `port/netxduo/src/nx_ether_driver_c6.c` (the `netxduo_port_c6` target),
    used by the two hw_validated Wi-Fi apps
    `examples/ek_ra8d2/hw_validated/c6/{c6_wifi_join,wifi_hal_join}`, which
    additionally compile the vendored DHCP client
    (`addons/dhcp/nxd_dhcp_client.c`) to take an address on the bench network.
- Integrity claim category: data-handling (frame parsing).

### What is not built

- **NetX Secure is compiled by nothing.** `cmake/netxduo.cmake` adds
  `nx_secure/inc` to the include path and nothing else; it deliberately
  excludes the one crypto TU that includes `nx_secure_tls.h`. All 314
  `nx_secure/` files are outside the build graph. TLS in this firmware is Mbed
  TLS behind `libs/ra8_tls/`; no NetX TLS record layer exists in any image, so
  this qualification makes no TLS claim.
- The OTA download path this document used to cite is gone: `threadx_ota_demo`
  was deleted in `d38587e80`, and `libs/ra8_ota` has never referenced NetX --
  it takes a dependency-injected interface.

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

- The SOUP boundary is a single driver shim per link, and both shims are
  first-party: `port/netxduo/src/nx_ether_driver_ra8_eth.c` calls the
  `ra8_eth_*` HAL directly, and `nx_ether_driver_c6.c` bridges onto
  `libs/ra8_c6link/`. (Neither goes through `libs/ra8_net_pal/`, whose
  consumers are `ra8_nsc_eth` and the host tests.)
- No safety-critical control loop runs over the network, and nothing in the
  product image depends on the link: the consumers are bench and bring-up
  applications.

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
- Use case + risk mitigation re-verified against the tree and corrected
  (#621): 2026-08-04. The document claimed a NetX Secure TLS role that no
  build has, cited an application deleted in `d38587e80`, put the driver
  boundary in a library the drivers do not call, and omitted the Wi-Fi
  consumers entirely.
- Expected re-review by: 2027-05-02
