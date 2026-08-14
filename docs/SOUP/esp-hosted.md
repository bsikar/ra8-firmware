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
  repository, checks out the pinned commit `949bb30`, applies the reviewed
  `0001-custom-rpc-sync-response-hook.patch`, stages the first-party
  `ra8_mdl_service` component, drops in the proven
  `coprocessor/esp32c6/sdkconfig.defaults`, and builds the `network_adapter`
  peripheral-side application with the pinned esp-idf. The fetched SOUP is not
  vendored; the patch, component, and recipe are the reviewable record.

## Use case in this project

- The ESP32-C6 is a **wireless co-processor** reached over a SPI transport,
  running the esp-hosted-mcu peripheral-side firmware with the RA8D2 as host.
  **Wi-Fi is what works today** and is bench-proven (station join + DHCP on
  `hw_validated/c6/`). Bluetooth is planned, not delivered: `cmake/esp_hosted.cmake`
  excludes the host `drivers/bt/` bridge, `coprocessor/esp32c6/sdkconfig.defaults`
  says nothing about the BT stack so what the C6 image contains is
  undetermined, and the RA8-side BLE work is still open (#493).
- The C6 image is mixed: pinned esp-hosted-mcu SOUP plus the first-party
  `ra8_mdl_service` component. A small checked patch exposes a synchronous,
  bounded CustomRpc response hook; the first-party component implements a
  pull-based HTTPS artifact transfer behind it. The C6 image remains separate
  from the RA8D2 image, and only the upstream portion is accepted as SOUP.
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
- **Black-box treatment**: the upstream portion is admitted as a pre-developed
  component whose internal structure is not re-verified here (no source-level
  MC/DC, MISRA, or Doxygen audit). The patch and `ra8_mdl_service` are
  first-party code and remain subject to normal source review and tests. The
  C6 is reachable only through the RA8-side host driver's integration boundary
  and carries no integrity claim.

## Risk mitigation

- The C6 is a separate device on a SPI link; RA8-side message correlation,
  bounds checks, digest verification, and storage ownership contain a remote
  fault to the connectivity/download path.
- The build is fully pinned (esp-idf `v5.5.4` + commit `949bb30` +
  verbatim `sdkconfig.defaults`), so the flashed image is reproducible.
- Host-side access is mediated through first-party RA8 code -- the port at
  `port/esp-hosted/` and the driver `libs/ra8_c6link/`, both landed -- keeping
  one integration boundary. (Two bring-up applications still reach vendored
  headers directly; see `esp-hosted-host.md`.)

## Deviations / patches

- **Modified**: yes. The build applies
  `coprocessor/esp32c6/patches/0001-custom-rpc-sync-response-hook.patch` to the
  exact pinned commit. It adds a weak bounded CustomRpc hook and registers the
  first-party component; it does not alter radio, transport, or Wi-Fi logic.
  `git apply --check` makes upstream drift a hard build failure, and the build
  asserts that the strong first-party handler symbol is present in the final
  ELF.

## CVE monitoring

The pinned commit is recorded in the SBOM registry
(`scripts/gen/sbom_registry.py`) with its upstream commit, so the weekly OSV scan
(`scripts/checks/osv_scan.sh`) issues a commit-range query for it against
OSV.dev alongside the vendored SOUP.

## Last review date

- Reviewed: 2026-07-26 (build recipe codified from the bench-proven build)
- Use case + mitigation tense corrected against the tree (#612): 2026-08-04.
  The RA8-side driver is no longer a "follow-on", and the Bluetooth half is
  now stated as planned rather than provided.
- Mixed-image trust boundary and media-service patch recorded: 2026-08-13.
- Expected re-review by: 2027-07-26

## See also

- [`esp-hosted-host.md`](esp-hosted-host.md) -- the host-driver half, vendored
  at `libs/third_party/esp-hosted/`.
- [`../design/c6_wireless_architecture.md`](../design/c6_wireless_architecture.md)
  -- the design-level architecture.
