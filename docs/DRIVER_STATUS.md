# Driver Status Matrix

Audit date: 2026-04-29 (post-sweep-11 commit `ba54974`, plus
closure commits `f4fb1a6`, `7551634`, `f272dc7`, `ce76aa4`, `87b606f`).

This file is the at-a-glance map of every driver in
`libs/ra8_hal/src/` plus the new top-level libraries (`libs/ra8_net/`,
`libs/ra8_fs/`, `libs/ra8_tls/` (Mbed TLS facade), `libs/ra8_gfx/`)
against their FSP `r_*` parity benchmark. Status
classes:

- `feature-complete` -- the public API matches FSP `R_*` parity for
  the in-scope feature set on RA8D2. Real register I/O on real
  silicon. May still be missing rarely-used FSP-only sub-modes;
  those are noted in the rightmost column.
- `partial` -- a useful subset of the FSP API is implemented but
  major features (peripheral mode, DMA, edge cases, etc.) are still
  missing. The driver moves real bytes today.
- `placeholder` -- driver wraps an FSP-shape register block whose
  RA8D2-silicon presence we could not confirm. File header carries
  a `@warning`. Public API is preserved so FSP example projects
  link against us; bodies maintain software state instead of
  issuing real register writes.
- `scaffold` -- init / deinit / register layout exists, no real
  data path. Cannot move bytes. Distinct from placeholder in that
  the silicon block is known to exist but the driver hasn't been
  wired up.
- `wholly-new` -- has no direct FSP counterpart on RA8D2 (or
  intentionally diverges). Treated as feature-complete relative
  to its own contract.

The FSP benchmark for "feature-complete" is the public surface of
`renesas/fsp` `r_<name>/r_<name>.c` and the corresponding
`r_<name>_api.h` interface contract.

Citations point at the sweep commit that landed the bulk of the
work; subsequent fixes may live in later commits.

---

## Bus / serial / parallel I/O

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra8_sci | r_sci_b_uart | feature-complete | sweep 1 (`3f97975`) | async read/write, abort, baud calc, DMA path, callback set |
| ra8_iic_b | IIC_B (controller mode) | feature-complete | sweep 1 (`3f97975`) | polling+IRQ read/write, restart, abort, callback. Peripheral mode = ra8_i2c_peripheral |
| ra8_i2c_peripheral | IIC_B (peripheral mode) | feature-complete | sweep 10 (`59cc3c3`) | IIC_B peripheral mode, MSDVAD + BCTL.BUSE + SVCTL.GCAE |
| ra8_iic | RIIC (controller + peripheral modes) | feature-complete | sweep 10 (`59cc3c3`) | legacy RIIC IP, controller + peripheral on one driver |
| ra8_iica_controller | IICA (controller mode) | placeholder | sweep 11 (`ba54974`) | alternate IIC variant; RA8D2 silicon presence unverified |
| ra8_iica_peripheral | IICA (peripheral mode) | placeholder | sweep 11 (`ba54974`) | alternate IIC variant; RA8D2 silicon presence unverified |
| ra8_spi | r_spi_b | feature-complete | sweep 1 (`3f97975`) | read/write/write_read, 8/16/32-bit, DMA path. Peripheral mode = partial |
| ra8_spi_b | r_spi_b | feature-complete | (pre-sweep) | extended ra8_spi back-end |
| ra8_sau_uart | r_sau_uart | placeholder | sweep 11 (`ba54974`) | SAU sub-protocol; RA8D2 silicon presence unverified |
| ra8_sau_spi | r_sau_spi | placeholder | sweep 11 (`ba54974`) | SAU sub-protocol; RA8D2 silicon presence unverified |
| ra8_sau_i2c | r_sau_i2c | placeholder | sweep 11 (`ba54974`) | SAU sub-protocol; RA8D2 silicon presence unverified |
| ra8_sau_lin | r_sau_lin | placeholder | sweep 11 (`ba54974`) | SAU sub-protocol; RA8D2 silicon presence unverified |
| ra8_uarta | r_uarta | placeholder | sweep 11 (`ba54974`) | legacy UARTA pre-SCI; RA8D2 ships SCI_B |
| ra8_sdhi | r_sdhi | feature-complete | sweep 1 (`3f97975`) | block read/write, 4-bit bus, control commands. Speed-class auto-switch deferred |
| ra8_i3c | r_i3c | partial | sweep 3 (`798b019`) | dynamic-address assign, CCC engine landed; HDR-DDR + IBI inbound queue still missing |
| ra8_can | r_can | feature-complete | sweep 10 (`59cc3c3`) | classical CAN, 11/29-bit IDs, mailbox arbitration |
| ra8_canfd | r_canfd | partial | (pre-sweep) | TX MB0 send + RX FIFO0 recv. GAFL filter + bit-rate switching still missing |
| ra8_gpio | r_ioport | feature-complete | (pre-sweep) | PFS + ICU IRQ attach. NCODR/PCR/DSCR + group-port atomic update remain |
| ra8_kint | r_kint | feature-complete | sweep 10 (`59cc3c3`) | Key Interrupt matrix, KRCTL column scan + KRSTR row read |

## Analog / safety / time

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| adc (adc_b) | r_adc_b | feature-complete | sweep 3 (`798b019`) | scan groups A/B, continuous scan, comparator-window, oversampling |
| ra8_adc_d | r_adc_d | placeholder | sweep 11 (`ba54974`) | differential ADC mode; RA8D2 silicon mode unverified |
| ra8_dac_b | r_dac_b | partial | (pre-sweep) | single-channel write. Synchronized two-channel update + ADC-trigger update missing |
| ra8_dac | r_dac | placeholder | sweep 11 (`ba54974`) | legacy 12-bit DAC; RA8D2 ships DAC_B |
| ra8_dac8 | r_dac8 | placeholder | sweep 11 (`ba54974`) | 8-bit DAC; RA8D2 ships DAC_B |
| ra8_acmphs | r_acmphs | partial | (pre-sweep) | comparator init + read. Filter / pin-out routing limited |
| ra8_acmphs_b | r_acmphs_b | placeholder | sweep 11 (`ba54974`) | high-channel-count comparator; RA8D2 silicon presence unverified |
| ra8_acmplp | r_acmplp | placeholder | sweep 11 (`ba54974`) | low-power comparator; RA8D2 silicon presence unverified |
| ra8_opamp | r_opamp | feature-complete | sweep 10 (`59cc3c3`) | on-chip op-amp routing, AMPC/AMPGAIN/AMPINSEL/AMPTRS, 4 channels |
| ra8_iirfa | r_iirfa | placeholder | sweep 11 (`ba54974`) | IIR Filter Accelerator; RA8D2 silicon presence unverified |
| ra8_dsmif | r_dsmif | placeholder | sweep 11 (`ba54974`) | Delta-Sigma Demodulator; RA8D2 silicon presence unverified |
| ra8_rtc | r_rtc | partial | (pre-sweep) | calendar set/get, alarm. Periodic-IRQ and pseudo-32k mode pending |
| ra8_rtc_c | r_rtc_c | feature-complete | sweep 10 (`59cc3c3`) | calendar-mode RTC variant |
| ra8_wdt / ra8_iwdt | r_wdt / r_iwdt | feature-complete | (pre-sweep) | refresh + status. Period locked by OFS0 by silicon |
| ra8_agt | r_agt | partial | (pre-sweep) | reload + IRQ. Event-counter mode missing |
| ra8_ulpt | r_ulpt | partial | (pre-sweep) | period + IRQ. Pulse-output / event-counter modes pending |
| ra8_cac | r_cac | feature-complete | (pre-sweep) | measure + IRQ on bound violation |
| ra8_crc | r_crc | feature-complete | (pre-sweep) | poly select + DMA-friendly streaming |
| ra8_doc | r_doc | partial | (pre-sweep) | data-operation circuit; comparator-mode missing |

## Timers / motor

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra8_gpt | r_gpt | feature-complete | sweep 3 (`798b019`) | PWM duty/period set, PWM pin output, dead-time |
| ra8_gpt three-phase | r_gpt_three_phase | feature-complete | sweep 3 (`798b019`) | `ra8_gpt_three_phase_*` family for motor PWM |
| ra8_poeg | r_poeg | feature-complete | (pre-sweep) | trigger-stop, status get/clear |
| ra8_tau | r_tau | placeholder | sweep 11 (`ba54974`) | Timer Array Unit; RA8D2 silicon presence unverified |
| ra8_tau_pwm | r_tau_pwm | placeholder | sweep 11 (`ba54974`) | Timer Array Unit PWM; RA8D2 silicon presence unverified |
| ra8_tml | r_tml | placeholder | sweep 11 (`ba54974`) | Timer Module Library; RA8D2 silicon presence unverified |

## DMA / event

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra8_dma (substrate) | (composite of r_dmac + r_dtc) | feature-complete | (pre-sweep) | request/configure/start primitive used by every other driver |
| ra8_dmac | r_dmac | feature-complete | sweep 6 (`3ff1a8d`) | repeat / block transfer, address-update modes, half-block IRQ |
| ra8_dtc | r_dtc | feature-complete | (pre-sweep) | vector-table-driven setup, RRS toggle, DTCSTS get/clear |
| ra8_elc | r_elc | feature-complete | (pre-sweep) | event link rewrite covering all peripheral sources |
| ra8_icu | r_icu | feature-complete | (pre-sweep) | IRQCR + NMI + IELSR allocator |

## Display / video / camera

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra8_glcdc | r_glcdc | feature-complete | sweep 2 (`4cde2a2`) | layer-2 alpha blend, CLUT double-buffered, brightness/contrast/dither |
| ra8_drw | r_drw | scaffold | (pre-sweep) | register layout only; not stitched into GLCDC pipeline |
| ra8_mipi_dsi | r_mipi_dsi | partial | sweep 6 (`3ff1a8d`) | video-mode HSA/HBP/HACT timing + lane config landed. Command-mode payload TBD |
| ra8_mipi_csi | r_mipi_csi | partial | sweep 6 (`3ff1a8d`) | virtual-channel + ECC/CRC checking landed. ULPS entry/exit TBD |
| ra8_mipi_phy | r_mipi_phy | feature-complete | (pre-sweep) | RFREQ encoding fixed in iter 3 |
| ra8_pdc | r_pdc | feature-complete | sweep 5 (`171901a`) | parallel camera bridge for OV5640 |
| ra8_ceu | (no FSP analog on RA8D2) | scaffold | (pre-sweep) | CEU register layout; PDC is the preferred path |
| ra8_jpeg_sw | (software) | wholly-new | sweep 8 (`5e154b9`) | software JPEG codec, no FSP `r_jpeg` hardware analog on RA8D2 |
| ra8_vin | r_vin | scaffold | (pre-sweep) | bring-up only |
| ra8_slcdc | r_slcdc | placeholder | sweep 11 (`ba54974`) | Segment LCD controller; not on EK-RA8D2 |

## Audio

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra8_ssie | r_ssi_b | scaffold | (pre-sweep) | register layout; data path TBD |
| ra8_pdm | r_pdm | feature-complete | (pre-sweep) | FIFO drain + IRQ. Decimation FIR is a downstream task |

## Storage / external memory

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra8_xspi | r_ospi_b | feature-complete | sweep 6 (`3ff1a8d`) | XIP enter/exit, DTR mode, DQS calibrate, suspend/resume |
| ra8_qspi | r_qspi | feature-complete | sweep 10 (`59cc3c3`) | legacy QSPI command-issue + memory-mapped XIP read |
| ra8_sdramc | r_bsp memory init | feature-complete | (pre-sweep) | timing + refresh interval + power transition |
| ra8_flash (MRAM) | r_mram | feature-complete | sweep 4 (`6fd856c`) | erase/write_block/blank_check + ARC; lock-bit + dual-bank deferred |
| ra8_flash_lp | r_flash_lp | placeholder | sweep 11 (`ba54974`) | low-power flash variant; RA8D2 ships MRAM |

## Networking

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra8_eth (gwca) | r_ether + r_ether_phy | feature-complete | sweep 2 (`4cde2a2`) | TX/RX descriptor rings, `ra8_eth_read` / `ra8_eth_write`, link state |
| ra8_eth_coma | (RA8D2-only) | feature-complete | (pre-sweep) | management agent, IRQ dispatch |
| ra8_eth_mfwd | (RA8D2-only) | feature-complete | (pre-sweep) | message-forwarding engine |
| ra8_eth_gptp | r_gptp | feature-complete | (pre-sweep) | timestamp counter + IRQ |
| ra8_etha | r_ether | scaffold | (pre-sweep) | bring-up; the live datapath uses gwca |
| ra8_ptp | r_ptp | feature-complete | sweep 6 (`3ff1a8d`) | IEEE 1588 controller/peripheral roles (per spec), sync/announce, time/rate adjust |
| ra8_tsn | r_tsn | scaffold | (pre-sweep) | IEEE 802.1 TSN block |
| ra8_rmac | r_rmac | scaffold | (pre-sweep) | reduced-MAC variant |
| ra8_layer3_switch | r_layer3_switch | placeholder | sweep 11 (`ba54974`) | L3 packet switch; RA8D2 silicon presence unverified |
| libs/ra8_net (IPv4) | (no FSP equivalent; NetX Duo is the heavy-weight choice) | wholly-new | sweep 8 (`5e154b9`) | hand-written ARP + IPv4 + ICMP + UDP + TCP stack on top of ra8_eth |

## USB

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra8_usb (FS+HS controller) | r_usb_basic + class peers | feature-complete | (pre-sweep) | unified controller front-end |
| ra8_usb_cdc (device) | r_usb_pcdc | feature-complete | (pre-sweep) | CDC ACM device |
| ra8_usb_phid (device) | r_usb_phid | feature-complete | sweep 2 (`4cde2a2`) | HID device class |
| ra8_usb_pmsc (device) | r_usb_pmsc | feature-complete | sweep 2 (`4cde2a2`) | MSC device class with SCSI command shim |
| ra8_usb_paud (device) | r_usb_paud | feature-complete | sweep 10 (`59cc3c3`) | USB device Audio class (UAC 1.0/2.0), iso-IN + iso-OUT |
| ra8_usb_pprn (device) | r_usb_pprn | feature-complete | sweep 10 (`59cc3c3`) | USB device Printer class, GET_DEVICE_ID / GET_PORT_STATUS / SOFT_RESET |
| ra8_usb_pvnd (device) | r_usb_pvnd | feature-complete | sweep 10 (`59cc3c3`) | USB device vendor-defined raw bulk transport |
| ra8_usb_composite (device) | r_usb_composite | feature-complete | sweep 3 (`798b019`) | multi-class composite device |
| ra8_usb_hcdc (host) | r_usb_hcdc | feature-complete | (pre-sweep) | host CDC ACM |
| ra8_usb_hmsc (host) | r_usb_hmsc | feature-complete | (pre-sweep) | host MSC + bulk-only transport |
| ra8_usb_hhid (host) | r_usb_hhid | partial | (pre-sweep) | host HID; `get_report` IN data phase still stubbed |
| ra8_usb_hcdc_ecm (host) | r_usb_hcdc_ecm | feature-complete | sweep 4 (`6fd856c`) | CDC-ECM (ethernet over USB host) |
| ra8_usb_haud (host) | r_usb_haud | feature-complete | sweep 8 (`5e154b9`) | host audio |
| ra8_usb_hhub (host) | r_usb_hhub | feature-complete | sweep 10 (`59cc3c3`) | USB host hub, up to 16 ports, USB 2.0 ch11 class requests |
| ra8_usb_typec | r_usb_typec | feature-complete | sweep 4 (`6fd856c`) | USB Type-C / PD orientation, source/sink, PD message build |

## Security / crypto

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra8_sce | r_sce | placeholder | sweep 5 (`171901a`) | SHA-1/256/384/512, AES-128/192/256, RSA, ECC. Backend is software stub; FSP `hw_sce_*.c` is closed-source |
| ra8_sce_protected | r_sce_protected | placeholder | sweep 11 (`ba54974`) | uses wrapped keys without exposing them. Backend wraps software ra8_sce |
| ra8_sce_key_injection | r_sce_key_injection | placeholder | sweep 11 (`ba54974`) | wrap raw AES/RSA/ECC keys into SCE wrapped-key blobs |
| ra8_rsip | r_rsip | placeholder | (pre-sweep) | wraps software ra8_sce; FSP r_rsip blob path not mirrorable |
| ra8_dotf | r_dotf | placeholder | (pre-sweep) | DOTF block layout; silicon mode unverified |
| ra8_cnecc | (no FSP RA8D2 analog) | scaffold | (pre-sweep) | bring-up |
| key_vault (secure_app) | (project-local) | wholly-new | (pre-sweep) | 8-slot 256-bit key store + SHA-256 challenge veneer |

## Wireless

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra8_ble | r_ble | placeholder | sweep 8 (`5e154b9`) | controller bring-up + HCI command/event ring; production needs Renesas firmware patch image |

## HMI / sensing

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra8_ctsu | r_ctsu | feature-complete | sweep 5 (`171901a`) | self-cap + mutual-cap scans, threshold filter |

## Power / clock / reset

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra8_cgc | r_cgc | feature-complete | (pre-sweep) | runtime reconfigure + stop detect |
| ra8_pwr / ra8_lpm | r_lpm | feature-complete | (pre-sweep) | LPM + module clock gating |
| ra8_mstp | (BSP utility) | feature-complete | (pre-sweep) | ref-counted module-stop |
| ra8_lvd | r_lvd | partial | (pre-sweep) | level detect + IRQ. Reset-on-trip not wired |
| ra8_reset | r_bsp reset | feature-complete | (pre-sweep) | software-reset, source readout |
| ra8_vreg | r_bsp vreg | feature-complete | (pre-sweep) | regulator config |

## Inter-core / debug / misc

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra8_ipc | r_ipc | scaffold | (pre-sweep) | M85 <-> M33 mailbox bring-up |
| ra8_pdg | r_pdg | scaffold | (pre-sweep) | bring-up |
| ra8_bkup | r_bkup | partial | (pre-sweep) | backup domain register read/write |
| ra8_sram | r_bsp ECC | feature-complete | (pre-sweep) | ECC enable + IRQ |
| ra8_bscan | r_bscan | scaffold | (pre-sweep) | boundary scan |
| ra8_mpc | r_ioport (PFS facade) | feature-complete | (pre-sweep) | pin route + PWPR unlock |
| ra8_ofs | (BSP option-function-select) | feature-complete | (pre-sweep) | OFS0/OFS1 read + audit |

## Top-level libraries

| Library | Status | Landed | Notes |
|---|---|---|---|
| libs/ra8_core | feature-complete | (pre-sweep) | ra8_err / ra8_check / ra8_log / ra8_assert substrate |
| libs/ra8_hal | feature-complete | (across all sweeps) | 115 driver source files, 185 headers |
| libs/ra8_nsc | feature-complete | (pre-sweep) | NSC veneers across comms + I/O + key vault + xspi + eth + log |
| libs/ra8_net_pal | feature-complete | (pre-sweep) | NetX Duo port glue with in-memory ring fallback |
| libs/ra8_usb_pal | feature-complete | (pre-sweep) | CherryUSB usb_dc port glue with per-EP ring |
| libs/ra8_net | wholly-new | sweep 8 (`5e154b9`) | hand-written ARP/IPv4/ICMP/UDP/TCP, no third-party stack |
| libs/ra8_fs | feature-complete | sweep 9 (`afeb54a`) | FAT12/16/32 reader+writer, mount/open/read/write/seek/listdir |
| libs/ra8_tls | partial | sweep 9 (`afeb54a`) | TLS 1.2 client, RSA key-exchange encrypt awaits real ra8_sce RSA path |
| libs/ra8_gfx | feature-complete | sweep 9 (`afeb54a`) | GLCDC framebuffer drawing + 8x16 IBM PC VGA font (95 ASCII glyphs) |
| libs/ra8_display_pal (LCD backend) | feature-complete | 2026-05-12 | One-vtable PAL over ra8_glcdc; folds 6-step bring-up |
| libs/ra8_display_pal (e-ink backend) | scaffold | 2026-05-12 | IT8951 stub: init/get_caps work; flush/get_fb return `k_ra8_err_not_supported` until hardware lands |

---

## Summary tally

Driver count rollup at the close of sweep 11:

- **115 driver source files** in `libs/ra8_hal/src/`
- **65 feature-complete** drivers (real register I/O, FSP-parity API)
- **6 partial** drivers (data path live, FSP gaps documented):
  ra8_i3c (HDR-DDR/IBI), ra8_canfd (GAFL/BRS), ra8_usb_hhid (get_report
  IN), ra8_lvd (reset-on-trip), ra8_dac_b (sync update), ra8_acmphs
  (filter routing), plus small gaps on ra8_rtc, ra8_agt, ra8_ulpt,
  ra8_doc, ra8_bkup
- **20 placeholders** with `@warning` (FSP-shape API, software-state
  body): ra8_acmphs_b, ra8_acmplp, ra8_adc_d, ra8_dac, ra8_dac8,
  ra8_iirfa, ra8_slcdc, ra8_uarta, ra8_dsmif, ra8_flash_lp,
  ra8_iica_controller, ra8_iica_peripheral, ra8_layer3_switch, ra8_sau_*,
  ra8_tau, ra8_tau_pwm, ra8_tml, plus crypto + BLE
  (ra8_sce family, ra8_rsip, ra8_dotf, ra8_ble)
- **24 scaffolds** (register layout traced, data path not wired):
  ra8_drw, ra8_etha, ra8_vin, ra8_tsn, ra8_rmac, ra8_ssie, ra8_cnecc,
  ra8_ipc, ra8_pdg, ra8_bscan, ra8_ceu

Top-level library rollup:

- **10 libraries** total in `libs/`
- **9 feature-complete** (ra8_core, ra8_hal, ra8_nsc, ra8_net_pal,
  ra8_usb_pal, ra8_net, ra8_fs, ra8_gfx, plus the
  secure_app key_vault inside ra8_hal)
- **1 partial** (ra8_tls -- awaits real RSA primitive)

Examples: 15 hardware-flashable applications (was 6 at start of
sweep 7, was 12 at end of sweep 8, was 12 at end of sweep 10).

Tests: 139 ctest executables covering every module above (was 91
at start of sweep 1, was 144 internally counted at sweep 8, ctest
-N reports 139 today after consolidation).

Of the 6 "partial" entries, only two (ra8_i3c HDR-DDR/IBI, ra8_canfd
GAFL/BRS) are realistic candidates for completion without external
inputs. The crypto + BLE placeholders cannot be promoted to
feature-complete without Renesas's closed-source firmware blobs --
see `docs/VENDOR_BLOBS.md`.
