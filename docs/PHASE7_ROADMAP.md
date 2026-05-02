# Phase 7 Roadmap

This document scopes the work that comes after the v0.1.0 code-complete
baseline. It is intentionally narrow: only items that are actually
actionable on the EK-RA8D2 hardware in front of the developer, plus a
short speculative-only section at the end.

The authoritative starting point for "what is already done" is
[`CHANGELOG.md`](../CHANGELOG.md) entry `[0.1.0] - 2026-05-01`.

---

## 1. What's done

The v0.1.0 baseline lands the hand-written RA8D2 HAL, TrustZone
substrate (`secure_app` + `ra_nsc` veneer set), the
`ra_board_ek_ra8d2` BSP, the `ra_net_pal` / `ra_usb_pal` platform
abstraction layers, vendored ThreadX / FileX / LevelX / NetX / GUIX /
USBX / lwIP / NimBLE / TF-PSA-Crypto subtrees with hand-written
integration shims, OTA orchestration with secure-side commit veneers,
the `ra_bootloader` / `ra_epub` / `ra_modem_at` / `ra_psa_crypto` /
`ra_wdt_supervisor` higher-level libraries, the 36-app
`examples/` fleet, and the supporting tooling (Doxygen target,
`tools/ra_qe`, `scripts/openocd`, `scripts/build_all_examples.sh`,
`make test-docker`, the `cite_check --strict` and `check_world_tags`
gates). Roughly 235 commits; see [`CHANGELOG.md`](../CHANGELOG.md) for
the per-area breakdown.

---

## 2. Hardware validation queue (Phase 7.1)

All 36 example apps under `examples/` cross-compile cleanly against
arm-none-eabi-gcc and pass host-side unit tests. Only **two** have
real-silicon mileage on the EK-RA8D2: `uart_hello` (verified
"hello, ra8d2!" on the J-Link OB VCOM) and `clock_check` (verified
PLL bring-up via the same VCOM). Everything else is bring-up risk.

The tiers below are ordered by escalating "what can go wrong" --
flash from the top down so a failure in tier N does not require
debugging tier N+M of unrelated subsystems.

### Tier 1 -- LED / IO only (safe to flash first)

Apps: `blink`, `blink_hal`, `threadx_blink`,
`threadx_mpu_partition_demo`.

Pass criteria:
- LED1 (P602 on the EK-RA8D2 user-LED bank) toggles at the cadence
  hard-coded in each `main.c`.
- `threadx_mpu_partition_demo` additionally asserts no SecureFault
  before the LED starts toggling (it exercises the SAU bring-up in
  `trustzone_init.c`).

If it fails, look for:
- PFS unlock sequence regression (CLAUDE.md "PFS write sequence
  corrected to follow HUM Ch 20.2.4" -- if PMR-clear-first is not
  honored the GPIO write is silently dropped).
- IOPORT MSTPCR not cleared by the BSP `ra_board_init`.
- For `threadx_mpu_partition_demo`, a SecureFault before the first
  LED toggle implies SAU region overlap with the
  `linker_script.ld` NS_MRAM / NS_SRAM ranges.

### Tier 2 -- UART

Apps: `uart_hello` (already validated), `threadx_filex_demo`,
`threadx_levelx_demo`.

Pass criteria:
- VCOM emits the per-app banner at 115200-8N1 within 200 ms of
  reset.
- FileX / LevelX demos print a successful mount + read-back of the
  in-memory test partition.

If it fails, look for:
- SCI_B clear/drain regression (CLAUDE.md "SCI clear/drain"
  rebuild) -- garbled first character usually means TCLK was not
  stable when TIE was set.
- For FileX/LevelX, missing `ra_flash` blank-check on the simulated
  region; check the `ra_flash` page-cross / boundary tests are
  still green on the host.

### Tier 3 -- USB device (won't brick)

Apps: `usb_hid_device`, `usb_cdc_echo`, `threadx_usbx_cdc_demo`.

Pass criteria:
- macOS / Linux host enumerates the device with the VID/PID compiled
  in.
- HID: arrow-key reports observable via `evtest` / equivalent.
- CDC: round-trip echo on `/dev/tty.usbmodem*` matches input.

If it fails, look for:
- USBHS PHY CGC / MSTP wiring -- the CHANGELOG notes "USBHS init was
  promoted into `ra_board_ek_ra8d2`"; a missing call into the BSP
  is the first thing to grep for in `main.c`.
- For USBX-backed apps, ThreadX USBX byte-pool exhaustion if the
  pool was sized for the host build only.

### Tier 4 -- USB host

Apps: `usb_host_cdc_echo`, `usb_host_keyboard`, `usb_host_msc_browse`.

Pass criteria:
- Plugging a known-good gadget peer (USB keyboard, USB CDC ACM
  device, USB mass-storage stick) results in successful enumeration
  printed on the VCOM.

If it fails, look for:
- VBUS supply on the host port -- the EK-RA8D2 USB host port needs
  the on-board VBUS switch enabled by `ra_board_ek_ra8d2` BSP.
- USBX host class registration order in `main.c`.

### Tier 5 -- Ethernet (needs LAN)

Apps: `ethernet_tcp_echo`, `threadx_lwip_tcp_echo`,
`threadx_netx_tcp_echo`, `threadx_https_client`, `ptp_master`.

Pass criteria:
- DHCP lease obtained (or static IP banner printed).
- TCP echo: `nc <ip> <port>` round-trips bytes.
- HTTPS client: TF-PSA-Crypto handshake completes against a known
  server (test against a local nginx with a known cert).
- `ptp_master`: gPTP announce / sync messages observable on the
  wire with `tcpdump -i <if> ether proto 0x88f7`.

If it fails, look for:
- ESWM sub-driver wiring -- the CHANGELOG notes the `ra_eth` split
  into `swm` / `mfwd` / `coma` / `gwca` / `gptp`; a single
  uninitialized sub-driver presents as "link up, no packets".
- RGMII pinmux in the BSP (`ra_board_ek_ra8d2` ethernet RGMII
  helpers).

### Tier 6 -- BLE (needs nRF52840 sniffer)

Apps: `ble_peripheral`, `threadx_nimble_peripheral`,
`threadx_ble_central`, `threadx_ble_mesh_node`.

**Blocked on the BLE patch image** (see Phase 7.2 below). Without
the patch, `ra_ble_patch_load` returns `k_ra_err_not_supported` and
the radio does not transmit.

Pass criteria (assuming patch is loaded):
- Sniffer (nRF52840 with Wireshark + nRF Sniffer plugin) sees
  advertising PDUs at the configured interval.
- Central / mesh demos complete pairing or a provisioning round.

If it fails, look for:
- `ra_ble_patch.c` returning `k_ra_err_not_supported` -- the radio
  is silent until a real image is loaded.
- NimBLE host / controller HCI transport mismatch.

### Tier 7 -- Display (needs MIPI mezzanine + RGB888 panel)

Apps: `lcd_demo`, `ereader`, `threadx_guix_demo`.

**Blocked on Phase 7.3** -- the panel timing is placeholder; see
Phase 7.3 below for the unconfirmed-parameter list.

Pass criteria (assuming Phase 7.3 numbers are confirmed):
- `lcd_demo`: solid-color fill visible on the panel.
- `ereader`: paginated EPUB renders via the LiteHTML port +
  IT8951 e-paper driver.
- `threadx_guix_demo`: GUIX widgets visible and responsive to
  GT911 touch.

If it fails, look for:
- The placeholder line rate (480 Mbps/lane) versus the actual panel
  spec -- a mismatched PMUL / NMUL solve produces a rolling /
  no-sync image.
- DCS init sequence -- the BSP comment notes "the per-panel DCS
  command sequence ... is not committed here".

### Tier 8 -- Audio (needs DA7212 codec breakout + speaker)

Apps: `audio_loopback`, `usb_audio_device`.

Pass criteria:
- SSIE0 loopback: input audio appears at the speaker output with
  acceptable latency.
- USB audio device: host OS enumerates a UAC1 device, speaker test
  produces tone.

If it fails, look for:
- DA7212 I2C-control bring-up (codec config writes via `ra_iic_b`
  before SSIE0 is enabled).
- SSIE0 sample-block wiring in the BSP audio init path.

### Tier 9 -- Motor (needs custom 3-phase carrier; flagged TODO)

Apps: `motor_3phase`.

Status: **bring-up blocked**. The EK-RA8D2 does not ship with a
3-phase inverter; a custom carrier is required. Treat as
out-of-scope for Phase 7 unless a carrier appears.

### Tier 10 -- Advanced

Apps: `threadx_canfd_demo` (needs CAN transceiver + bus partner),
`threadx_ota_demo`, `threadx_sdcard_demo`, `ra_bootloader`,
`threadx_filex_levelx_demo`.

Pass criteria:
- CAN-FD: classic + FD frames exchanged with a peer node at the
  configured bit rates.
- `threadx_sdcard_demo`: SDHI mounts a FAT32 SD card and reads /
  writes a test file.
- `ra_bootloader` + `threadx_ota_demo`: A/B image swap completes
  through the OTA orchestration with the secure-side commit veneer
  (see CHANGELOG "ota: Phase-5 OTA orchestration").
- `threadx_filex_levelx_demo`: combined FileX-on-LevelX stack
  mounts, writes, remounts cleanly.

If it fails, look for:
- CANFD: `CFDCNCFG` bit positions / widths (CHANGELOG fix); a
  miscalculated TQ / sample point silently produces all-error
  frames.
- SDHI: 32-bit register access (CHANGELOG "SDHI switched to 32-bit
  registers with full layout"); 16-bit access reads zero on real
  silicon.

---

## 3. Vendor-blob integration (Phase 7.2)

See [`docs/VENDOR_BLOBS.md`](VENDOR_BLOBS.md) for the full
discussion. Quick reminder:

- **RSIP-E50D firmware blobs**: drop into
  `libs/third_party/renesas-rsip-blobs/` (gitignored, never
  committed). Affects `ra_rsip_*_install_plain`, `ra_rsip_key_wrap`,
  `ra_rsip_key_unwrap`, and the `secure_app/key_import.{c,h}`
  veneers. Without them, the `RA_RSIP_SOFTWARE_BACKEND` path runs
  and produces results that are verifiable but not
  cryptographically Renesas-signed.
- **BLE encrypted patch image**: drop into
  `libs/third_party/renesas-ble-patch/` (gitignored). Without it,
  `ra_ble_patch_load` returns `k_ra_err_not_supported` and the BLE
  radio does not transmit -- gates Tier 6 above.

Both blobs come from a Renesas FAE / RA SDK distribution / OEM
key-wrap service. We deliberately do not publish URLs or NDA terms
in this tree.

---

## 4. MIPI panel datasheet confirmation (Phase 7.3)

The BSP's MIPI bring-up in
[`libs/ra_board_ek_ra8d2/src/ra_board_ek_ra8d2.c`](../libs/ra_board_ek_ra8d2/src/ra_board_ek_ra8d2.c)
carries seven `TODO(panel-datasheet)` markers. The reference
documents to consult are the **Renesas MIPI Graphics Expansion
Board** datasheet (RTKMIPILCDB00000BE) and the **Focus-LCD
E45RA-MW276-C** panel datasheet. Unconfirmed parameters:

| Marker (line) | What needs the datasheet |
|---|---|
| `s_mipi_panel_cfg.timing` (line 803) | `CLSTPTSETR` / `LPTRNSTSETR` guard-band timing block. |
| `s_mipi_panel_cfg.timeouts` (line 804) | `HSTXTOSETR`, `LRXHTOSETR`, `TATOSETR`, `PRESPTO*SETR` bus timeouts. |
| `k_ra_board_mipi_panel_h_active` / `_v_active` / `_line_rate_mbps` (lines 772-776) | Confirm 480 x 854 active geometry and pin the per-lane bit rate (placeholder is 480 Mbps/lane). |
| `s_mipi_phy_timing_placeholder` (line 819) | Replace single-`TINIT` placeholder with a `ra_mipi_phy_select_timing` lookup keyed on the confirmed line rate. |
| `s_mipi_phy_cfg.pclka_mhz` / `.line_rate_mbps` (line 837 ff.) | Re-solve PLL coefficients once the actual MOSC frequency on the EK-RA8D2 board is confirmed; today's `pclka_mhz=60` assumes the chip's CGC reset default. |
| `pll.nmul_int = 48U` (line 847) | Re-derive once line rate is locked. |
| Per-panel DCS init sequence (line 877) | Sleep-out / pixel-format set / display-on commands for the Focus E45RA-MW276-C; currently owned by the application, no canonical sequence committed. |

Cross-references: HUM Ch 64 (MIPI PHY), Ch 65 (MIPI DSI host link
layer), and Renesas FSP `r_mipi_dsi` / `r_mipi_phy` source as
*reference only* per the no-FSP-in-tree policy.

---

## 5. Future enhancements (NOT scoped -- speculative only)

These are **not** Phase 7 deliverables. They are tracked here only so
they aren't lost; pull them into a real phase only if a concrete
deployment use-case appears.

- `ra_logging_remote` -- syslog-over-UDP sink for `ra_log`, once a
  deployment use-case appears that needs off-device log aggregation.
- `ra_coap_client` / MQTT client -- IoT-scenario libraries layered on
  top of the existing `ra_net_pal` + lwIP / NetX stacks.
- Multi-instance `ra_board_*` -- if a second Renesas RA board enters
  the tree (e.g. EK-RA8M3), generalize `ra_board_ek_ra8d2` so the
  examples can target either board via a build flag rather than a
  fork.
