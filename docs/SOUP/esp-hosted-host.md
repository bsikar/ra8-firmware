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
- **Integrity**: the 77 esp-hosted-owned files are pinned file by file in
  `docs/sbom/upstream/esp-hosted.manifest` (the nested protobuf-c subtree is
  excluded and pinned separately, below). Those hashes come from a real fetch
  of upstream, and `scripts/checks/check_soup_upstream.py` compares them
  against this tree on every run of the `soup-upstream` gate; the per-run
  derived digest is published in `docs/sbom/ra8-firmware.cdx.json`. No
  aggregate hash is transcribed into the registry -- #538 removed that field
  because a hand-copied constant compared against itself reports clean on a
  mutated byte.

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
- **Integrity**: its 3 files are pinned individually in
  `docs/sbom/upstream/esp-hosted/protobuf-c.manifest`, checked by the same
  `soup-upstream` gate.

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
(NimBLE and the ThreadX family, both vendored wholesale): vendor the subtree as
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

## Build status: the SPI transport half compiles, and runs on silicon

The first-party port landed at `port/esp-hosted/`, and `cmake/esp_hosted.cmake`
compiles **eight** of the vendored translation units into the
`esp_hosted_objs` object library behind the `RA8_USE_ESP_HOSTED` option. Five
applications consume it, all of them under
`examples/ek_ra8d2/hw_validated/c6/`: `c6_fw_version`, `c6_hosted_init`,
`c6_wifi_join`, `c6_wifi_link` and `wifi_hal_join`. The cross-build gate
therefore covers it on every push.

**Compiled today** -- the SPI transport, the serial (control-plane) lower
layer, the RPC wire codec and the shared utilities:

`host/drivers/transport/spi/spi_drv.c`, `host/drivers/transport/transport_util.c`,
`host/api/src/esp_hosted_transport_config.c`,
`host/drivers/serial/serial_ll_if.c`, `host/drivers/power_save/power_save_drv.c`,
`host/utils/stats.c`, `common/proto/esp_hosted_rpc.pb-c.c`,
`common/protobuf-c/protobuf-c/protobuf-c.c`.

**Not compiled, and why.** Four groups, for four different reasons:

- `host/drivers/transport/transport_drv.c`, the whole `host/drivers/rpc/`
  layer, `host/api/src/esp_hosted_api.c` and `esp_wifi_weak.c` are written
  against ESP-IDF's Wi-Fi API, naming 43 distinct `wifi_*_t` / `esp_netif_*`
  types this tree does not have. They are excluded because they are upstream's
  API surface, **not** because the capability is missing. An earlier revision
  of this document argued that those struct layouts "are what the co-processor
  decodes on the far side of the link" and called reproducing them the next
  piece of work; #490 disproved that on the bench. The C6 decodes **protobuf**:
  `esp_hosted_rpc.pb-c.{h,c}` contains zero `wifi_config_t` / `esp_netif`
  references, `WifiStaConfig` is a message with named fields, and padding or
  field order on this side never reaches the co-processor. The landed
  replacement is the first-party `libs/ra8_c6link/`, which speaks the same
  wire through the generated codec compiled above and takes the C6's Wi-Fi
  station up on silicon (`c6_wifi_link`).
- `common/mempool/mempool*.c` is excluded structurally: `mempool_ll.h`
  includes `freertos/FreeRTOS.h`, `portmacro.h`, `task.h` and `semphr.h`
  unconditionally. It is a FreeRTOS data structure and this image runs
  ThreadX. `H_USE_MEMPOOL` is therefore left **undefined** -- the vendored
  vtable header guards four of its rows with `#ifdef`, so defining that symbol
  even to zero would grow `hosted_osi_funcs_t`, and because that header does
  not include the port config, the struct's layout would then depend on
  per-TU include order with no diagnostic. With the symbol absent, the port's
  own fixed ThreadX byte pool supplies the bounded, allocate-once behaviour
  the upstream pool existed to provide.
- `host/drivers/serial/serial_drv.c` and
  `host/drivers/virtual_serial_if/serial_if.c` expand `HOSTED_CALLOC`, an
  allocate-or-bail macro whose failure arm is a `goto` to a caller-supplied
  label. (`host/drivers/rpc/core/rpc_core.c` expands it too, and is excluded
  for the ESP-IDF-API reason above.) Supplying that macro would put a `goto`
  in first-party code, which NASA Power of 10 Rule 1 forbids and
  `check_no_goto_setjmp.py` rejects with no allowlist. Substituting a `return`
  for the jump is not equivalent either: `serial_drv.c`'s `free_bufs` label
  frees two buffers before returning, so a bare `return` would leak. Both
  files are therefore excluded. `serial_ll_if.c`, the lower layer that does
  not expand the macro, is compiled.
- `host/drivers/transport/{sdio,spi_hd,uart}/` and `host/drivers/bt/` are
  transports and a Bluetooth bridge this integration does not use yet: the
  C6 is reached over full-duplex SPI only, and the Bluetooth pair needs the
  NimBLE transport headers and belongs with the NimBLE integration.

This port **runs on silicon**. The wire beneath it was qualified first -- the
probe `examples/ek_ra8d2/hw_validated/c6/c6_spi_probe` walked every J26 hole on
2026-07-27 and brought the raw link up at SPI mode 3 / 1 MHz with zero bad
checksums, driving the SCI directly -- and the first protocol round-trip
through the code described here landed in `6d7ddb532`. Every application that
links this port consequently sits under
`examples/ek_ra8d2/hw_validated/c6/`; none is in `hw_pending/`. They form
their own HIL lane (`make hil-c6`) rather than joining `hw_validated/hil/`,
because `ra8_emulator` models no ESP32-C6 (#494) and the EIL-parity gate over
that directory rightly admits no skips.

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

- **One integration boundary, with two bring-up exceptions stated honestly.**
  Production-shaped access goes through the first-party `libs/ra8_c6link/`,
  which owns the vendored RPC codec and frame header behind its own API --
  that is the path `c6_wifi_join`, `c6_wifi_link` and `wifi_hal_join` take.
  The two bring-up applications predate it and do reach vendored headers
  directly: `c6_hosted_init` and `c6_fw_version` include
  `esp_hosted_header.h`, and `c6_fw_version` additionally includes
  `esp_hosted_rpc.pb-c.h` and `protobuf-c/protobuf-c.h` to assemble a version
  query. Moving those two onto `ra8_c6link` is the change that would make the
  boundary absolute.
- **The port is first-party and fully governed.** Every line of
  `port/esp-hosted/` is held to the full project bar (C23, Doxygen, MC/DC,
  NASA P10). Because the vendored core reaches the hardware *only* through the
  72-entry vtable and the port headers, the entire hardware-facing surface of
  this SOUP is first-party code under test.
- **No safety claim.** The link carries connectivity, not any integrity
  signal; a fault is contained to the connectivity path.
- **Untrusted input is remote, and this surface is NOT yet fuzzed.** The
  driver parses frames produced by the C6. The RPC codec path (protobuf-c plus
  the generated `esp_hosted_rpc.pb-c.c`) is the parsing surface that most
  warrants a fuzz harness, and every precondition for one is now met: the code
  is buildable, host-tested (`tests/cmake/tests_c6link.cmake` compiles the
  codec) and parsing remote frames on silicon. No harness covers it -- an open
  gap tracked by #612, which is the one part of this component's audit that
  is not a documentation fix.
- **Fully pinned and reproducible.** A commit pin plus a per-file upstream
  manifest for both the esp-hosted tree and the nested protobuf-c tree -- see
  "Integrity" above, which is also why no aggregate hash is transcribed into
  the registry. Every vendored file was verified byte-identical at vendor-in.

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
- Build status, compiled-TU list, integration boundary and integrity clause
  re-verified against the tree and corrected (#612): 2026-08-04. The document
  still said this port had never run on silicon, listed nine compiled TUs
  including one the build excludes, and taught the premise #490 disproved.
- Expected re-review by: 2027-07-26

## See also

- [`esp-hosted.md`](esp-hosted.md) -- the co-processor firmware half.
- [`../design/c6_wireless_architecture.md`](../design/c6_wireless_architecture.md)
  -- the design-level architecture and the rationale for a co-processor.
- [`../../THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md) -- the
  aggregated license inventory.
