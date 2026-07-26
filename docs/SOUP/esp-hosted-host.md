# SOUP Justification: Espressif esp-hosted-mcu (host driver)

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting the **host-side** half of
Espressif's esp-hosted-mcu into this firmware as Software Of Unknown
Provenance (SOUP).

esp-hosted has two halves and this project consumes both. They are catalogued
as two components because they are qualified differently:

| Half | Runs on | Vendored? | Document |
|------|---------|-----------|----------|
| Host driver (**this document**) | RA8D2, linked into the RA8 image | yes, `libs/third_party/esp-hosted/` | this file |
| Co-processor firmware | ESP32-C6, flashed as its own image | no, built from a pinned recipe | [`esp-hosted.md`](esp-hosted.md) |

Both halves come from the same upstream commit and report the same protocol
version (2.12.11), which is what makes them wire-compatible.

## Component identity

- **Name**: Espressif esp-hosted-mcu, host driver + shared protocol.
- **Version**: 2.12.11 (`ESP_HOSTED_VERSION_*` in
  `host/esp_hosted_host_fw_ver.h`).
- **Source pin**: upstream commit
  `949bb30612747a3bd9e402eda8d01fbfa1f8503e` (short `949bb30`).
- **Upstream URL**: <https://github.com/espressif/esp-hosted-mcu>.
- **Local path**: `libs/third_party/esp-hosted/`.
- **Integrity**: aggregate SHA-256
  `79ae04974accce04871f64d6e5cfb1e46676a4e70a0252eed616405826d29cc0`
  (SHA-256 over the newline-joined, name-sorted per-file SHA-256 hashes of
  the 77 esp-hosted-owned files; the separately-pinned nested protobuf-c
  subtree is excluded and hashed on its own). Recorded in
  `scripts/gen/sbom_registry.py`.

### Nested component: protobuf-c

Upstream carries protobuf-c as a **git submodule** at `common/protobuf-c`, so
it is a distinct upstream project under a distinct license that merely happens
to be embedded by Espressif. It therefore gets its own SBOM entry, its own
license record, and its own OSV commit query rather than disappearing into the
esp-hosted aggregate hash.

- **Name**: protobuf-c (Protocol Buffers C runtime).
- **Version**: 1.4.1 (`PROTOBUF_C_VERSION` in `protobuf-c/protobuf-c.h`).
- **Source pin**: submodule commit
  `abc67a11c6db271bedbb9f58be85d6f4e2ea8389`.
- **Upstream URL**: <https://github.com/protobuf-c/protobuf-c>.
- **Local path**: `libs/third_party/esp-hosted/common/protobuf-c/`.
- **License**: BSD-2-Clause (upstream `LICENSE`).
- **Integrity**: aggregate SHA-256
  `67da2264194eb142d30830ab92a8a64decb1557d4d4ee8a82dddd7f731784d7f`
  over its 3 files.

## Provenance

- **Origin**: Espressif Systems, the esp-hosted-mcu project.
- **License**: Apache-2.0 (upstream `LICENSE`, vendored verbatim). Upstream
  ships no separate `NOTICE` file, so there is no NOTICE propagation
  obligation beyond the license text itself.
- **How it entered our tree**: `git archive` of the pinned commit, restricted
  to `host/`, `common/`, `LICENSE` and `README.md`, with two subtractions (see
  "Vendoring scope"). The protobuf-c submodule was materialized separately
  from its own upstream at the submodule's pinned commit. All **80** vendored
  files were verified byte-identical to their upstream pins after copying.

## Vendoring scope

**Kept: the whole host driver and shared protocol, all four transports.**
This follows the repository's dominant pattern for driver and stack SOUP
(NimBLE 858 files, the ThreadX family wholesale): vendor the subtree as
upstream ships it. Pruning the sibling transports would not be a file-level
subset selection like libwebp's decode-only split -- transport choice is a
compile-time switch (`H_TRANSPORT_IN_USE`) evaluated *inside shared files*
(`esp_hosted_transport_config.c` alone names all four 38 times), so removing
`sdio/`, `spi_hd/` or `uart/` would force edits to shared source and cost this
component its "Deviations: none" status. Unused transports cost link-time
size, not correctness, and the linker drops what is never referenced.

**Subtracted (two file-level exclusions, no source edited):**

- `host/port/` -- the upstream ESP-IDF / FreeRTOS port. Replaced by a
  first-party RA8 port, per the contract below. It is excluded rather than
  vendored-and-ignored because its headers `#include "freertos/FreeRTOS.h"`,
  `esp_timer.h`, `esp_wifi_types.h` and friends: they are not
  interface-neutral glue and could never compile here.
- `common/esp_hosted_lwip_src_port_hook.h` -- verified unreferenced by any
  driver translation unit. Its only upstream consumer is the top-level
  `CMakeLists.txt`, which force-includes it into **ESP-IDF's lwip** build.
  This project does not build ESP-IDF's lwip, so nothing can reach it.

## Use case in this firmware

- The ESP32-C6 is a wireless co-processor providing Wi-Fi and Bluetooth to the
  RA8D2 over a SPI transport. This component is the RA8D2-side driver that
  speaks the esp-hosted protocol to it: transport framing, the RPC codec, the
  serial and Bluetooth HCI channels, and the power-save path.
- This project uses "co-processor" / "peripheral-side" for the C6 role to match
  its inclusive terminology standard, in place of the upstream role name. Paths
  and symbols inside the vendored tree keep their upstream spelling and are not
  renamed. <!-- LEGACY-OK: explains the upstream esp-hosted slave role naming retained in vendored paths -->
- Integrity-claim category: connectivity convenience. No safety signal in this
  project depends on the C6 link.

## Build status: not yet compiled

**No CMake target builds this source yet, by design.** The driver cannot
compile until the first-party port exists, because the port supplies headers
the core includes by name. The port under `port/esp-hosted/` and the build
wiring land together in the follow-on change; this change is source vendoring
and records only.

## The port contract

This is the interface our first-party port must satisfy. It is stated here
because deleting `host/port/` is what creates the obligation.

### 1. Ten port headers, included by name

The core includes these by exact filename, so the port must provide headers
with these names on the include path. Counts are the symbols each upstream
header defines that the vendored core actually references:

| Header | Symbols the core needs | Includers | Purpose |
|--------|------------------------|-----------|---------|
| `port_esp_hosted_host_config.h` | 97 | 19 | Transport selection, GPIO pin/port map, queue sizes, timeouts, feature flags |
| `port_esp_hosted_host_os.h` | 26 | 12 | RTOS handle typedefs, task priorities/stack sizes, allocation macros, return codes |
| `port_esp_hosted_host_wifi_config.h` | 17 | 10 | Wi-Fi feature-availability flags matched to the peripheral-side API level |
| `port_esp_hosted_host_bt_config.h` | 3 | 1 | Bluetooth host selection and low-level init flag |
| `port_esp_hosted_host_log.h` | 1 | 13 | `DEFINE_LOG_TAG` plus the log-level macros |
| `port_esp_hosted_host_openthread.h` | 1 | 2 | `H_HOST_OT_ENABLE` (disabled for this project) |
| `port_esp_hosted_host_spi.h` | 1 | 1 | `MAX_TRANSPORT_BUFFER_SIZE` for full-duplex SPI |
| `port_esp_hosted_host_sdio.h` | 2 | 1 | Buffer size + unresponsive-peripheral code |
| `port_esp_hosted_host_spi_hd.h` | 1 | 1 | `MAX_TRANSPORT_BUFFER_SIZE` for half-duplex SPI |
| `port_esp_hosted_host_uart.h` | 1 | 1 | `MAX_TRANSPORT_BUFFER_SIZE` for UART |

Only the header matching the selected `H_TRANSPORT_IN_USE` is pulled in by
`transport_drv.h`, so the SPI-transport build needs
`port_esp_hosted_host_spi.h` and may leave the other three minimal.

### 2. The OS abstraction vtable

`host/esp_hosted_os_abstraction.h` **is vendored** -- it is the seam itself,
not part of the port. It declares `hosted_osi_funcs_t`, a **72-entry function
pointer table** the port must populate: memory (8), threads and delays (7),
queues (5), mutexes (4), semaphores (5), timers (3), mempool locks (4), GPIO
(7), bus init and transfer (3), event posting (2), plus transport-specific
blocks for SDIO (7), half-duplex SPI (6) and UART (3), and the printf, init
hook, restart and power-save hooks.

### 3. Eight link-time symbols

Declared in vendored headers, defined only in the excluded port, so the port
must define them:

| Symbol | Kind | Needed when |
|--------|------|-------------|
| `g_h` | `struct hosted_config_t` | always |
| `g_hosted_osi_funcs` | `hosted_osi_funcs_t` | always |
| `esp_hosted_get_default_spi_config` | function | SPI transport (this project) |
| `esp_hosted_get_default_sdio_config` | function | SDIO transport |
| `esp_hosted_get_default_sdio_iomux_config` | function | SDIO transport |
| `esp_hosted_get_default_spi_hd_config` | function | half-duplex SPI transport |
| `esp_hosted_get_default_uart_config` | function | UART transport |
| `esp_hosted_openthread_get_radio_config` | function | `H_HOST_OT_ENABLE` only |

The four unused transport defaults are declared unconditionally in the public
header but only *called* through the `INIT_DEFAULT_HOST_*_CONFIG()` macros, so
an SPI-only build resolves with `g_h`, `g_hosted_osi_funcs` and
`esp_hosted_get_default_spi_config` alone.

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 (proven-in-use route) and
DO-178C Section 12.1.4 (previously developed software):

- **Service history**: esp-hosted / esp-hosted-mcu is Espressif's supported
  host-MCU connectivity solution across the ESP32 family, deployed in large
  volumes for years. This host driver is the same code every esp-hosted
  integration runs.
- **Vendor maintenance**: Espressif is the sole authoritative source for the
  driver, the peripheral-side firmware and the silicon; fixes ship as tagged
  releases and commits on the upstream default branch.
- **Bug tracker review**: the upstream issue tracker at
  <https://github.com/espressif/esp-hosted-mcu/issues> was reviewed at
  vendor-in; no open advisory affects the SPI transport configuration this
  project uses.
- **protobuf-c**: a stable, widely deployed C implementation of Protocol
  Buffers; version 1.4.1 is a tagged release. Only the runtime is vendored,
  not the code generator.
- **Black-box treatment**: admitted as a pre-developed component whose
  internal structure is not re-verified here -- no source-level MC/DC, MISRA,
  Doxygen or NASA Power of 10 audit, per the `libs/third_party/` exemption in
  `CLAUDE.md`.

## Risk mitigation (compensating controls)

- **One integration boundary.** All C6 access is mediated by the first-party
  port and driver module; application code never reaches the vendored API
  directly.
- **The port is first-party and fully governed.** Every line of
  `port/esp-hosted/` is held to the full project bar (C23, Doxygen, MC/DC,
  NASA P10). Because the vendored core reaches the hardware *only* through the
  72-entry vtable and the port headers, the entire hardware-facing surface of
  this SOUP is first-party code under test.
- **No safety claim.** The link carries connectivity, not any integrity
  signal; a fault is contained to the connectivity path.
- **Untrusted input is remote.** The driver parses frames produced by the C6.
  The RPC codec path (protobuf-c plus the generated
  `esp_hosted_rpc.pb-c.c`) is the parsing surface that most warrants a fuzz
  harness once the port makes the code buildable.
- **Fully pinned and reproducible.** Commit pin plus aggregate SHA-256 for
  both the esp-hosted tree and the nested protobuf-c tree; every vendored file
  was verified byte-identical at vendor-in.

## Deviations / patches

**None.** All 80 vendored files are byte-identical to their upstream pins
(77 to esp-hosted `949bb30`, 3 to protobuf-c `abc67a11`). The two omissions
described under "Vendoring scope" are whole-file exclusions, not source
modifications; no vendored file was edited. All porting happens in
first-party code outside this directory.

## CVE monitoring

Both pins are recorded in the SBOM registry (`scripts/gen/sbom_registry.py`) with
their upstream commits, so the weekly OSV scan
(`scripts/checks/osv_scan.sh`) issues a commit-range query for
esp-hosted-mcu `949bb30` and for protobuf-c `abc67a11` against OSV.dev
alongside the rest of the vendored SOUP.

## Last review date

- Reviewed: 2026-07-26 (host driver vendored at `949bb30`)
- Expected re-review by: 2027-07-26

## See also

- [`esp-hosted.md`](esp-hosted.md) -- the co-processor firmware half.
- [`../design/c6_wireless_architecture.md`](../design/c6_wireless_architecture.md)
  -- the design-level architecture and the rationale for a co-processor.
- [`../../THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md) -- the
  aggregated license inventory.
