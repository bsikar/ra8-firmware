# RA8D2 Roadmap -- What We Need to Change

Audit date: 2026-04-30 (post-cleanup).

This document is the **actionable plan** to close the gap between our
HAL and what FSP ships for RA8D2 specifically. Items in `docs/MISSING.md`
that are tagged "genuinely impossible" (closed-source Renesas firmware
blobs, board-manual pin-muxing) are out of scope for this roadmap --
they require Renesas business decisions, not engineering decisions.

The roadmap is ordered by **impact on real hardware behaviour**: the
first item makes a feature go from "broken on silicon" to "working on
silicon"; the last is documentation polish.

---

## Phase 1 -- Make the hardware actually work for the things we claim

### 1.1 Drive the RSIP-E50D registers

**Problem.** `libs/ra_hal/src/ra_rsip.c` exposes the FSP-shaped public
API but the body is a software xorshift64* mixer. Real silicon will
compute the wrong bytes.

**Action.**
- Add HUM Ch 67 register-level access for the RSIP-E50D block:
  - RSIPCMD command-issue register
  - RSIPSTAT status / done-flag register
  - RSIPDATA0..N data input/output FIFOs
  - RSIPKEY key-blob register
  - RSIPCTL control / mode-select register
- Replace the mixer body in:
  - `ra_rsip_aes128_install_plain`, `_aes192_install_plain`,
    `_aes256_install_plain` -- write the key into the wrapped-key
    region via the OEM-install command sequence
  - `ra_rsip_aes_cipher`, `_aes_gcm`, `_aes_ccm` -- issue the AES
    command + drive DATA0/DATA1 FIFOs + poll STAT.DONE
  - `ra_rsip_sha256` -- one-shot SHA path on the RSIP block
  - `ra_rsip_trng_read` -- drain the TRNG entropy pool register
- Keep the existing simulator path behind `#ifdef RA_RSIP_SOFTWARE_BACKEND`
  so host tests still run.

**Acceptance.**
- New tests assert `RSIPCMD = expected_opcode` for each call.
- A hardware bring-up test on EK-RA8D2 (not in CI; `tests/hw/` if we
  add one) shows known-answer-test (KAT) vectors from FIPS-197 (AES)
  and FIPS-180 (SHA-256) match the bytes our code computes.
- Simulator tests continue to pass via the software backend.

**Estimate.** 1 sweep (~25 min cadence) + 1 hardware bring-up session.

**Note.** OEM-provisioned keys / attestation / Renesas-managed key
infrastructure require the closed-source `hw_sce_*.c` blobs and are
explicitly out of scope. We can ship raw-AES-with-software-loaded-key
without them.

---

### 1.2 Decide on `libs/ra_tls` strategy

**Problem.** `libs/ra_tls` was removed because it depended on the
software-stub `ra_sce`. We have no TLS stack today.

**Options.**

A. **Re-implement on top of real ra_rsip** (after Phase 1.1 lands).
   Requires:
   - Adding incremental SHA-256 to `ra_rsip` (today only one-shot).
   - Adding incremental HMAC-SHA-256 to `ra_rsip`.
   - Real RSA encrypt for ClientKeyExchange premaster secret.
   - Drop the SHA-256-only cert-pinning shortcut; either implement
     full X.509 chain or keep cert-pin as a lightweight option.

B. **Integrate mbedTLS** as the crypto provider.
   Requires:
   - Adding `libs/third_party/mbedtls` (vendored) under
     `cmake/third_party.cmake`.
   - Writing a `mbedtls_aes_alt.c` / `mbedtls_sha256_alt.c` shim that
     routes mbedTLS API calls into `ra_rsip` for hardware acceleration.
   - Replacing the stubs in our removed `libs/ra_tls/` with a thin
     wrapper around `mbedtls_ssl_*`.

**Recommendation.** Option B. mbedTLS is the de-facto embedded TLS
choice, has cipher-suite agility we'd never match by hand-rolling, and
its ALT-provider hooks are exactly designed for this. Hand-rolling TLS
1.2 / 1.3 to spec compliance is a multi-month effort that mbedTLS has
already done.

**Acceptance.**
- `examples/tls_https_client/` connects to https://www.example.com,
  verifies the cert chain, GETs `/`, prints the body to SCI8.
- Unit tests cover the `*_alt.c` shims (input/output bytes match
  software fallback when ra_rsip is in software backend).

**Estimate.** 2 sweeps (vendor mbedTLS + write ALT shims + write the
example).

---

### 1.3 BLE patch loader -- ship-or-document

**Problem.** `libs/ra_hal/src/ra_ble.c` has a `internal_load_patch`
stub. The real RA8D2 BLE controller will not transmit on the radio
without a Renesas-supplied encrypted firmware patch image.

**Action.**
- Document the gap in `examples/ble_peripheral/README.md`: "Cannot
  transmit on the radio without `<patch.bin>`. Renesas distributes
  the patch under their FSP license; obtain via your Renesas
  distributor or use FSP."
- Add a `RA_BLE_PATCH_PATH` build option that, if set at compile
  time, includes the binary blob via `objcopy --binary-architecture`.
- Add a runtime check: if the patch did not load, refuse to start
  advertising and return a clear error code.
- L2CAP / ATT / GATT host stack remains usable for unit-test
  scenarios (they speak HCI to the controller; the controller is
  what's gated).

**Acceptance.**
- Build without RA_BLE_PATCH_PATH succeeds; runtime returns
  k_ra_err_not_initialized from `ra_ble_host_advertise_start`.
- Build with RA_BLE_PATCH_PATH succeeds; on hardware the patch
  loads (verified via SCI8 log) and advertising starts.

**Estimate.** 0.5 sweep (mostly docs + build-system glue).

---

### 1.4 Hardware-validate the 15 unflashed example apps

**Problem.** Of 17 apps, only `uart_hello` and `clock_check` were
flashed. The other 15 compile clean but have never run on silicon.
Several have placeholder pin-mux tables.

**Action.**
- Commit the EK-RA8D2 v1 board manual to `docs/reference/`. (User
  needs to add this; available from Renesas's product page.)
- Walk each of the 15 apps in order, replacing
  `/* TODO: confirm against EK-RA8D2 v1 manual */` markers with
  real port/pin pairs verified against the manual's J57 / J64 / etc.
  connector tables.
- Flash each app, verify in its README's "Test recipe" section that
  the documented behaviour actually happens.
- For apps that need external hardware (ETH cable, USB stick,
  speaker, motor driver IC), document the hardware setup in the
  README.

**Suggested order** (cheapest to validate first):
1. `blink`, `blink_hal` -- LED only, no external hardware
2. `usb_cdc_echo`, `usb_host_cdc_echo` -- USB cable to laptop
3. `usb_hid_device`, `usb_msc_device` -- same
4. `usb_host_keyboard`, `usb_host_msc_browse` -- USB peripheral + cable
5. `lcd_demo` -- on-board TFT panel
6. `ethernet_tcp_echo` -- ETH cable + host with static IP
7. `ble_peripheral` -- needs patch image first (Phase 1.3)
8. `usb_audio_device`, `audio_loopback` -- USB audio peripheral
9. `motor_3phase` -- external motor driver IC
10. `ptp_master` -- second host running ptp4l

**Acceptance.** Each README's "Test recipe" actually works on the
real EK-RA8D2.

**Estimate.** 2 sweeps for non-external-hardware apps; the rest as
the user has hardware available.

---

## Phase 2 -- Close the partial-driver feature gaps

These are RA8D2 peripherals we drive correctly but have only minimal
public APIs. Pick whichever the next demo app needs first.

### 2.1 I3C HDR-DDR + IBI (1 sweep)

`libs/ra_hal/src/ra_i3c.c`. Add:
- `ra_i3c_set_hdr_mode(target_addr, mode)` with HDR-DDR / HDR-TS
  encoding via NCMDQP commando-attribute bits.
- `ra_i3c_ibi_enable(target_addr)` programming NTIBIVCTL valid-count.
- `ra_i3c_ibi_drain(*ibi)` reading NTIBIQP queue, returning IBI
  payload + originating target.
- Slave-mode entry via BCTL.SLVE bit and NSDVAD slave-address program.

### 2.2 CANFD GAFL + BRS (1 sweep)

`libs/ra_hal/src/ra_canfd.c`. Add:
- `ra_canfd_filter_set(filter_id, accept_id, mask, dlc)` programming
  CFDGAFLID/CFDGAFLM/CFDGAFLP1.
- `ra_canfd_set_brs(channel, fast_bitrate)` for the bit-rate-switch
  payload phase.
- `ra_canfd_iso_mode(enable)` for ISO 11898-1 vs non-ISO.

### 2.3 MIPI-DSI command-mode + ULPS (1 sweep)

`libs/ra_hal/src/ra_mipi_dsi.c`. Add:
- `ra_mipi_dsi_send_command_short` / `_long` already exist (sweep 6);
  wire up command-mode timing register set.
- `ra_mipi_dsi_enter_ulps()` / `_exit_ulps()` already exist; verify
  PHY drives clock + data lanes to ULPS LP-00 state, polls the wakeup
  flag.

### 2.4 USB host HID get_report IN (0.5 sweep)

`libs/ra_hal/src/ra_usb_hhid.c`. Today the SETUP packet is sent but
the IN data phase is stubbed. Wire `ra_usb_host_setup_data_in` (or
add a controller-level helper if needed) to drain the EP0 IN data
into the caller's buffer.

### 2.5 Flash suspend/resume + lock-bit (0.5 sweep)

`libs/ra_hal/src/ra_flash.c`. Add:
- `ra_flash_suspend()` / `_resume()` driving MENTRYR.PCKA pause/run.
- `ra_flash_lock_set(addr, lock_bits)` programming MRCBPROT0/1.

### 2.6 Small driver gaps (0.5-1 sweep total)

Pick the ones a demo app actually needs:
- `ra_dac_b` batch-conversion mode
- `ra_acmphs` filter routing
- `ra_rtc` alarm B
- `ra_agt` pulse-width measurement
- `ra_ulpt` pulse-output mode
- `ra_lvd` reset-on-trip mode
- `ra_doc` chained comparator
- `ra_bkup` TZ secure-domain hand-off

---

## Phase 3 -- Promote scaffolds to feature-complete

These have correct register layouts but minimal public APIs. None block
common use cases, but each corresponds to a real RA8D2 silicon block.

| Driver | What's needed |
|---|---|
| `ra_drw` | Hardware blit / fill / line / glyph through D/AVE 2D queue |
| `ra_etha` | Per-port descriptor ring management for the GMAC switch fabric |
| `ra_vin` | Frame-end IRQ + dynamic capture window via VINSCR |
| `ra_tsn` | TSN scheduler + time-aware shaper + FRER tables |
| `ra_rmac` | Auto-neg state machine for the GbE PHY |
| `ra_ssie` | DMAC-paired iso transfer + FIFO-threshold IRQs |
| `ra_dotf` | Hardware tamper-protected flash mapping |
| `ra_cnecc` | ECC computation paths beyond status-only read |
| `ra_ipc` | Multi-core M85 <-> M33 mailbox dispatch glue |
| `ra_pdg` | Capture-start + completion callback |
| `ra_ceu` | DMAC-driven framebuffer fill |

Each is roughly 0.5-1 sweep depending on complexity. `ra_drw` is the
biggest payoff (lcd_demo currently ignores the 2D accelerator entirely
and uses software pixel pushing, which is wasteful).

---

## Phase 4 -- Library polish

### 4.1 `libs/ra_net` -> integrate lwIP

Hand-rolled minimal IPv4 is fine for `examples/ethernet_tcp_echo` but
real apps need DHCP + IPv6 + multi-connection TCP + retransmission +
etc. lwIP is the standard choice; we already have a `libs/ra_net_pal`
PAL layer. Vendor lwIP under `libs/third_party/lwip/`, write a
`netif/ethernetif.c` adapter on `ra_eth_*`, retire `libs/ra_net`'s TCP
in favor of lwIP.

### 4.2 `libs/ra_fs` -> add LFN + directory creation

Today FAT 8.3 short names only. Add Long File Name (LFN) entries
(directory entries with the 0x0F attribute byte) and `ra_fs_mkdir`.
Or vendor FatFs (Elm Chan's) which is the embedded de-facto.

### 4.3 `libs/ra_gfx` -> add DRW acceleration

Wire the `RA_GFX_USE_DRW` path so `ra_gfx_rect`, `ra_gfx_blit`, and
`ra_gfx_text_out` route through `ra_drw`'s hardware blitter. Software
fallback stays for the host-test build.

### 4.4 `libs/ra_ble_host` -> add bonding + GATT client

Today it's a server-only stack. Add:
- Pairing / bonding (Security Manager).
- GATT client API (Discover-services, read-by-uuid, write-with-resp,
  subscribe-notify).
- Bluetooth Mesh would be Phase 5.

---

## Phase 5 -- New capabilities (no FSP equivalent we're missing)

These are Renesas-shipped features that currently have no peer in our
tree. Each is a multi-sweep effort, only do them when a real product
demands them.

- **RTOS port glue.** FreeRTOS or ThreadX integration: scheduler
  hooks, MPU partitioning, IRQ priority tables, task-context save.
- **GUIX-style widget framework** on top of `libs/ra_gfx`.
- **Bluetooth Mesh profile** on top of `libs/ra_ble_host`.
- **OTA firmware update** on top of `libs/ra_flash` (dual-bank) and
  `libs/ra_net` (download path).
- **Renesas QE-style configurator** -- a tool that emits clock-tree /
  pin-mux / IRQ vector / MSTP gating C from a JSON description, so
  app authors don't hand-roll register sequences.

---

## What we deleted in the RA8D2 cleanup

For provenance: these placeholder drivers were removed because
RA8D2 silicon does not carry the underlying peripheral. Each is
verified absent via the BSP feature header.

| Removed driver | Reason | BSP flag |
|---|---|---|
| `ra_acmphs_b` | RA8D2 has ACMPHS, not the _B variant | `ACMPHS_IS_AVAILABLE=1`, no `_B_` |
| `ra_acmplp` | low-power comparator absent | `ACMPLP_IS_AVAILABLE=0` |
| `ra_adc_d` | differential ADC absent | `ADC_D_IS_AVAILABLE=0` |
| `ra_can` | classical CAN absent (RA8D2 has CANFD) | `CAN_IS_AVAILABLE=0` |
| `ra_cec` | HDMI-CEC block absent | `CEC_IS_AVAILABLE=0` |
| `ra_ctsu` | capacitive touch sensing absent | `CTSU_IS_AVAILABLE=0` |
| `ra_dac` | legacy 12-bit DAC absent (RA8D2 has DAC_B) | `DAC12_IS_AVAILABLE=0` |
| `ra_dac8` | 8-bit DAC absent | `DAC8_IS_AVAILABLE=0` |
| `ra_dsmif` | delta-sigma demod absent | not in BSP |
| `ra_ethercat_phy` | EtherCAT slave absent | `ESC_IS_AVAILABLE=0` |
| `ra_flash_lp` | low-power flash absent (RA8D2 uses MRAM) | `FLASH_LP_IS_AVAILABLE=0` |
| `ra_iic` | legacy IIC absent (RA8D2 has IIC_B) | `IIC_IS_AVAILABLE=1` (`_B` variant) |
| `ra_iica_master`, `ra_iica_slave` | yet-another IIC variant absent | `SAU_IS_AVAILABLE=0` |
| `ra_iirfa` | IIR Filter Accelerator absent | `TFU_IS_AVAILABLE=0` |
| `ra_kint` | Key Interrupt matrix absent | `KINT_IS_AVAILABLE=0` |
| `ra_opamp` | on-chip op-amps absent | `OPAMP_IS_AVAILABLE=0` |
| `ra_pdc` | PDC parallel-data-capture absent | not in BSP (RA8D2 uses VIN) |
| `ra_qspi` | legacy QSPI absent (RA8D2 has OSPI_B) | `QSPI_IS_AVAILABLE=0` |
| `ra_rtc_c` | calendar-mode-only RTC variant absent | RA8D2 has full ra_rtc |
| `ra_sau_*` | SAU sub-protocol family absent | `SAU_IS_AVAILABLE=0` |
| `ra_sce`, `ra_sce_protected`, `ra_sce_key_injection` | SCE family absent (RA8D2 has RSIP-E50D) | all `SCE*_SUPPORTED=0` |
| `ra_slcdc` | segment LCD absent | `SLCDC_IS_AVAILABLE=0` |
| `ra_tau`, `ra_tau_pwm`, `ra_tml` | Timer Array Unit family absent | `TAU_IS_AVAILABLE=0`, `TML_IS_AVAILABLE=0` |
| `ra_uarta` | UARTA pre-SCI variant absent | `UARTA_IS_AVAILABLE=0` |
| `ra_usb_typec` | USB-C / PD block absent | `USB_HAS_TYPEC=0` |
| `examples/cap_touch_demo/` | depended on ra_ctsu | -- |
| `libs/ra_tls` | depended on removed `ra_sce` software stubs | -- |

This cleanup dropped 33 driver source files + tests. The remaining
**126** driver source files all map to peripherals confirmed present
on RA8D2 silicon.

---

## Acceptance for "RA8D2 work is done"

We will declare the RA8D2 HAL feature-complete when:

1. Phase 1 lands: ra_rsip drives real RSIP-E50D registers; TLS path
   works (via mbedTLS + ALT shims); BLE patch loader is documented
   even if the patch image is not shipped; all 17 example apps verified
   on EK-RA8D2 hardware.
2. Phase 2 lands: I3C / CANFD / MIPI-DSI / USB hhid / Flash partials
   filled.
3. The 12 scaffolds in Phase 3 are at least at "feature-complete for
   the apps in `examples/`" level (some scaffolds have no demand
   today and can stay as scaffolds).

After that, we are at a point where someone using our HAL has feature
parity with FSP for everything RA8D2 silicon can do, except for:
- Closed-source Renesas crypto/BLE blobs (genuinely impossible)
- RTOS / GUI / TLS framework integrations that FSP ships and we don't,
  unless we explicitly Phase 4/5 them.
