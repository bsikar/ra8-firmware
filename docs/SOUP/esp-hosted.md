# SOUP Justification: Espressif esp-hosted-mcu (co-processor firmware)

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting Espressif's esp-hosted-mcu
co-processor firmware into this project as Software Of Unknown Provenance
(SOUP).

esp-hosted has two halves and this project consumes both. They are catalogued
as two components because they are qualified differently:

| Half | Runs on | Vendored? | Document |
|------|---------|-----------|----------|
| Co-processor firmware (**this document**) | ESP32-C6, flashed as its own image | no, built from a pinned recipe | this file |
| Host driver | RA8D2, linked into the RA8 image | yes, `libs/third_party/esp-hosted/` | [`esp-hosted-host.md`](esp-hosted-host.md) |

Both halves come from the same upstream commit `949bb30` and report the same
protocol version 2.12.11, which is what makes them wire-compatible.

## Component identity

- **Name**: Espressif esp-hosted-mcu (`network_adapter` application).
- **Firmware version**: 2.12.11 (esp-hosted protocol firmware version reported
  by the `network_adapter` app).
- **Source pin**: upstream commit
  `949bb30612747a3bd9e402eda8d01fbfa1f8503e` (short `949bb30`).
- **Build toolchain pin**: esp-idf `v5.5.4`.
- **Upstream URL**: <https://github.com/espressif/esp-hosted-mcu>.
- **Local path**: the **co-processor firmware is NOT vendored** into the tree.
  The build recipe (pinned versions, our proven `sdkconfig.defaults`, and the
  build/flash scripts) lives in `coprocessor/esp32c6/`;
  `coprocessor/esp32c6/build.sh` fetches the pinned upstream into the
  git-ignored `coprocessor/esp32c6/esp-hosted-mcu/` at build time.
  This is a statement about the C6 image only: the complementary **host
  driver from the same upstream repository IS vendored**, at
  `libs/third_party/esp-hosted/`, and is qualified separately in
  [`esp-hosted-host.md`](esp-hosted-host.md).

## Provenance

- **Origin**: Espressif Systems, the esp-hosted-mcu project.
- **License**: Apache-2.0 (upstream `LICENSE`).
- **How it enters our build**: `coprocessor/esp32c6/build.sh` clones the upstream
  repository, checks out the pinned commit `949bb30`, drops in the proven
  `coprocessor/esp32c6/sdkconfig.defaults`, and builds the `network_adapter`
  peripheral-side application with the pinned esp-idf. Nothing is copied into
  the repository; the recipe is the record.

## Use case in this project

- The ESP32-C6 is a **wireless co-processor**: it provides Wi-Fi and Bluetooth
  to the RA8D2 host over a SPI transport. The C6 runs the esp-hosted-mcu
  peripheral-side firmware; the RA8D2 is the host driving it.
- **Zero first-party code runs on the C6.** The entire C6 image is upstream
  esp-hosted-mcu. This is why it is catalogued as co-processor firmware SOUP
  rather than a linked library: it never enters the RA8D2 firmware binary.
- This project uses "co-processor" / "peripheral-side" for the C6 app to match its inclusive terminology standard, in place of the upstream role name. <!-- LEGACY-OK: names the upstream esp-hosted-mcu slave role verbatim -->
- Integrity-claim category: none. No safety signal in this project depends on
  the C6 link; it is a connectivity convenience.

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 (proven-in-use route) and
DO-178C Section 12.1.4 (previously developed software):

- **Service history**: esp-hosted / esp-hosted-mcu has shipped as Espressif's
  supported host-MCU connectivity solution across the ESP32 family for years,
  deployed in large volumes.
- **Vendor maintenance**: Espressif is the sole authoritative source for both
  the firmware and the ESP32-C6 silicon it runs on; fixes ship as tagged
  releases and commits on the upstream default branch.
- **Bug tracker review**: the upstream issue tracker at
  <https://github.com/espressif/esp-hosted-mcu/issues> was reviewed at
  vendor-in; no open advisory affects the SPI transport configuration this
  project uses.
- **Black-box treatment**: the firmware is admitted as a pre-developed
  component whose internal structure is not re-verified here (no source-level
  MC/DC, MISRA, or Doxygen audit). The compensating control is that the C6 is
  reachable only through the RA8-side host driver's single integration
  boundary and carries no integrity claim.

## Risk mitigation

- The C6 is a separate device on a SPI link; a fault is contained to the
  connectivity path and cannot corrupt RA8D2 state outside the host driver.
- The build is fully pinned (esp-idf `v5.5.4` + commit `949bb30` +
  verbatim `sdkconfig.defaults`), so the flashed image is reproducible.
- All host-side access will be mediated through a single RA8 driver (a
  separate follow-on), keeping one integration boundary.

## Deviations / patches

- **Modified**: no. The upstream tree is built unmodified at the pinned
  commit. The only project-supplied input is `coprocessor/esp32c6/sdkconfig.defaults`
  (build configuration: transport = SPI, the SPI pin assignments, and 16 MB
  flash size), which selects upstream options and patches no upstream source.

## CVE monitoring

The pinned commit is recorded in the SBOM registry
(`scripts/gen/sbom_registry.py`) with its upstream commit, so the weekly OSV scan
(`scripts/checks/osv_scan.sh`) issues a commit-range query for it against
OSV.dev alongside the vendored SOUP.

## Last review date

- Reviewed: 2026-07-26 (build recipe codified from the bench-proven build)
- Expected re-review by: 2027-07-26

## See also

- [`esp-hosted-host.md`](esp-hosted-host.md) -- the host-driver half, vendored
  at `libs/third_party/esp-hosted/`.
- [`../design/c6_wireless_architecture.md`](../design/c6_wireless_architecture.md)
  -- the design-level architecture.
