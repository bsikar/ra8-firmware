# Missing / Incomplete (RA8D2-only)

Audit date: 2026-04-30 (post-cleanup, RA8D2-focused).

This document lists what is **NOT yet at FSP-grade feature parity for the
Renesas RA8D2 silicon specifically**. Anything in FSP that targets a
different chip family (RA2 / RA4 / RA6 / older RX) has been removed from
our tree -- if a peripheral is not on RA8D2, it is not "missing", it is
not applicable.

The ground truth for what RA8D2 actually has is
`/Users/bsikar/Documents/github/_reference/fsp/ra/fsp/src/bsp/mcu/ra8d2/bsp_feature.h`.
Verified via `BSP_FEATURE_*_IS_AVAILABLE` flags.

For "what we need to do", see `docs/ROADMAP_RA8D2.md`.

---

## 1. Drivers we have, but with a software stub instead of real silicon I/O

These would compile, pass simulator tests, but produce wrong results on
real RA8D2 hardware. Each is the highest-priority work to fix.

### 1.1 Crypto (`libs/ra_hal/src/ra_rsip.c` + peers)

RA8D2 carries the **RSIP-E50D** crypto block (per
`BSP_FEATURE_RSIP_RSIP_E50D_SUPPORTED = 1`). Our `ra_rsip` driver
currently:

- Has the FSP-shaped public API (`ra_rsip_init` / `ra_rsip_aes_gcm` /
  `ra_rsip_sha256` / `ra_rsip_trng_read` /
  `ra_rsip_aes128_install_plain` / etc.).
- **Does not drive the RSIP-E50D registers.** The xorshift64* mixer
  produces deterministic byte streams that round-trip in unit tests
  but are NOT real cryptography.
- Real production code needs to either (a) drive the RSIP-E50D
  registers per the HUM, or (b) link against Renesas's signed
  `hw_sce_*.c` blobs (Renesas-only license, cannot ship).

Affects `ra_rsip_key_injection`, `ra_rsip_protected` peers since they
share the same software stub.

### 1.2 BLE radio (`libs/ra_hal/src/ra_ble.c`, `libs/ra_ble_host/`)

RA8D2 has a BLE radio block. Our driver:

- Models the HCI command / event / ACL ring at a placeholder base
  address.
- Does NOT load the Renesas-supplied encrypted BLE firmware patch
  into the controller. The patch is required for the radio to work
  at all on real silicon. Renesas does not publish the patch image;
  their FSP `r_ble` driver loads it from a license-restricted blob.
- L2CAP / ATT / GATT host stack (`libs/ra_ble_host/`) is real C code
  on top of the controller, but the controller never gets a working
  firmware, so end-to-end BLE does not function on hardware.

---

## 2. Drivers that drive registers but are partial

### 2.1 I3C (`libs/ra_hal/src/ra_i3c.c`)

Sweep 3 brought dynamic-address-assignment + the CCC engine. Still
missing:
- HDR-DDR (Double Data Rate) mode
- In-Band Interrupt (IBI) inbound queue (NTIBIQP / NTIBIVCTL)
- Slave-mode operation entirely
- Hot-join handling

### 2.2 CANFD (`libs/ra_hal/src/ra_canfd.c`)

Still missing:
- GAFL (Global Acceptance Filter List) for hardware filtering
- Bit-rate-switching mode for CAN-FD payload phase
- ISO vs non-ISO mode select
- Time-triggered transmission (TTCAN)
- Transmit-history queue

### 2.3 MIPI-DSI / MIPI-CSI (`libs/ra_hal/src/ra_mipi_*.c`)

Sweep 6 added video-mode timing for DSI and virtual-channel for CSI.
Still missing:
- DSI command-mode payload transfer
- DSI ULPS (Ultra Low Power State) entry / exit
- CSI ULPS entry / exit
- Continuous-clock mode

### 2.4 USB host HID (`libs/ra_hal/src/ra_usb_hhid.c`)

`ra_usb_hhid_get_report` IN data phase still stubbed (NOLINT comment
present). The SETUP packet is correctly issued; the response payload
is not collected.

### 2.5 Flash / MRAM (`libs/ra_hal/src/ra_flash.c`)

Sweep 4 added erase / program / blank-check. Still missing:
- Suspend / resume during long ops
- Code-bank-swap (if supported on dual-bank parts)
- Background ops with completion callback
- Lock-bit programming for read protection

### 2.6 Small gaps on smaller drivers

Each has a documented FSP feature gap that doesn't block typical use:

- `ra_dac_b`: data-format register single-shot only, no batch
  conversion (DAE bit), no ADC-trigger sync
- `ra_acmphs`: filter-routing matrix not exposed
- `ra_rtc`: alarm B not wired (only alarm A)
- `ra_agt`: pulse-width-measurement mode not exposed
- `ra_ulpt`: pulse-output mode not exposed
- `ra_lvd`: reset-on-trip mode not exposed (only IRQ mode)
- `ra_doc`: comparator chain only one-shot per call
- `ra_bkup`: TZ secure-domain hand-off not wired

---

## 3. Drivers that are scaffolds (register-correct but minimal API)

These have correct register layouts (cross-verified vs FSP CMSIS) but
their public API is sparse. Real applications would either need to
extend them or hand-write registers.

| Driver | What's there | What's missing |
|---|---|---|
| `ra_drw` | controller bring-up | Hardware blit / fill / line / glyph paths |
| `ra_etha` | open / close | Per-port descriptor ring management |
| `ra_vin` | power-on, capture-start | Frame-end IRQ + dynamic capture window |
| `ra_tsn` | open / close | TSN scheduler / time-aware shaper / FRER |
| `ra_rmac` | MDIO read/write | Auto-neg state machine for the GbE PHY |
| `ra_ssie` | init / start | DMAC-paired iso transfer, FIFO threshold IRQs |
| `ra_dotf` | init | Hardware tamper-protected flash mapping |
| `ra_cnecc` | init | ECC computation paths beyond status read |
| `ra_ipc` | mailbox open | Multi-core M85<->M33 dispatch glue |
| `ra_pdg` | open / read | Capture-start / completion callback |
| `ra_bscan` | placeholder | Boundary-scan path |
| `ra_ceu` | open / close | DMAC-driven framebuffer fill |

---

## 4. Top-level libraries that are minimal-rather-than-complete

### 4.1 `libs/ra_net` (sweep 8)

Minimal IPv4 stack. Compared to lwIP / NetXDuo:
- No retransmission timer (TCP)
- No congestion control
- Single-connection TCP
- DNS A-records only with no compression
- IP options not parsed
- No fragmentation reassembly
- No DHCP client
- No ICMPv6 / IPv6

### 4.2 `libs/ra_fs` (sweep 9)

Minimal FAT12 / 16 / 32 read+write.
- 8.3 short filenames only (no LFN)
- 4 file handles, 2 mount points
- No directory creation (only listdir)
- No exFAT, no journaling

### 4.3 `libs/ra_gfx` (sweep 9)

Software pixel pusher only.
- DRW hardware acceleration deferred behind `RA_GFX_USE_DRW`
- One bundled font (8x16 IBM PC VGA)
- No font scaling / sub-pixel rendering

### 4.4 `libs/ra_ble_host` (sweep 10)

Minimal GATT server.
- 8 services, 32 characteristics
- No GATT client
- No Bluetooth Mesh
- No HID-over-GATT profile catalog
- No bonding / pairing / security manager

### 4.5 TLS (deleted, will be replaced by 3rd-party)

`libs/ra_tls` was removed in this cleanup. Per the roadmap, TLS
will be provided by **mbedTLS** (with ALT-provider hooks routing
AES + SHA into ra_rsip once RSIP-E50D registers are real) or by
**NetX Secure** (if going all-in on Eclipse ThreadX, which is the
project's chosen RTOS).

See `docs/ROADMAP_RA8D2.md` Phase 1.2 for details.

---

## 5. Examples not yet validated on real hardware

Of the 17 example apps, only `uart_hello` and `clock_check` were
actually flashed to the EK-RA8D2 and verified working. The other 15
compile clean but were never flashed:

`blink`, `blink_hal`, `usb_cdc_echo`, `usb_host_cdc_echo`,
`usb_hid_device`, `usb_msc_device`, `usb_host_msc_browse`,
`usb_host_keyboard`, `lcd_demo`, `ethernet_tcp_echo`,
`audio_loopback`, `motor_3phase`, `usb_audio_device`,
`ble_peripheral`, `ptp_master`.

Several have pin-mux tables annotated
`/* TODO: confirm against EK-RA8D2 v1 manual */` because the manual
is not yet committed under `docs/reference/`.

---

## 6. What FSP has that we genuinely don't

These are FSP services that operate on top of the HAL layer; we have
either nothing or a drastically minimal substitute.

| FSP component | What it is | Plan |
|---|---|---|
| FreeRTOS / ThreadX / Azure RTOS port | Full RTOS scheduler + glue | **Adopt Eclipse ThreadX** (Phase 0) |
| FileX / exFAT | Production filesystem | **Adopt FileX** (Phase 4.2; ships with ThreadX) |
| GUIX / TouchGFX integration | GUI framework | **Adopt GUIX** (Phase 4.3; ships with ThreadX) |
| NetXDuo / lwIP | Production TCP/IP | **Adopt NetX Duo** (Phase 4.1; ships with ThreadX) |
| mbedTLS / NetXSecure | TLS stack | **Adopt mbedTLS** with ra_rsip ALT shims (Phase 1.2), OR NetX Secure (Phase 1.2 alternate) |
| Bluetooth host (Mesh / GATT client / bonding) | Standard BLE profiles | **Adopt Apache NimBLE** (Phase 1.3) |
| QE configurator | GUI clock-tree / pin-mux / IRQ generator | Hand-written register sequences (Phase 5 deferred) |
| `rm_*` mid-level libs | Higher-layer abstractions | None planned |

---

## 7. Genuinely impossible (cannot fix without Renesas IP)

These are blockers no amount of hand-written code can fix:

- **Renesas RSIP firmware blobs** (`hw_sce_*.c` in FSP tree,
  license-restricted). Without these, OEM-provisioned key handling /
  attestation / Renesas-managed key infrastructure cannot work.
  Driving raw AES-GCM with a software-loaded key IS achievable from
  datasheet-only material -- that's a real primitive call.
- **Renesas BLE firmware patch image.** Closed-source binary required
  by the BLE controller silicon to function. Same situation.
- **EK-RA8D2 v1 board manual.** The schematic + pin-mux tables for
  the development board are published by Renesas but not yet
  committed under `docs/reference/`. Several example apps have
  placeholder pin-mux that may be wrong on first flash.

---

## 8. Summary

After 9 cross-verify iterations + 12 implementation sweeps + this
RA8D2-focused cleanup:

- **126 driver source files** across `libs/ra_hal/`, `libs/ra_net/`,
  `libs/ra_fs/`, `libs/ra_gfx/`, `libs/ra_ble_host/`, `libs/ra_core/`,
  `libs/ra_nsc/`, `libs/ra_net_pal/`, `libs/ra_usb_pal/`.
- **113 host-side test executables, all passing.**
- **17 hardware-flashable example apps** (the `cap_touch_demo`,
  `usb_typec`, `ra_sce*`, `ra_pdc`, and other non-RA8D2 modules
  removed in this cleanup).
- All 113 tests pass. `format_code.sh --check` and
  `clang_tidy.sh --check` both clean.
- 2 of 17 apps verified on EK-RA8D2 hardware so far.

Everything in this document is **either software work we can do**
(sections 1-6) or **third-party blob work we cannot ship** (section 7).

For the actionable plan, see `docs/ROADMAP_RA8D2.md`.
