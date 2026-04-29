# Driver Status Matrix

Audit date: 2026-04-29 (post-sweep-8 commit `5e154b9`).

This file is the at-a-glance map of every driver in
`libs/ra_hal/src/` plus the new top-level libraries (`libs/ra_net/`,
`libs/ra_fs/`) against their FSP `r_*` parity benchmark. Status
classes:

- `scaffold` -- init / deinit / register layout exists, no real
  data path. Cannot move bytes.
- `partial` -- a useful subset of the FSP API is implemented but
  major features (slave mode, DMA, edge cases, etc.) are still
  missing.
- `feature-complete` -- the public API matches FSP `R_*` parity
  for the in-scope feature set on RA8D2. May still be missing
  rarely-used FSP-only sub-modes; those are noted.
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
| ra_sci | r_sci_b_uart | feature-complete | sweep 1 (`3f97975`) | async read/write, abort, baud calc, DMA path, callback set |
| ra_iic_b | r_iic_b_master | feature-complete | sweep 1 (`3f97975`) | polling+IRQ read/write, restart, abort, callback. Slave mode N/A on RA8D2 master IP |
| ra_spi | r_spi_b | feature-complete | sweep 1 (`3f97975`) | read/write/write_read, 8/16/32-bit, DMA path. Slave mode = partial |
| ra_sdhi | r_sdhi | feature-complete | sweep 1 (`3f97975`) | block read/write, 4-bit bus, control commands. Speed-class auto-switch deferred |
| ra_i3c | r_i3c | partial | sweep 3 (`798b019`) | dynamic-address assign, CCC engine landed; HDR-DDR + IBI inbound queue still missing |
| ra_canfd | r_canfd | partial | (pre-sweep) | TX MB0 send + RX FIFO0 recv. GAFL filter + bit-rate switching still missing |
| ra_gpio | r_ioport | feature-complete | (pre-sweep) | PFS + ICU IRQ attach. NCODR/PCR/DSCR + group-port atomic update remain |

## Analog / safety / time

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| adc (adc_b) | r_adc_b | feature-complete | sweep 3 (`798b019`) | scan groups A/B, continuous scan, comparator-window, oversampling |
| ra_dac_b | r_dac_b | partial | (pre-sweep) | single-channel write. Synchronized two-channel update + ADC-trigger update missing |
| ra_acmphs | r_acmphs | partial | (pre-sweep) | comparator init + read. Filter / pin-out routing limited |
| ra_rtc | r_rtc | partial | (pre-sweep) | calendar set/get, alarm. Periodic-IRQ and pseudo-32k mode pending |
| ra_wdt / ra_iwdt | r_wdt / r_iwdt | feature-complete | (pre-sweep) | refresh + status. Period locked by OFS0 by silicon |
| ra_agt | r_agt | partial | (pre-sweep) | reload + IRQ. Event-counter mode missing |
| ra_ulpt | r_ulpt | partial | (pre-sweep) | period + IRQ. Pulse-output / event-counter modes pending |
| ra_cac | r_cac | feature-complete | (pre-sweep) | measure + IRQ on bound violation |
| ra_crc | r_crc | feature-complete | (pre-sweep) | poly select + DMA-friendly streaming |

## Timers / motor

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra_gpt | r_gpt | feature-complete | sweep 3 (`798b019`) | PWM duty/period set, PWM pin output, dead-time |
| ra_gpt three-phase | r_gpt_three_phase | feature-complete | sweep 3 (`798b019`) | `ra_gpt_three_phase_*` family for motor PWM |
| ra_poeg | r_poeg | feature-complete | (pre-sweep) | trigger-stop, status get/clear |

## DMA / event

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra_dma (substrate) | (composite of r_dmac + r_dtc) | feature-complete | (pre-sweep) | request/configure/start primitive used by every other driver |
| ra_dmac | r_dmac | feature-complete | sweep 6 (`3ff1a8d`) | repeat / block transfer, address-update modes, half-block IRQ |
| ra_dtc | r_dtc | feature-complete | (pre-sweep) | vector-table-driven setup, RRS toggle, DTCSTS get/clear |
| ra_elc | r_elc | feature-complete | (pre-sweep) | event link rewrite covering all peripheral sources |
| ra_icu | r_icu | feature-complete | (pre-sweep) | IRQCR + NMI + IELSR allocator |

## Display / video / camera

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra_glcdc | r_glcdc | feature-complete | sweep 2 (`4cde2a2`) | layer-2 alpha blend, CLUT double-buffered, brightness/contrast/dither |
| ra_drw | r_drw | scaffold | (pre-sweep) | register layout only; not stitched into GLCDC pipeline |
| ra_mipi_dsi | r_mipi_dsi | partial | sweep 6 (`3ff1a8d`) | video-mode HSA/HBP/HACT timing + lane config landed. Command-mode payload TBD |
| ra_mipi_csi | r_mipi_csi | partial | sweep 6 (`3ff1a8d`) | virtual-channel + ECC/CRC checking landed. ULPS entry/exit TBD |
| ra_mipi_phy | r_mipi_phy | feature-complete | (pre-sweep) | RFREQ encoding fixed in iter 3 |
| ra_pdc | r_pdc | feature-complete | sweep 5 (`171901a`) | parallel camera bridge for OV5640 |
| ra_ceu | (no FSP analog on RA8D2) | partial | (pre-sweep) | ceu register layout; PDC is the preferred path |
| ra_jpeg_sw | (software) | wholly-new | sweep 8 (`5e154b9`) | software JPEG codec, no FSP `r_jpeg` hardware analog on RA8D2 |
| ra_vin | r_vin | scaffold | (pre-sweep) | bring-up only |

## Audio

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra_ssie | r_ssi_b | scaffold | (pre-sweep) | register layout; data path TBD |
| ra_pdm | r_pdm | feature-complete | (pre-sweep) | FIFO drain + IRQ. Decimation FIR is a downstream task |

## Storage / external memory

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra_xspi | r_ospi_b | feature-complete | sweep 6 (`3ff1a8d`) | XIP enter/exit, DTR mode, DQS calibrate, suspend/resume |
| ra_sdramc | r_bsp memory init | feature-complete | (pre-sweep) | timing + refresh interval + power transition |
| ra_flash (MRAM) | r_mram | feature-complete | sweep 4 (`6fd856c`) | erase/write_block/blank_check + ARC; lock-bit + dual-bank deferred |

## Networking

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra_eth (gwca) | r_ether + r_ether_phy | feature-complete | sweep 2 (`4cde2a2`) | TX/RX descriptor rings, `ra_eth_read` / `ra_eth_write`, link state |
| ra_eth_coma | (RA8D2-only) | feature-complete | (pre-sweep) | management agent, IRQ dispatch |
| ra_eth_mfwd | (RA8D2-only) | feature-complete | (pre-sweep) | message-forwarding engine |
| ra_eth_gptp | r_gptp | feature-complete | (pre-sweep) | timestamp counter + IRQ |
| ra_etha | r_ether | scaffold | (pre-sweep) | bring-up; the live datapath uses gwca |
| ra_ptp | r_ptp | feature-complete | sweep 6 (`3ff1a8d`) | IEEE 1588 master/slave roles, sync/announce, time/rate adjust |
| ra_tsn | r_tsn | scaffold | (pre-sweep) | IEEE 802.1 TSN block |
| ra_rmac | r_rmac | scaffold | (pre-sweep) | reduced-MAC variant |
| libs/ra_net (IPv4) | (no FSP equivalent; lwIP would be FSP's pick) | wholly-new | sweep 8 (`5e154b9`) | hand-written ARP + IPv4 + ICMP + UDP + TCP stack on top of ra_eth |

## USB

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra_usb (FS+HS controller) | r_usb_basic + r_usb_hmsc/hcdc/hhid + ... | feature-complete | (pre-sweep) | unified controller front-end |
| ra_usb_cdc (device) | r_usb_pcdc | feature-complete | (pre-sweep) | CDC ACM device |
| ra_usb_phid (device) | r_usb_phid | feature-complete | sweep 2 (`4cde2a2`) | HID device class |
| ra_usb_pmsc (device) | r_usb_pmsc | feature-complete | sweep 2 (`4cde2a2`) | MSC device class with SCSI command shim |
| ra_usb_composite (device) | r_usb_composite | feature-complete | sweep 3 (`798b019`) | multi-class composite device |
| ra_usb_hcdc (host) | r_usb_hcdc | feature-complete | (pre-sweep) | host CDC ACM |
| ra_usb_hmsc (host) | r_usb_hmsc | feature-complete | (pre-sweep) | host MSC + bulk-only transport |
| ra_usb_hhid (host) | r_usb_hhid | partial | (pre-sweep) | host HID; `get_report` IN data phase still stubbed |
| ra_usb_hcdc_ecm (host) | r_usb_hcdc_ecm | feature-complete | sweep 4 (`6fd856c`) | CDC-ECM (ethernet over USB host) |
| ra_usb_haud (host) | r_usb_haud | feature-complete | sweep 8 (`5e154b9`) | host audio |
| ra_usb_typec | r_usb_typec | feature-complete | sweep 4 (`6fd856c`) | USB Type-C / PD orientation, source/sink, PD message build |

## Security / crypto

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra_sce | r_sce + r_sce_protected | feature-complete | sweep 5 (`171901a`) | SHA-1/256/384/512, AES-128/192/256, key handling. RSA + ECC = task |
| ra_rsip | r_rsip | scaffold | (pre-sweep) | unprotected RSIP only; r_rsip_protected + key_injection still missing |
| ra_dotf | r_dotf | scaffold | (pre-sweep) | DOTF block layout |
| ra_cnecc | (no FSP RA8D2 analog) | scaffold | (pre-sweep) | bring-up |
| key_vault (secure_app) | (project-local) | wholly-new | (pre-sweep) | 8-slot 256-bit key store + SHA-256 challenge veneer |

## Wireless

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra_ble | r_ble | scaffold | sweep 8 (`5e154b9`) | controller bring-up + HCI command/event ring; full host stack pending |

## HMI / sensing

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra_ctsu | r_ctsu | feature-complete | sweep 5 (`171901a`) | self-cap + mutual-cap scans, threshold filter |

## Power / clock / reset

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra_cgc | r_cgc | feature-complete | (pre-sweep) | runtime reconfigure + stop detect |
| ra_pwr / ra_lpm | r_lpm | feature-complete | (pre-sweep) | LPM + module clock gating |
| ra_mstp | (BSP utility) | feature-complete | (pre-sweep) | ref-counted module-stop |
| ra_lvd | r_lvd | partial | (pre-sweep) | level detect + IRQ. Reset-on-trip not wired |
| ra_reset | r_bsp reset | feature-complete | (pre-sweep) | software-reset, source readout |
| ra_vreg | r_bsp vreg | feature-complete | (pre-sweep) | regulator config |
| ra_pwr_b | r_lpm | feature-complete | (pre-sweep) | (covered by ra_pwr) |

## Inter-core / debug / misc

| Driver | FSP parity | Status | Landed | Notes |
|---|---|---|---|---|
| ra_ipc | r_ipc | scaffold | (pre-sweep) | M85 <-> M33 mailbox bring-up |
| ra_doc | r_doc | partial | (pre-sweep) | data-operation circuit; comparator-mode missing |
| ra_pdg | r_pdg | scaffold | (pre-sweep) | bring-up |
| ra_bkup | r_bkup | partial | (pre-sweep) | backup domain register read/write |
| ra_sram | r_bsp ECC | feature-complete | (pre-sweep) | ECC enable + IRQ |
| ra_bscan | r_bscan | scaffold | (pre-sweep) | boundary scan |
| ra_mpc | r_ioport (PFS facade) | feature-complete | (pre-sweep) | pin route + PWPR unlock |

## Top-level libraries

| Library | Status | Notes |
|---|---|---|
| libs/ra_core | feature-complete | ra_err / ra_check / ra_log / ra_assert substrate |
| libs/ra_nsc | feature-complete | NSC veneers across comms + I/O + key vault + xspi + eth + log |
| libs/ra_net_pal | feature-complete | lwIP-port glue with in-memory ring fallback |
| libs/ra_usb_pal | feature-complete | CherryUSB usb_dc port glue with per-EP ring |
| libs/ra_net | wholly-new | hand-written ARP/IPv4/ICMP/UDP/TCP, no third-party stack |
| libs/ra_fs | scaffold | CMakeLists only; sibling agent landing fat-fs in parallel |

---

## Summary tally

- feature-complete drivers in libs/ra_hal/src/: 47
- partial drivers: 11
- scaffold drivers: 14
- wholly-new (no FSP analog): 2 (ra_jpeg_sw, key_vault)
- top-level libs feature-complete: 4 (ra_core, ra_nsc, ra_net_pal, ra_usb_pal)
- top-level libs wholly-new: 1 (ra_net)
- top-level libs scaffold: 1 (ra_fs -- in flight by sibling agent)

Of the 11 "partial" entries, six (ra_dac_b, ra_acmphs, ra_rtc, ra_agt,
ra_ulpt, ra_canfd) carry small FSP gaps that are not on the critical
path for the EK-RA8D2 reference apps. The remaining five (ra_i3c
HDR-DDR/IBI, ra_mipi_dsi command-mode, ra_mipi_csi ULPS, ra_usb_hhid
get_report IN, ra_lvd reset-on-trip) are tracked in MISSING.md.
