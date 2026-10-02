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
- **Local path**: none. Pinned in `build.zig.zon` as the upstream tarball
  (url + Zig content hash) and fetched into the Zig package cache by
  `cmake/zig_package.cmake`.

## Provenance

- **Origin**: Eclipse Foundation, Eclipse ThreadX top-level project
  (donated by Microsoft from Azure RTOS in 2024).
- **License**: MIT (`LICENSE.txt`, "Copyright (c) 2024 - present Microsoft
  Corporation").
- **How it enters the build**: the upstream Eclipse NetX Duo tarball at
  release tag `v6.5.0.202601_rel`, commit
  `8b6e03ac30ab688bec02c69d42f2304b7f72a202`, pinned by Zig content hash in
  `build.zig.zon`. Until 2026-10-02 (RA8FW-385) it was a vendored copy; all
  1220 of its source files were byte-identical to that commit, and the only
  local edit (a `.gitattributes` macro block) is not part of the tarball tree
  the build compiles.

## Use case in this firmware

<!-- consumer-census: key=netxduo total=7 hw_validated=5 c6=4 unsupported=1 -->

- Dual IPv4/IPv6 TCP/IP stack. **The TCP/IP core only** -- see "What is not
  built" below. Seven example applications declare it, **5** of them under
  `examples/ek_ra8d2/hw_validated/` and **1** under `examples/_unsupported/`.
  Two driver bindings carry it:
  - Wired, over the on-chip Ethernet MAC:
    `port/netxduo/src/nx_ether_driver_ra8_eth.c`, used by
    `examples/ek_ra8d2/hw_validated/hil/threadx_netx_tcp_echo` and by the two
    TLS apps (`examples/ek_ra8d2/hw_pending/tls_client`,
    `examples/_unsupported/threadx_https_client`), which get their TLS from
    Mbed TLS, not from this component.
  - Wireless, over the ESP32-C6 co-processor:
    `port/netxduo/src/nx_ether_driver_c6.c` (the `netxduo_port_c6` target),
    used by the four hw_validated apps under
    `examples/ek_ra8d2/hw_validated/c6/`: `c6_wifi_join` and `wifi_hal_join`,
    which additionally compile the vendored DHCP client
    (`addons/dhcp/nxd_dhcp_client.c`) to take an address on the bench network,
    plus `c6_camera_livestream` and `c6_camera_mjpeg`. Those two declare their
    middleware through the shared
    `examples/ek_ra8d2/common/c6_camera_server/c6_camera_server.cmake`
    wrapper rather than in their own `CMakeLists.txt`, which is why an earlier
    read of this record missed them.
- Integrity claim category: data-handling (frame parsing).

### What is not built

- **NetX Secure is compiled by nothing.** `cmake/netxduo.cmake` adds
  `nx_secure/inc` to the include path and nothing else. All 314
  `nx_secure/` files are outside the build graph. TLS in this firmware is Mbed
  TLS behind `libs/ra8_tls/`; no NetX TLS record layer exists in any image, so
  this qualification makes no TLS claim.
- **NetX Crypto is compiled by nothing either.** All 56
  `crypto_libraries/src` TUs used to be globbed into every NetX image, with no
  first-party caller of any `nx_crypto_*` symbol -- `--gc-sections` was the
  only thing keeping them out of flash. The glob was removed rather than
  justified: a second, unqualified crypto implementation inside this component
  is exactly the sort of thing a SOUP record must not have to explain away.
  `crypto_libraries/inc` remains on the include path because
  `common/inc/nx_api.h` needs the `NX_CRYPTO_METHOD` struct definition; the
  core reads its fields and never calls a crypto function.
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

None. The build uses the pinned upstream tarball as is. While NetX Duo was
vendored (until RA8FW-385, 2026-10-02) its one local edit was a
repository-hygiene change to `.gitattributes`: commit `368072a1a` dropped two
`[attr]` macro blocks that git only honours in the top-level `.gitattributes`.
No shipped source was ever changed, and that file is not part of the tree the
build compiles.

## Last review date

- Reviewed: 2026-05-02
- Use case + risk mitigation re-verified against the tree and corrected:
  2026-08-04. The document claimed a NetX Secure TLS role that no
  build has, cited an application deleted in `d38587e80`, put the driver
  boundary in a library the drivers do not call, and omitted the Wi-Fi
  consumers entirely.
- Expected re-review by: 2027-05-02
