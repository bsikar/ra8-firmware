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

We are NOT trying to hand-roll everything. Past sweeps wrote minimal
versions of TCP/IP / FAT / TLS / BLE-host / GUI because we didn't have
a strategy yet. The strategy now is: **go all-in on the Eclipse ThreadX
X-Ware platform** for everything ThreadX provides, plus Apache NimBLE
for BLE host. Hand-rolled minimal libraries stay in-tree as a no-RTOS
fallback path so apps can still build without ThreadX.

| Layer | Strategy | Source |
|---|---|---|
| **RTOS** | **Eclipse ThreadX** | github.com/eclipse-threadx/threadx |
| **TCP/IP** | **NetX Duo** -- replaces `libs/ra_net` | github.com/eclipse-threadx/netxduo |
| **TLS** | **NetX Secure + NetX Crypto** -- ALT-shimmed into `ra_rsip` for hardware AES/SHA | (bundled in netxduo) |
| **Filesystem** | **FileX** -- replaces `libs/ra_fs` | github.com/eclipse-threadx/filex |
| **GUI** | **GUIX** -- on top of `libs/ra_gfx` + `ra_drw` | github.com/eclipse-threadx/guix |
| **USB** | **USBX** -- replaces `libs/ra_hal/src/ra_usb_*cdc/hid/msc/audio*.c` class layers | github.com/eclipse-threadx/usbx |
| **Flash wear leveling** | **LevelX** -- for the OSPI external flash | github.com/eclipse-threadx/levelx |
| **BLE host stack** | **Apache NimBLE** -- replaces `libs/ra_ble_host` | github.com/apache/mynewt-nimble |
| **Crypto block driver** | Native `libs/ra_hal/src/ra_rsip.c` driving RSIP-E50D registers (datasheet-only) | -- |
| **USB controller driver** | Native `libs/ra_hal/src/ra_usb.c` (ra_usb stays; USBX class layers replace `ra_usb_*cdc/hid/msc/audio`) | -- |
| **HAL** | All native. The HAL is the project's reason to exist. | -- |

The native ThreadX-replaceable libraries (`libs/ra_net`, `libs/ra_fs`,
`libs/ra_gfx`, `libs/ra_ble_host`, `libs/ra_hal/src/ra_usb_*cdc/hid/msc*.c`)
stay in-tree as a no-RTOS fallback path so apps can still build without
ThreadX. Production apps switch to the 3rd-party stacks via build
options.

### Why "all-in on ThreadX X-Ware":

- The X-Ware components are designed to integrate. NetX Duo expects
  ThreadX semaphores; FileX uses ThreadX mutexes; GUIX schedules its
  renderer thread; USBX uses ThreadX block pools for transfer
  buffers. No impedance mismatches.
- Single license (Apache 2.0), single coding style, single
  community.
- Renesas FSP already validates this combo on RA8 silicon -- we
  benefit from their bring-up work even though we don't use FSP
  itself, because the Cortex-M85 port and the integration patterns
  are public.

### Why NimBLE instead of NetX Bluetooth:

NetX Bluetooth (Microsoft's BLE host for ThreadX) exists but has far
less production mileage than NimBLE. NimBLE is the de-facto-standard
embedded BLE host stack; battle-tested across millions of devices
including all Espressif WiFi+BLE products. NimBLE's transport layer
plugs cleanly into our `ra_ble.c` HCI ring -- it doesn't require
ThreadX (we'd run NimBLE's tasks under ThreadX threads, but NimBLE
itself is RTOS-portable).

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

### 4.4 USBX for USB class layers (replaces `libs/ra_hal/src/ra_usb_*cdc/hid/msc/audio*.c`)

**Why.** Ships with ThreadX. Production USB device + host with all
class drivers (CDC, HID, MSC, audio, video, hub, OTG, printer,
vendor-defined). Adopting it means a single integrated stack
across all our X-Ware components. Keep `libs/ra_hal/src/ra_usb.c`
as the controller-layer driver underneath USBX.

**Action.**
- Vendor USBX under `libs/third_party/usbx/`.
- Write `ux_dcd_ra_usb.c` (device controller driver) and
  `ux_hcd_ra_usb.c` (host controller driver) that bridge USBX's
  abstract calls to `ra_usb_*` register I/O.
- Re-write the USB example apps against USBX's class API. The
  hand-rolled class layers (`ra_usb_cdc`, `ra_usb_phid`,
  `ra_usb_pmsc`, `ra_usb_paud`, `ra_usb_hcdc`, `ra_usb_hmsc`,
  `ra_usb_hhid`, `ra_usb_haud`, `ra_usb_hcdc_ecm`, `ra_usb_hhub`,
  `ra_usb_pprn`, `ra_usb_pvnd`, `ra_usb_composite`) stay in-tree
  for the no-ThreadX fallback path.

**Estimate.** 4 sweeps (DCD/HCD bridge + re-write 6+ example apps
against USBX class API).

### 4.5 LevelX for OSPI flash wear leveling

**Why.** Ships with ThreadX. Wear-leveling layer for raw NOR/NAND
flash. The EK-RA8D2 ships with 64 MB Octo-SPI flash on the board
(MX25LM512); without wear leveling the same blocks wear out under
a database / log / ereader-library workload.

**Action.**
- Vendor LevelX under `libs/third_party/levelx/`.
- Write `lx_nor_driver_ra_xspi.c` -- LevelX NOR driver that wraps
  our `ra_xspi_*` read/program/erase calls.
- Wire LevelX as the underlying block device for FileX -- so apps
  open a `fx_media` on the OSPI flash and get wear-leveled FAT
  storage transparently.

**Estimate.** 1 sweep.

### 4.6 NetX Crypto + NetX Secure for TLS

**Why.** Ships with NetX Duo. Replaces the deleted `libs/ra_tls`
plan with a fully integrated TLS stack. NetX Crypto provides the
algorithm primitives (AES, SHA, RSA, ECDH, etc.); NetX Secure is
the TLS state machine on top. ALT-shim NetX Crypto's AES + SHA
into our hardware `ra_rsip` for hardware acceleration; let it fall
back to software for everything else.

**Action.**
- (Already vendored as part of NetX Duo in Phase 4.1.)
- Write `nx_crypto_aes_alt.c` and `nx_crypto_sha256_alt.c` that
  route NetX Crypto's algorithm-table calls through `ra_rsip_*`.
- Add `examples/tls_https_client/` (re-create the deleted one):
  TLS 1.2 + 1.3 client to https://www.example.com over NetX Duo,
  GET `/`, dump body to SCI8.

**Estimate.** 2 sweeps.

### 4.7 Apache NimBLE for BLE host (replaces `libs/ra_ble_host`)

(Already detailed in Phase 1.3. NimBLE under
`libs/third_party/nimble/`; transport adapter in `port/nimble/`.)

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

## Phase 6 -- First product: ePub ereader (BOOX-style)

This phase scopes the user's first target product on top of the
RA8D2 + ThreadX X-Ware platform. An ePub ereader is roughly:

> ARM-side software stack: framebuffer + GUI + filesystem + epub
> parser + font renderer + CSS reflow engine + page-turn UX +
> battery management + sleep/wake + USB mass-storage for
> sideloading.

What we already have, what we're missing, and where each piece
plugs in:

### 6.1 What's already in place from earlier phases

| Need | Source |
|---|---|
| Display framebuffer (1024x600 RGB565 over GLCDC for prototype) | `libs/ra_hal/src/ra_glcdc.c` (sweep 2) |
| 2D acceleration (blit / fill / glyph) | `libs/ra_hal/src/ra_drw.c` (Phase 3) |
| File access | FileX (Phase 4.2) on top of FAT-formatted SD card via `ra_sdhi` |
| External flash for the user library | LevelX (Phase 4.5) on top of OSPI via `ra_xspi` |
| GUI widgets (book list, settings, reader view) | GUIX (Phase 4.3) |
| USB sideloading (host plugs EVM in, sees the ereader as a USB drive containing books) | USBX MSC device class (Phase 4.4) |
| Power management (sleep when not reading) | LPM driver from sweep 7 |
| Battery low warning | LVD driver from sweep 7 |
| Wake-on-button | ICU + GPIO from earlier sweeps |

### 6.2 What's missing -- new HAL or 3rd-party work

#### 6.2.1 E-Ink panel driver (`libs/ra_hal/src/ra_epaper.c`, NEW)

**Problem.** A real ereader uses an electrophoretic (E-Ink) panel,
not a TFT panel. E-Ink panels are typically driven over SPI through
a controller IC like **IT8951** (DKE/Waveshare) or directly via the
panel's SPI command stream (panels using JD9930 / EK79007 / etc.).

**For development on EK-RA8D2 itself**: prototype on the on-board
1024x600 parallel TFT. The GLCDC framebuffer rendering is identical;
only the panel driver layer differs.

**For a custom board**: write `ra_epaper` to drive the chosen panel.
Recommended starting point: **IT8951 over SPI** -- well-documented,
4096 grayscale levels, full A2 / GC16 / GC4 / DU waveform support
in the controller IC. Driver provides:
- `ra_epaper_init(spi_channel, busy_pin, reset_pin, hrdy_pin)` --
  bring up controller, query panel info (width / height / VCOM).
- `ra_epaper_load_image(x, y, w, h, buf, mode)` -- DMA-paste a
  framebuffer region into IT8951 image RAM.
- `ra_epaper_display_area(x, y, w, h, mode)` -- trigger waveform
  refresh. mode: `k_epaper_mode_init / a2 / gc16 / gc4 / du`.
- `ra_epaper_sleep()` / `_wake()` -- VCOM rail down for deep sleep.

**Estimate.** 2 sweeps.

#### 6.2.2 Touch input

**Problem.** RA8D2 has NO CTSU (capacitive touch sensing unit).
Touch on a custom ereader board is typically a **separate touch
controller IC** -- e.g. GoodIX **GT911**, FocalTech **FT5x06**, or
Cypress CY8CTMA -- talking to the MCU over IIC_B + an interrupt
GPIO.

**Action.** Add `libs/ra_hal/src/ra_touch.c` -- a high-level touch
driver (multi-touch protocol) sitting on `ra_iic_b`. Pluggable
backend per controller IC (GT911 first, FT5x06 second). Routes
touch events into GUIX's input queue.

**Estimate.** 1 sweep (GT911 backend) + 0.5 sweep per additional
backend.

#### 6.2.3 ePub container parser

**Problem.** ePub is a ZIP archive. Inside: `META-INF/container.xml`
points to the OPF (Open Packaging Format) document, which lists
spine items (XHTML chapter files), manifest items, and the table of
contents (NCX or XHTML nav). Need ZIP + XML parsing.

**Action.** 3rd-party libraries:
- **miniz** (single-header ZIP under MIT) -- vendored under
  `libs/third_party/miniz/`. Used to read the .epub container.
- **TinyXML2** (single-header XML under zlib license) -- vendored
  under `libs/third_party/tinyxml2/`. Used to parse OPF + NCX +
  XHTML structure.
- Write `libs/ra_epub/` -- a thin domain layer on top of miniz +
  tinyxml2 that exposes:
  - `ra_epub_open(media, path, *out_book)` -- crack the .epub,
    read OPF, build chapter list.
  - `ra_epub_get_chapter_count(book)`.
  - `ra_epub_load_chapter(book, idx, *out_xhtml_buf, *out_len)` --
    extract one chapter's XHTML into RAM.
  - `ra_epub_close(book)`.
  - `ra_epub_get_metadata(book, *out_meta)` -- title, author,
    cover image, language.

**Estimate.** 2 sweeps.

#### 6.2.4 HTML/CSS reflow engine

**Problem.** XHTML chapters need to flow text within a page
viewport, respecting CSS box model + line breaking + text styling
(bold / italic / heading sizes). This is the hardest part of an
ereader.

**Options.**

A. **LVGL's `lv_html`** -- LVGL has an HTML rendering widget but
   it's not full reflow.

B. **LiteHTML** (BSD-licensed full HTML+CSS rendering library,
   used by Sumatra PDF and Notepad++) -- ~50 KLOC, supports flex
   reflow and CSS3 selectors. Heavyweight for an MCU but it does
   the job. Vendor under `libs/third_party/litehtml/`.

C. **Hand-rolled minimal**: parse a subset of XHTML (`<p>`,
   `<h1>..<h6>`, `<em>`, `<strong>`, `<br>`, `<img>`, `<ul>`/`<ol>`/
   `<li>`, `<blockquote>`); ignore CSS positioning beyond
   font-size + bold/italic + text-align. Greedy line-break by
   measuring glyph widths. ~3-5 KLOC.

**Recommendation.** Start with option C (hand-rolled) for the
prototype, swap in LiteHTML if reflow fidelity is unsatisfactory.
The hand-rolled approach renders 90% of public-domain ePubs
acceptably (Project Gutenberg books are mostly simple).

**Estimate.** 3 sweeps for hand-rolled; 4-5 sweeps for LiteHTML
integration.

#### 6.2.5 Font rendering

**Problem.** Need to rasterise TrueType / OpenType glyphs at
arbitrary sizes (10pt body text, 24pt chapter heading, etc.) into
the framebuffer.

**Options.**
- **stb_truetype.h** (single-header, public-domain, ~5 KLOC) --
  supports rendering, kerning, hinting. Used by countless
  embedded apps.
- **FreeType** (mature, larger, BSD-licensed) -- the gold standard,
  but ~50 KLOC and overkill for a few fonts.

**Recommendation.** stb_truetype.h. Vendor under
`libs/third_party/stb/`. Pre-bake the system fonts (e.g. Noto Serif
+ Noto Sans, free Google fonts) into the firmware at compile time
or load from FileX.

**Estimate.** 1 sweep.

#### 6.2.6 Image decoding

**Problem.** Book covers and embedded illustrations are usually
JPEG or PNG. ePub spec also allows GIF, SVG, and WebP.

**Action.**
- **JPEG**: we already have `libs/ra_hal/src/ra_jpeg_sw.c` (sweep 8).
- **PNG**: vendor `stb_image.h` under `libs/third_party/stb/` --
  same single-header lib as stb_truetype, supports PNG / BMP /
  JPEG / GIF.
- **SVG**: skip for v1. PDF rendering also skipped.

**Estimate.** 0.5 sweep.

#### 6.2.7 Power / wake / sleep flow

**Problem.** An ereader must look "always on" -- show the current
page persistently (E-Ink keeps the image with zero power), wake
quickly on button press to turn the page, deep-sleep aggressively.

**Action.** Application-level using existing HAL drivers:
- `ra_lpm_enter_software_standby()` between page turns.
- `ra_icu_enable_wakeup_on_pin(button_irq)` for wake-on-button.
- `ra_lvd` to detect low-battery and warn user 50 pages early.
- `ra_rtc` alarm for periodic-poll wake (e.g. once per hour to
  check for new books over USB / OTA).

**Estimate.** 0.5 sweep (it's mostly app code on top of existing
HAL).

### 6.3 Phasing for the ereader

Roughly 6-month effort if all sweeps run sequentially; can be
shorter with parallel agent work. Suggested order:

1. **Phase 0 first**: ThreadX up.
2. **Phase 4.2 (FileX)** + **Phase 4.3 (GUIX)** + **Phase 4.5
   (LevelX)** in parallel.
3. **6.2.5 (stb_truetype font rendering)** + **6.2.6 (PNG image
   decode)** in parallel.
4. **6.2.3 (ePub container parsing)**.
5. **6.2.4 (reflow engine, hand-rolled v1)**.
6. **6.2.7 (power/wake)**.
7. **6.2.2 (touch IC over IIC_B)** -- only if doing custom board.
8. **6.2.1 (e-paper driver)** -- only if doing custom board.
9. **MVP demo**: load Project Gutenberg's "Pride and Prejudice"
   from the SD card, render it with proper page-turning + library
   view. Target: book opens in <1 s, page turn in <500 ms.
10. **Iterate**: hyphenation, CSS3 selectors, table-of-contents
    navigation, bookmarks, font-size adjustment, day/night mode.

The minimum to "this is an ereader" is FileX + GUIX + stb_truetype
+ ePub parser + hand-rolled reflow. Everything else is polish.

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

## Acceptance for "RA8D2 platform work is done"

We will declare the platform feature-complete when:

1. **Phase 0 lands**: ThreadX runs on EK-RA8D2; `examples/threadx_blink/`
   verified.
2. **Phase 1 lands**: ra_rsip drives real RSIP-E50D registers; NetX
   Secure has a working TLS path with hardware-accelerated AES+SHA;
   BLE patch loader is documented; all 17 prior example apps
   verified on EK-RA8D2 hardware.
3. **Phase 2 lands**: I3C / CANFD / MIPI-DSI / USB hhid / Flash
   partials filled.
4. **Phase 3 lands at least for `ra_drw`**: hardware-accelerated
   blit / fill so `lcd_demo` is not software-pushing pixels.
5. **Phase 4 lands**: ThreadX + NetX Duo + FileX + GUIX + USBX +
   LevelX + NetX Secure + NimBLE all adopted; native fallback
   libraries kept in-tree behind build options.

After that, someone using our HAL has feature parity with FSP +
Azure RTOS X-Ware for everything RA8D2 silicon can do, except for
closed-source Renesas crypto / BLE blobs (genuinely impossible)
and FSP's QE configurator GUI tool (Phase 5 polish).

**Phase 6 is product work**, not platform work. The ereader is the
first product to validate that the platform is actually usable for
shipping a real device.
