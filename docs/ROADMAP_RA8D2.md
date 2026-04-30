# RA8D2 Roadmap -- What We Need to Change

Audit date: 2026-04-30 (post-cleanup, post third-party-strategy
decision).

This document is the **actionable plan** to close the gap between our
HAL and what FSP ships for RA8D2 specifically. Items in `docs/MISSING.md`
that are tagged "genuinely impossible" (closed-source Renesas firmware
blobs, board-manual pin-muxing) are out of scope for this roadmap --
they require Renesas business decisions, not engineering decisions.

The roadmap is ordered by **impact on real hardware behaviour**: the
first phase makes features go from "broken on silicon" to "working on
silicon"; the last phase is library polish.

---

## Strategy: when to write native vs adopt 3rd party

We are NOT trying to hand-roll everything. Past sweeps wrote
hand-rolled minimal versions of TCP/IP / FAT / TLS / BLE-host stacks
because we didn't have a strategy yet -- they're useful as reference,
but for production we adopt the proven 3rd-party libraries below.

| Layer | Strategy |
|---|---|
| **RTOS** | **Eclipse ThreadX** (formerly Azure RTOS). User-chosen. |
| **TCP/IP** | **NetX Duo** (ships with ThreadX, designed to integrate). lwIP is the alternate if we ever leave ThreadX. |
| **TLS** | **NetX Crypto + NetX Secure** (ThreadX-native), with **mbedTLS** as an alternate ALT-provider option. |
| **Filesystem** | **FileX** (ThreadX-native, ships with). Alternate: FatFs (Elm Chan) for non-ThreadX builds. |
| **GUI** | **GUIX** (ThreadX-native) on top of `libs/ra_gfx` + `ra_drw`. |
| **USB** | Keep our existing native `libs/ra_hal/src/ra_usb*.c` for evaluation and the small-footprint case. **USBX** (ThreadX-native) is the production option once ThreadX lands. |
| **BLE host stack** | Replace our `libs/ra_ble_host` with **Apache NimBLE** or **Zephyr Bluetooth host** (both production-grade, both portable, both have GATT client + bonding + Mesh). Renesas does not ship a competitive BLE host stack. |
| **Crypto block driver** | Native `libs/ra_hal/src/ra_rsip.c` driving the RSIP-E50D registers (datasheet-only). No 3rd-party crypto library at this layer; mbedTLS / NetX Crypto sit ABOVE this. |
| **HAL** | All native. The HAL is the project's reason to exist. |

The native libraries we already have (`libs/ra_net`, `libs/ra_fs`,
`libs/ra_gfx`, `libs/ra_ble_host`) stay in-tree as a no-RTOS fallback
path so apps can still build without ThreadX. Production apps move
to the 3rd-party stacks.

---

## Phase 0 -- Stand up ThreadX

This is foundational. NetX Duo, FileX, GUIX, USBX, NetX Crypto / NetX
Secure are all designed to integrate with the ThreadX scheduler and
ThreadX-aware buffer pools. Pick this first; everything below is
easier afterwards.

**Action.**
- Vendor Eclipse ThreadX under `libs/third_party/threadx/`. Source:
  https://github.com/eclipse-threadx/threadx (Eclipse Public License
  2.0; commercially viable).
- Add a port for Cortex-M85 (the M85 port may not exist yet --
  ThreadX has Cortex-M7 and Cortex-M33 ports; M85 may need either a
  new port or to be approximated by the M7 port + Helium init).
- Add `cmake/threadx.cmake` to expose the library to all `examples/`
  with an `RA_USE_THREADX` build option (default OFF).
- Wire SysTick to the ThreadX timer source.
- Add `examples/threadx_blink/`: two ThreadX threads, one blinks LED1
  at 1 Hz, the other LED2 at 0.5 Hz. Validates the scheduler works on
  EK-RA8D2.
- Add a `tx_user.h` configuration header with stack sizes / priorities
  / mutex counts tuned for RA8D2 SRAM budget.

**Acceptance.**
- `examples/threadx_blink/` builds + runs on EK-RA8D2.
- Existing bare-metal examples still build with `RA_USE_THREADX=OFF`.

**Estimate.** 2 sweeps (vendor + port + example). The Cortex-M85 port
itself may be the bottleneck.

---

## Phase 1 -- Make the hardware actually work for the things we claim

### 1.1 Drive the RSIP-E50D registers (native, no 3rd party)

**Problem.** `libs/ra_hal/src/ra_rsip.c` exposes the FSP-shaped public
API but the body is a software xorshift64* mixer. Real silicon will
compute the wrong bytes. mbedTLS / NetX Crypto sit ABOVE this layer
and need it to be real.

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
    region via the OEM-install command sequence.
  - `ra_rsip_aes_cipher`, `_aes_gcm`, `_aes_ccm` -- issue the AES
    command + drive DATA0 / DATA1 FIFOs + poll STAT.DONE.
  - `ra_rsip_sha256` -- one-shot SHA path on the RSIP block.
  - `ra_rsip_trng_read` -- drain the TRNG entropy pool register.
- Add **incremental** SHA-256 + HMAC-SHA-256 (`_init / _update /
  _final`) -- TLS handshake transcript hashing needs incremental
  hashing, currently we only expose one-shot.
- Keep the existing simulator path behind
  `#ifdef RA_RSIP_SOFTWARE_BACKEND` so host tests still run.

**Acceptance.**
- New tests assert `RSIPCMD = expected_opcode` for each call.
- A hardware bring-up test on EK-RA8D2 (gated under `tests/hw/`,
  not in CI) shows known-answer-test (KAT) vectors from FIPS-197
  (AES) and FIPS-180 (SHA-256) match the bytes our code computes.
- Simulator tests continue to pass via the software backend.

**Estimate.** 1 sweep + 1 hardware bring-up session.

**Note.** OEM-provisioned keys / attestation / Renesas-managed key
infrastructure require the closed-source `hw_sce_*.c` blobs and are
explicitly out of scope. Driving raw AES-GCM with a software-loaded
key IS achievable from datasheet-only material -- that's a real
RSIP primitive call.

---

### 1.2 Pull in mbedTLS (or NetX Secure) for the TLS layer

**Problem.** `libs/ra_tls` was removed in this cleanup because it
depended on the software-stub `ra_sce`. We have no TLS stack today.

**Recommendation.** mbedTLS via its ALT-provider hooks, with the AES
and SHA primitives backed by `ra_rsip` from Phase 1.1. mbedTLS has
been a battle-tested embedded TLS for over a decade and supports
arbitrary cipher-suite agility we'd never reach by hand-rolling.

If we go all-in on ThreadX, **NetX Secure** is the alternate -- it
integrates more cleanly with NetX Duo (Phase 4.1) and takes its
crypto from NetX Crypto. NetX Secure ships with ThreadX so there's
no separate vendoring cost.

**Action (mbedTLS path).**
- Vendor mbedTLS under `libs/third_party/mbedtls/`. Source:
  https://github.com/Mbed-TLS/mbedtls (Apache 2.0).
- Write `port/mbedtls_aes_alt.c` and `port/mbedtls_sha256_alt.c`
  shims that route mbedTLS's `aes` and `sha256` modules through
  `ra_rsip_aes_*` and `ra_rsip_sha256_*`.
- Configure mbedTLS via `mbedtls_config.h`:
  enable TLS 1.2 + TLS 1.3, disable PSA crypto for now, enable
  ECDHE-ECDSA + ECDHE-RSA + AES-128-GCM cipher suites.
- Replace the removed `examples/tls_https_client/` (call it back into
  existence): connect to `https://www.example.com`, validate cert
  chain, GET `/`, print body to SCI8.

**Action (NetX Secure path -- if going ThreadX-native).**
- After Phase 0, `libs/third_party/netxduo/` already has NetX Secure
  as a sub-component.
- Write `nx_secure_aes_alt.c` shims same as above.
- Demo same `examples/tls_https_client/` but using NetX Secure
  sockets instead of mbedTLS.

**Acceptance.**
- `examples/tls_https_client/` connects to https://www.example.com
  on real EK-RA8D2 hardware, gets a 200, prints the body.
- Unit tests cover the `*_alt.c` shims (input/output bytes match
  software fallback when ra_rsip is in software backend).

**Estimate.** 2 sweeps (vendor + ALT shims + example).

---

### 1.3 BLE patch loader -- ship-or-document, then move to a real host stack

**Problem.** `libs/ra_hal/src/ra_ble.c` has a `internal_load_patch`
stub. The real RA8D2 BLE controller will not transmit on the radio
without a Renesas-supplied encrypted firmware patch image.
Separately, `libs/ra_ble_host/` is a minimal hand-rolled GATT server.

**Action (controller side).**
- Document the patch gap in `examples/ble_peripheral/README.md`:
  "Cannot transmit on the radio without `<patch.bin>`. Renesas
  distributes the patch under their FSP license; obtain via your
  Renesas distributor or use FSP."
- Add a `RA_BLE_PATCH_PATH` build option that, if set at compile
  time, includes the binary blob via objcopy.
- Add a runtime check: if the patch did not load, refuse to start
  advertising and return a clear error code.

**Action (host stack side).**
- Replace `libs/ra_ble_host/` with **Apache NimBLE** or **Zephyr
  Bluetooth** -- both are production-grade C BLE host stacks that
  expose the standard HCI interface our `ra_ble.c` already speaks.
- Recommendation: **Apache NimBLE**. It's smaller (15 KLOC vs 40+),
  has a straightforward HCI transport layer (we plug `ra_ble.c` in
  as the controller transport), and ships under Apache 2.0.
  Zephyr's Bluetooth host is more featureful (incl. Mesh) but pulls
  in Zephyr-isms that don't fit a non-Zephyr build.
- Write a `nimble/transport/ra_ble/src/ble_hci_ra_ble.c` adapter
  that bridges NimBLE's `ble_hci_trans_*` calls to our
  `ra_ble_hci_send_command` / `ra_ble_hci_send_acl_data` /
  attach-event-handler callbacks.
- Keep our minimal `libs/ra_ble_host/` for unit tests and bare-metal
  builds; production apps switch to NimBLE.
- Re-write `examples/ble_peripheral/` to advertise the same Battery
  Service via NimBLE's GATT API.

**Acceptance.**
- `examples/ble_peripheral/` runs on EK-RA8D2 (with patch image
  present); nRF Connect on phone shows "EK-RA8D2" and Battery
  Service.
- Phone successfully subscribes to Battery Level notifications.

**Estimate.** 3 sweeps (vendor NimBLE + transport adapter + example
re-write + hardware validation).

---

### 1.4 Hardware-validate the 15 unflashed example apps

**Problem.** Of 17 apps, only `uart_hello` and `clock_check` were
flashed. The other 15 compile clean but have never run on silicon.
Several have placeholder pin-mux tables.

**Action.**
- Commit the EK-RA8D2 v1 board manual to `docs/reference/`. (User
  obtains from Renesas product page.)
- Walk each of the 15 apps, replacing `/* TODO: confirm against
  EK-RA8D2 v1 manual */` markers with port/pin pairs verified
  against the manual's J57 / J64 / etc. connector tables.
- Flash each app, verify in its README's "Test recipe" section that
  the documented behaviour actually happens.
- For apps needing external hardware (ETH cable, USB stick, speaker,
  motor driver IC), document the setup in the README.

**Suggested order** (cheapest to validate first):
1. `blink`, `blink_hal` -- LED only, no external hardware
2. `usb_cdc_echo`, `usb_host_cdc_echo` -- USB cable to laptop
3. `usb_hid_device`, `usb_msc_device` -- same
4. `usb_host_keyboard`, `usb_host_msc_browse` -- USB peripheral
5. `lcd_demo` -- on-board TFT panel
6. `ethernet_tcp_echo` -- ETH cable + host with static IP
7. `ble_peripheral` -- needs patch image first (Phase 1.3)
8. `usb_audio_device`, `audio_loopback` -- USB audio peripheral
9. `motor_3phase` -- external motor driver IC
10. `ptp_master` -- second host running ptp4l

**Acceptance.** Each README's "Test recipe" actually works on real
EK-RA8D2.

**Estimate.** 2 sweeps for non-external-hardware apps; the rest as
the user has hardware available.

---

## Phase 2 -- Close partial-driver feature gaps

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

`libs/ra_hal/src/ra_mipi_dsi.c`. Wire `send_command_short / _long`
through the command-mode timing register set; ULPS already exists,
verify drive of clock + data lanes to LP-00 + wakeup-flag poll.

### 2.4 USB host HID get_report IN (0.5 sweep)

`libs/ra_hal/src/ra_usb_hhid.c`. Today the SETUP packet is sent but
the IN data phase is stubbed. Wire `ra_usb_host_setup_data_in` (add
controller-level helper if needed) to drain EP0 IN data into the
caller's buffer.

### 2.5 Flash suspend/resume + lock-bit (0.5 sweep)

`libs/ra_hal/src/ra_flash.c`. Add:
- `ra_flash_suspend()` / `_resume()` driving MENTRYR.PCKA pause/run.
- `ra_flash_lock_set(addr, lock_bits)` programming MRCBPROT0/1.

### 2.6 Small driver gaps (0.5-1 sweep total)

Pick ones a demo app actually needs:
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

These have correct register layouts but minimal public APIs. None
block common use cases, but each corresponds to a real RA8D2 silicon
block.

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

Each is roughly 0.5-1 sweep. **`ra_drw` is the biggest payoff** --
`lcd_demo` currently ignores the 2D accelerator and uses software
pixel pushing, which is wasteful.

---

## Phase 4 -- Adopt the 3rd-party stacks

These replace our minimal hand-rolled libraries with production
versions. Each requires Phase 0 (ThreadX) for the integrated path,
or can be adopted standalone with a thinner adapter.

### 4.1 NetX Duo for TCP/IP (replaces `libs/ra_net`)

**Why.** Ships with ThreadX. Full IPv4 + IPv6, DHCP, DNS, multicast,
fragmentation, retransmission, congestion control. Multiple
concurrent sockets. Designed for low-overhead embedded.

**Action.**
- Vendor NetX Duo under `libs/third_party/netxduo/`.
- Write `nx_ether_driver_ra_eth.c` -- a NetX Duo network driver
  callback that bridges NetX's frame-send / frame-receive to our
  `ra_eth_*` descriptor rings.
- Update `examples/ethernet_tcp_echo/` to use NetX Duo's `nx_tcp_*`
  API instead of our hand-rolled TCP FSM.

**Alternate (no ThreadX).** lwIP under `libs/third_party/lwip/` with
a `netif/ra_ethernetif.c` adapter. We already have a `libs/ra_net_pal`
PAL layer that suggests this was the original plan.

**Estimate.** 2 sweeps.

### 4.2 FileX for filesystem (replaces `libs/ra_fs`)

**Why.** Ships with ThreadX. Production-grade FAT12/16/32 + exFAT,
LFN, journaling, multi-mount. Designed for low-overhead embedded.

**Action.**
- Vendor FileX under `libs/third_party/filex/`.
- Write `fx_media_driver_ra_sdhi.c` -- FileX media driver that wraps
  our `ra_sdhi_read_block / write_block`.
- Add `examples/sdcard_filebrowser/` -- mount an SD card, list root
  directory, dump README.TXT to SCI8.

**Alternate (no ThreadX).** FatFs (Elm Chan) under
`libs/third_party/fatfs/` + the `disk_io.c` adapter on `ra_sdhi`.

**Estimate.** 2 sweeps.

### 4.3 GUIX for GUI (augments `libs/ra_gfx`)

**Why.** Ships with ThreadX. Production GUI framework with widgets,
animations, touch/keypad input, screen transitions, fonts. Sits on
top of `libs/ra_gfx` for the framebuffer + `ra_drw` for hardware
acceleration.

**Action.**
- Vendor GUIX under `libs/third_party/guix/`.
- Wire `gx_display_driver_*` to `libs/ra_gfx` (or directly to the
  GLCDC/DRW path for higher performance).
- Add `examples/guix_demo/` -- a window with a button + label that
  reacts to touch input (touch comes from ACMPHS comparator since
  RA8D2 has no CTSU; GUIX expects a digital touch event).

**Alternate (no ThreadX).** TouchGFX (commercial, ST-recommended) or
LVGL (MIT license, RTOS-agnostic).

**Estimate.** 3 sweeps (GUIX has a learning curve).

### 4.4 USBX for USB (alternative to native `libs/ra_hal/src/ra_usb*`)

**Why.** Ships with ThreadX. Production USB device + host with all
class drivers (CDC, HID, MSC, audio, video, hub, OTG). Replaces
our hand-rolled stack.

**Action.** OPTIONAL -- our native stack was the project's reason
to exist. If we move to USBX:
- Vendor USBX under `libs/third_party/usbx/`.
- Write `ux_dcd_ra_usb.c` (device controller driver) and
  `ux_hcd_ra_usb.c` (host controller driver) that bridge USBX's
  abstract calls to `ra_usb_*` register I/O.
- Re-write the USB example apps against USBX's class API.

**Recommendation.** Keep native USB. The native code is committed,
tested, and works in the simulator. USBX is the production option
once we have hardware-validation pain we want to outsource.

**Estimate.** 4 sweeps if we do it. 0 if we keep native.

### 4.5 Apache NimBLE for BLE host (replaces `libs/ra_ble_host`)

(Already detailed in Phase 1.3.)

---

## Phase 5 -- Capabilities ThreadX gives us "for free"

Once Phase 0 lands, these become attractive low-effort wins:

- **Threading + synchronization primitives** in apps that need them
  (motor control PID loops, audio DMA double-buffer ping-pong, etc.)
- **Mutex / semaphore / event-flag** primitives layered into
  `libs/ra_hal/` so callbacks can hand off to a worker thread.
- **MPU partitioning** for TrustZone secure / non-secure boundary
  enforcement.
- **OTA firmware update** -- a thread that downloads a new firmware
  image over NetX Duo + writes to the inactive bank via `ra_flash` +
  reboots into the new bank.
- **Bluetooth Mesh** on top of NimBLE.
- **Renesas QE-style configurator** -- a tool that emits clock-tree /
  pin-mux / IRQ vector / MSTP gating C from a JSON description, so
  app authors don't hand-roll register sequences.

Each of these is its own 1-3 sweep effort, scheduled when product
demand exists.

---

## What we deleted in the RA8D2 cleanup

For provenance: these placeholder drivers were removed because RA8D2
silicon does not carry the underlying peripheral. Each is verified
absent via the BSP feature header.

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

We will declare the project feature-complete when:

1. **Phase 0 lands**: ThreadX runs on EK-RA8D2; `examples/threadx_blink/`
   verified.
2. **Phase 1 lands**: ra_rsip drives real RSIP-E50D registers;
   mbedTLS or NetX Secure has a working TLS path; BLE patch loader
   is documented; all 17 example apps verified on EK-RA8D2 hardware.
3. **Phase 2 lands**: I3C / CANFD / MIPI-DSI / USB hhid / Flash
   partials filled.
4. **Phase 3 lands at least for `ra_drw`**: hardware-accelerated
   blit / fill so `lcd_demo` is not software-pushing pixels.
5. **Phase 4 has at least NetX Duo + FileX + NimBLE adopted**:
   Production-quality TCP/IP, filesystem, BLE host stack.

After that, someone using our HAL has feature parity with FSP for
everything RA8D2 silicon can do, except for closed-source Renesas
crypto / BLE blobs (genuinely impossible) and FSP's QE configurator
GUI tool (Phase 5 polish).
