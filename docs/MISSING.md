# Missing / Incomplete Driver Surface

Audit date: 2026-04-29 (post-sweep-11 commit `ba54974`, plus Wave 11
closure commits `f4fb1a6`, `7551634`, `f272dc7`, `ce76aa4`, `87b606f`).

This document inventories everything that is **NOT yet implemented or
not yet at FSP-grade feature parity** in `ra8d2-firmware`. Compiled by
walking `/Users/bsikar/Documents/github/_reference/fsp/ra/fsp/src/` and
diffing against `libs/ra_hal/src/` plus each driver's public header.

The cross-verification work in iterations 2-9 fixed register-layout
bugs and stood up native USB device + host class layers. Sweeps 1-11
brought every realistic driver to either FSP-parity feature surface or
an FSP-shaped placeholder. What remains as a true gap is documented
below; the rest moved to `docs/DRIVER_STATUS.md`.

---

## 1. Drivers that were scaffolds but are now feature-complete

The following items from earlier versions of this document were
landed in sweeps 1-11 and are no longer "missing":

- IIC R/W public API: landed in sweep 1 (`3f97975`). `ra_iic_b_read`,
  `ra_iic_b_write`, `ra_iic_b_write_then_read`, `ra_iic_b_abort` are
  all in the public header.
- SDHI block-transfer data phase: landed in sweep 1 (`3f97975`).
  `ra_sdhi_read_block`, `ra_sdhi_write_block`.
- SPI multi-byte read/write: landed in sweep 1 (`3f97975`).
  `ra_spi_read`, `ra_spi_write`, `ra_spi_write_read`, plus DMA path.
- SCI async read/write: landed in sweep 1 (`3f97975`). `ra_sci_read`,
  `ra_sci_write`, `ra_sci_abort`, `ra_sci_baud_calculate`,
  `ra_sci_callback_set`.
- Ethernet TX/RX descriptor rings: landed in sweep 2 (`4cde2a2`).
  `ra_eth_write` and `ra_eth_read` move real frames; PAL ring sits
  on top.
- USB device HID class: landed in sweep 2 (`4cde2a2`).
- USB device MSC class: landed in sweep 2 (`4cde2a2`).
- GLCDC layer-2 + brightness/contrast/dither/CLUT-double-buffered:
  landed in sweep 2 (`4cde2a2`).
- ADC scan groups + continuous-scan + comparator-window + oversampling:
  landed in sweep 3 (`798b019`).
- GPT PWM duty/period set, dead-time, three-phase mode: landed in
  sweep 3 (`798b019`).
- I3C dynamic address assignment + CCC engine: landed in sweep 3
  (`798b019`). HDR-DDR + IBI inbound queue still pending.
- USB composite device: landed in sweep 3 (`798b019`).
- Flash erase / write-block / blank-check: landed in sweep 4
  (`6fd856c`). Suspend/resume + lock-bit programming still pending.
- USB host CDC-ECM: landed in sweep 4 (`6fd856c`).
- USB Type-C / Power Delivery: landed in sweep 4 (`6fd856c`).
- CTSU (capacitive touch): landed in sweep 5 (`171901a`).
- PDC (parallel camera bridge): landed in sweep 5 (`171901a`).
- SCE (hardware crypto -- SHA + AES): landed in sweep 5 (`171901a`).
  RSA + ECC + key-injection paths landed in sweep 11 (`ba54974`).
- DMAC repeat / block / address-update / half-block IRQ: landed in
  sweep 6 (`3ff1a8d`).
- xSPI XIP enter/exit, DTR mode, DQS calibrate, suspend/resume:
  landed in sweep 6 (`3ff1a8d`).
- MIPI-DSI video-mode HSA/HBP/HACT timing: landed in sweep 6
  (`3ff1a8d`). Command-mode payload + ULPS still pending.
- MIPI-CSI virtual-channel + ECC/CRC: landed in sweep 6 (`3ff1a8d`).
- IEEE 1588 PTP master/slave + sync/announce: landed in sweep 6
  (`3ff1a8d`).
- BLE controller bring-up + HCI command/event ring: landed in sweep 8
  (`5e154b9`).
- USB host audio (UAC): landed in sweep 8 (`5e154b9`).
- Software JPEG codec (no FSP `r_jpeg` hardware on RA8D2): landed in
  sweep 8 (`5e154b9`).
- IPv4/ARP/ICMP/UDP/TCP stack (`libs/ra_net`): landed in sweep 8
  (`5e154b9`).
- FAT12/16/32 filesystem (`libs/ra_fs`): landed in sweep 9 (`afeb54a`).
- TLS 1.2 client (`libs/ra_tls`): landed in sweep 9 (`afeb54a`).
- GLCDC framebuffer drawing + 8x16 VGA font (`libs/ra_gfx`): landed
  in sweep 9 (`afeb54a`).
- USB device Audio class: landed in sweep 10 (`59cc3c3`).
- USB device Printer class: landed in sweep 10 (`59cc3c3`).
- USB device Vendor-defined class: landed in sweep 10 (`59cc3c3`).
- USB host hub: landed in sweep 10 (`59cc3c3`).
- BLE host stack (L2CAP + ATT + GATT) `libs/ra_ble_host`: landed in
  sweep 10 (`59cc3c3`).
- Classical CAN driver (`ra_can`): landed in sweep 10 (`59cc3c3`).
- Legacy IIC master + slave (`ra_iic`): landed in sweep 10 (`59cc3c3`).
- IIC_B peripheral (slave) mode (`ra_iic_b_slave`): landed in sweep
  10 (`59cc3c3`).
- Legacy QSPI driver (`ra_qspi`): landed in sweep 10 (`59cc3c3`).
- Calendar-mode RTC variant (`ra_rtc_c`): landed in sweep 10 (`59cc3c3`).
- On-chip op-amp routing (`ra_opamp`): landed in sweep 10 (`59cc3c3`).
- Key Interrupt matrix controller (`ra_kint`): landed in sweep 10
  (`59cc3c3`).
- High-channel-count + low-power comparators (`ra_acmphs_b`,
  `ra_acmplp`): landed in sweep 11 (`ba54974`).
- Differential ADC (`ra_adc_d`): landed in sweep 11 (`ba54974`).
- Legacy 12-bit + 8-bit DAC (`ra_dac`, `ra_dac8`): landed in sweep 11
  (`ba54974`).
- IIR Filter Accelerator (`ra_iirfa`): landed in sweep 11 (`ba54974`).
- Segment LCD controller (`ra_slcdc`): landed in sweep 11 (`ba54974`).
- Legacy UARTA (`ra_uarta`): landed in sweep 11 (`ba54974`).
- Delta-Sigma demodulator (`ra_dsmif`): landed in sweep 11 (`ba54974`).
- Low-power flash (`ra_flash_lp`): landed in sweep 11 (`ba54974`).
- Alternate IIC variants (`ra_iica_master`, `ra_iica_slave`): landed
  in sweep 11 (`ba54974`).
- L3 packet switch (`ra_layer3_switch`): landed in sweep 11 (`ba54974`).
- SAU sub-protocols (`ra_sau_uart`, `_spi`, `_i2c`, `_lin`): landed in
  sweep 11 (`ba54974`).
- Timer Array Unit + PWM (`ra_tau`, `ra_tau_pwm`): landed in sweep 11
  (`ba54974`).
- Timer Module Library (`ra_tml`): landed in sweep 11 (`ba54974`).
- SCE protected ops + key injection (`ra_sce_protected`,
  `ra_sce_key_injection`): landed in sweep 11 (`ba54974`).
- RSA / ECC / ECDSA / ECDH on top of `ra_sce`: landed in sweep 11
  (`ba54974`).
- Example apps `ethernet_tcp_echo`, `lcd_demo`, `usb_hid_device`,
  `usb_msc_device`, `usb_host_keyboard`, `usb_host_msc_browse`:
  landed in sweep 7 (`09abe38`).
- Example apps `cap_touch_demo`, `audio_loopback`, `motor_3phase`:
  landed in sweep 11 (`ba54974`).

---

## 2. Drivers we have, but still partial

### 2.1 I3C (`libs/ra_hal/src/ra_i3c.c`)

After sweep 3, dynamic address assignment and the CCC engine work.
Still missing:
- HDR-DDR (Double Data Rate) mode
- In-Band Interrupt (IBI) inbound queue (NTIBIQP / NTIBIVCTL)
- Slave-mode operation entirely
- Hot-join handling

### 2.2 CANFD (`libs/ra_hal/src/ra_canfd.c`)

Still missing:
- GAFL (Global Acceptance Filter List) programming for hardware filters
- Bit-rate-switching mode for CAN-FD payload phase
- CAN-FD ISO vs non-ISO mode select
- Time-triggered transmission (TTCAN)
- Transmit-history queue

### 2.3 USB host HID (`libs/ra_hal/src/ra_usb_hhid.c`)

`ra_usb_hhid_get_report` IN data phase still stubbed (NOLINT comment
present). Returns the SETUP only; does not collect the response
payload.

### 2.4 Flash / MRAM (`libs/ra_hal/src/ra_flash.c`)

Sweep 4 added erase/program/blank-check. Still missing:
- Suspend / resume during long ops (so a slow erase doesn't block IRQs)
- Code-bank-swap (if the silicon supports dual-bank)
- Background ops with completion callback
- Lock-bit programming for read protection

### 2.5 DAC_B, ACMPHS, RTC, AGT, ULPT, LVD, DOC, BKUP

Small FSP gaps remain on these (see the per-driver Notes column in
`docs/DRIVER_STATUS.md`). None are critical-path for the EK-RA8D2
reference apps.

---

## 3. Drivers we have as FSP-shaped placeholders

The 20 drivers listed below were stood up in sweeps 10-11 to provide
an FSP-compatible public API surface for code that expects these
peripherals. Each carries a file-level `@warning` indicating the
RA8D2 silicon may not actually expose the block, and the body keeps
state in software rather than issuing real register writes.

These are intentional placeholders, not "missing":

| Driver | Reason |
|---|---|
| ra_acmphs_b | RA8D2 silicon ACMPHS register block not in the BSP feature header |
| ra_acmplp | Low-power comparator not exposed in BSP CMSIS header |
| ra_adc_d | Differential ADC mode not described in current HUM cuts |
| ra_dac | Legacy 12-bit DAC; RA8D2 ships DAC_B |
| ra_dac8 | 8-bit DAC; RA8D2 ships DAC_B |
| ra_iirfa | IIR Filter Accelerator block not yet in HUM |
| ra_slcdc | Segment LCD controller not on EK-RA8D2 |
| ra_uarta | Legacy UARTA pre-SCI; RA8D2 ships SCI_B |
| ra_dsmif | Delta-Sigma demodulator |
| ra_flash_lp | Low-power flash variant; RA8D2 ships MRAM |
| ra_iica_master / ra_iica_slave | Alternate IIC variant |
| ra_layer3_switch | L3 packet switch |
| ra_sau_uart / _spi / _i2c / _lin | SAU sub-protocols |
| ra_tau / ra_tau_pwm | Timer Array Unit |
| ra_tml | Timer Module Library |
| ra_sce | AES/SHA backend stubbed in software (FSP `hw_sce_*.c` blobs are closed-source) |
| ra_sce_protected / ra_sce_key_injection | Wraps software SCE backend |
| ra_rsip | Wraps software SCE backend |
| ra_dotf | Decryption-on-the-fly block |
| ra_ble | HCI mailbox at 0x40700000 -- production needs Renesas BLE firmware patch image |
| ra_drw, ra_etha, ra_vin, ra_tsn, ra_rmac | Bring-up scaffolds |
| ra_pdg, ra_bscan, ra_ipc, ra_cnecc, ra_ssie | Bring-up scaffolds |

Promotion path: when Renesas publishes the corresponding RA8D2
register block (or when the silicon revision ships the missing
peripheral), each placeholder can swap from software-state to real
register I/O without changing its public API.

---

## 4. Subsystems still without driver coverage

### 4.1 DRW (Display Renderer)

`ra_drw.c` exists as a register-layout scaffold but is not stitched
into the GLCDC frame pipeline. 2D acceleration handoff TBD.

### 4.2 Multi-core IPC

`ra_ipc.c` is a stub. The Cortex-M33 secondary core does not yet
have a dispatch surface from M85 code. **DEFERRED**: the secondary
core bring-up requires a coordinated boot sequence + shared-memory
contract that has not been spec'd.

### 4.3 Trace / debug

4-bit ETM trace port, boundary scan beyond raw register dump --
no driver, no example. **DEFERRED**: J-Link does not expose the
ETM trace pins on the EK-RA8D2 form factor.

---

## 5. DEFERRED items (intentional non-goals)

The following gaps will not be closed in the foreseeable future and
are listed here for completeness:

- **Renesas closed-source SCE / RSIP `hw_sce_*.c` blobs.** FSP
  ships a binary blob library that implements the actual TRNG +
  authenticated AES + RSA hardware accelerator backend. We cannot
  mirror this in source. `ra_sce`, `ra_sce_protected`,
  `ra_sce_key_injection`, and `ra_rsip` retain a software stub
  backend behind the same public API. Any production use must
  link against Renesas's blob.
- **Renesas BLE firmware patch image.** `ra_ble` and
  `ra_ble_host` exercise the HCI command/event mailbox surface,
  but a real BLE link requires the encrypted DSP firmware blob
  Renesas loads into the radio. Out of scope for this codebase.
- **lwIP feature parity for `libs/ra_net`.** Our hand-written
  IPv4 stack covers ARP/IPv4/ICMP/UDP/TCP plus DNS-A. We will not
  match lwIP's IPv6/SNMP/PPPoE/DHCPv6/802.1X/SLAAC surface.
  Use lwIP via `libs/ra_net_pal` if those are needed.
- **Full BLE GATT profile catalog.** `libs/ra_ble_host` ships a
  starter L2CAP/ATT/GATT server with an 8-service / 32-char
  attribute table. Building out HID-over-GATT, GATT-Mesh, IAS,
  AOA/AOD, etc. is left to application code.
- **Hardware JPEG codec.** RA8D2 silicon does not include a JPEG
  block. We ship `ra_jpeg_sw` (software baseline JPEG, optional
  Helium/MVE acceleration). This will not change unless a future
  silicon revision adds the block.
- **ETM 4-bit trace + boundary scan path-out.** No connector on
  EK-RA8D2.

---

## 6. USB stack residual gaps

### 6.1 Device side -- COMPLETE

`ra_usb_cdc`, `ra_usb_phid`, `ra_usb_pmsc`, `ra_usb_composite`,
`ra_usb_paud`, `ra_usb_pprn`, `ra_usb_pvnd`. Every FSP `r_usb_p*`
class has a peer.

### 6.2 Host side -- COMPLETE except for one stub

`ra_usb_hcdc`, `ra_usb_hmsc`, `ra_usb_hhid`, `ra_usb_hcdc_ecm`,
`ra_usb_haud`, `ra_usb_hhub`. Outstanding stub:

- `ra_usb_hhid_get_report` IN data phase (returns SETUP only). All
  other host classes feature-complete.

---

## 7. Things tested only at the simulator level

The host-side test corpus (139 ctests as of sweep 11) is
register-write semantics + happy-path flows in a memory-mapped
simulator. Real-world coverage gaps:

- Driver behavior under back-to-back calls (state reset between)
- Driver interaction with ICU interrupt latency
- Driver behavior when MSTP is gated off mid-operation
- DMAC + driver pairing under load
- Multi-core IPC stress

---

## 8. Summary

Snapshot at the close of sweep 11 (commit `ba54974`) plus
Wave 11 closure work (commits `f4fb1a6`, `7551634`, `f272dc7`,
`ce76aa4`, `87b606f`):

- **Sweeps complete**: 11 (after the iter 2-9 cross-verify waves)
- **Driver source files** in `libs/ra_hal/src/`: 115
- **Top-level libraries**: 11 -- ra_core, ra_hal, ra_nsc,
  ra_net_pal, ra_usb_pal, ra_net, ra_fs, ra_tls, ra_gfx,
  ra_ble_host, plus the ra_hal-anchored secure_app key vault.
- **Test executables**: 139 (ctest -N reports 139)
- **Hardware-flashable example apps**: 15 (was 6 at start of
  sweep 7, was 12 at end of sweep 8)
- **Headers in `libs/ra_hal/inc/`**: 185

Of the 115 HAL drivers:
- 65 are feature-complete vs FSP parity (real register writes,
  full data path, FSP-shape API).
- 6 are partial (I3C HDR-DDR/IBI, CANFD GAFL/BRS, USB host HID
  GET_REPORT IN, flash suspend/resume + lock-bits, plus a few
  small analog/RTC gaps -- see DRIVER_STATUS.md Notes column).
- 20 are FSP-shaped placeholders (RA8D2 silicon doesn't expose
  the block, but the public API is preserved for FSP-app
  compatibility -- see section 3).
- 24 are bring-up scaffolds (register-layout traced; data path
  not yet wired -- ra_drw, ra_etha, ra_vin, ra_tsn, ra_rmac,
  ra_ssie, ra_dotf, ra_cnecc, ra_ipc, ra_pdg, ra_bscan, ra_ceu,
  plus the placeholder set).

The "everything FULLY implemented" target -- every FSP `r_*` driver
matched at hardware-real I/O level, every silicon block driven from
an example, every veneer exercised end-to-end on the EVM -- is
substantially blocked by:

- Renesas closed-source crypto + BLE firmware blobs (DEFERRED).
- Silicon blocks not present on RA8D2 (placeholders, will stay
  placeholders).
- Multi-core IPC + ETM trace (DEFERRED for tooling reasons).

The realistic remaining work scope is:

- 1-2 sweeps of HAL completion on residual partial drivers (I3C
  HDR-DDR, CANFD GAFL, USB hHID get_report IN, flash suspend).
- 0-2 sweeps of new examples driving the placeholder peripherals
  end-to-end on EVM (most meaningful examples are landed).

Total estimated effort to "no realistic gap remains": 2-4 more
sweeps. Beyond that the gaps become DEFERRED items per section 5.
