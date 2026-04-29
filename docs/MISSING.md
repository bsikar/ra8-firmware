# Missing / Incomplete Driver Surface

Audit date: 2026-04-29 (post-sweep-8 commit `5e154b9`).

This document inventories everything that is **NOT yet implemented or
not yet at FSP-grade feature parity** in `ra8d2-firmware`. Compiled by
walking `/Users/bsikar/Documents/github/_reference/fsp/ra/fsp/src/` and
diffing against `libs/ra_hal/src/` plus each driver's public header.

The cross-verification work in iterations 2-9 fixed register-layout
bugs and stood up native USB device + host class layers. Sweeps 1-8
brought the most important drivers to FSP-parity feature surfaces.
What remains is documented below; the rest moved to
`docs/DRIVER_STATUS.md`.

---

## 1. Drivers that were scaffolds but are now feature-complete

The following items from the previous version of this document were
landed in sweeps 1-8 and are no longer "missing":

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
  RSA + ECC + key-injection paths still pending.
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
  (`5e154b9`). Full host BLE stack still pending.
- USB host audio (UAC): landed in sweep 8 (`5e154b9`).
- Software JPEG codec (no FSP `r_jpeg` hardware on RA8D2): landed in
  sweep 8 (`5e154b9`).
- IPv4/ARP/ICMP/UDP/TCP stack (`libs/ra_net`): landed in sweep 8
  (`5e154b9`).
- Example apps `ethernet_tcp_echo`, `lcd_demo`, `usb_hid_device`,
  `usb_msc_device`, `usb_host_keyboard`, `usb_host_msc_browse`:
  landed in sweep 7 (`09abe38`).

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

### 2.3 MIPI-DSI / CSI (`libs/ra_hal/src/ra_mipi_*.c`)

Still missing:
- DSI command-mode payload transfer
- DSI ULPS (Ultra Low Power State) entry / exit
- CSI ULPS entry / exit
- Continuous-clock mode

### 2.4 USB host HID (`libs/ra_hal/src/ra_usb_hhid.c`)

`ra_usb_hhid_get_report` IN data phase still stubbed (NOLINT comment
present). Returns the SETUP only; does not collect the response
payload.

### 2.5 Flash / MRAM (`libs/ra_hal/src/ra_flash.c`)

Sweep 4 added erase/program/blank-check. Still missing:
- Suspend / resume during long ops (so a slow erase doesn't block IRQs)
- Code-bank-swap (if the silicon supports dual-bank)
- Background ops with completion callback
- Lock-bit programming for read protection

### 2.6 DAC_B, ACMPHS, RTC, AGT, ULPT, LVD, DOC, BKUP

Small FSP gaps remain on these (see the per-driver Notes column in
`docs/DRIVER_STATUS.md`). None are critical-path for the EK-RA8D2
reference apps.

---

## 3. Drivers entirely missing (FSP has them, we have nothing)

| FSP driver | What it does | Why we should care |
|---|---|---|
| `r_acmphs_b` | High-channel-count comparator | RA8D2 has > 4 comparator channels |
| `r_acmplp` | Low-power comparator | Wake-from-standby on analog event |
| `r_adc_d` | Differential ADC | RA8 supports differential mode |
| `r_can` | Classical CAN | Non-FD CAN bus interop |
| `r_cec` | HDMI-CEC | If LCD has HDMI input |
| `r_dac` | Legacy 12-bit DAC | Some examples need it |
| `r_dac8` | 8-bit DAC | Auxiliary analog out |
| `r_dsmif` | Delta-sigma demodulator | Motor current sensing |
| `r_flash_lp` | Low-power flash variant | Separate from MRAM path |
| `r_iic_b_slave` | IIC peripheral mode | Acting as an I2C slave |
| `r_iic_master` | Legacy IIC master | Older IP, some examples need it |
| `r_iic_slave` | Legacy IIC slave | |
| `r_iica_master` / `r_iica_slave` | yet another IIC variant | |
| `r_iirfa` | IIR Filter Accelerator | RA8-specific DSP block |
| `r_jpeg` (hw) | Hardware JPEG codec | RA8D2 silicon does not expose one; we ship `ra_jpeg_sw` |
| `r_kint` | Key Interrupt matrix | Matrix keyboards |
| `r_layer3_switch` | L3 packet switch | If multiple GMACs in use |
| `r_opamp` | Op-amp routing | Analog signal conditioning |
| `r_qspi` | Legacy QSPI | Pre-OSPI Flash devices |
| `r_rmac_phy` | Reduced-MAC PHY | Network configuration |
| `r_rsip_key_injection` | Secure key injection | TrustZone key management |
| `r_rsip_protected` | Protected RSIP ops | We have `ra_rsip.c` but it's the unprotected path |
| `r_rtc_c` | Calendar-mode RTC | Alternate to current calendar mode |
| `r_sau_*` | SAU sub-protocols (i2c, lin, spi, uart) | Some RA8 variants |
| `r_sce_key_injection`, `r_sce_protected` | Protected SCE ops | We have base `ra_sce`; protected paths pending |
| `r_slcdc` | Segment LCD controller | If display is segment LCD |
| `r_tau`, `r_tau_pwm` | Timer Array Unit | Higher-channel-count timers |
| `r_tml` | Timer Module Library | Higher-level timer abstractions |
| `r_uarta` | UARTA | Pre-SCI legacy UART |

---

## 4. USB stack residual gaps

### 4.1 Device side

We now have `ra_usb_cdc`, `ra_usb_phid`, `ra_usb_pmsc`,
`ra_usb_composite`. Missing:

- `r_usb_paud` -> device Audio class (UAC 1.0 / 2.0). USB headset out.
- `r_usb_pprn` -> device Printer class.
- `r_usb_pvnd` -> device Vendor-defined (raw bulk-only).

### 4.2 Host side

We have `ra_usb_hcdc`, `ra_usb_hmsc`, `ra_usb_hhid`,
`ra_usb_hcdc_ecm`, `ra_usb_haud`. Missing:

- `ra_usb_hhid_get_report` IN data phase still stubbed (see partial
  list above).
- Hub support. Today the host stack assumes one device on the root
  port; we don't enumerate hub topology.

---

## 5. Subsystems still without driver coverage

### 5.1 BLE host stack

`ra_ble` lands the controller bring-up + HCI ring in sweep 8 but the
GATT / L2CAP / ATT / SM host stack on top is not written. A real BLE
peripheral demo will need that host stack first.

### 5.2 Filesystem

`libs/ra_fs/` is currently a CMakeLists scaffold. A FAT / FATFS
implementation is in flight by a sibling agent.

### 5.3 TLS

There is no TLS layer on top of `libs/ra_net`. A sibling agent is
landing this in parallel.

### 5.4 Font rendering

A sibling agent is landing a font-rendering helper for `lcd_demo`.

### 5.5 DRW (Display Renderer)

`ra_drw.c` exists as a register-layout scaffold but is not stitched
into the GLCDC frame pipeline. 2D acceleration handoff TBD.

### 5.6 Multi-core IPC

`ra_ipc.c` is a stub. The Cortex-M33 secondary core does not yet
have a dispatch surface from M85 code.

### 5.7 Trace / debug

4-bit ETM trace port, boundary scan beyond raw register dump --
no driver, no example.

---

## 6. Things tested only at the simulator level

The host-side test corpus (now > 130 cases) is register-write
semantics + happy-path flows in a memory-mapped simulator.
Real-world coverage gaps:

- Driver behavior under back-to-back calls (state reset between)
- Driver interaction with ICU interrupt latency
- Driver behavior when MSTP is gated off mid-operation
- DMAC + driver pairing under load
- Multi-core IPC stress

---

## 7. Examples we should still have but don't

Have: `blink`, `blink_hal`, `clock_check`, `uart_hello`,
`usb_cdc_echo`, `usb_host_cdc_echo`, `ethernet_tcp_echo`, `lcd_demo`,
`usb_hid_device`, `usb_msc_device`, `usb_host_keyboard`,
`usb_host_msc_browse`.

Still missing demo apps:
- Camera capture from OV5640 through PDC + GLCDC display
- Audio loopback through SSIE
- USB MIDI device (uses `r_usb_pvnd` we don't have)
- Motor control via three-phase GPT (driver landed; example pending)
- Capacitive-touch demo via CTSU
- Hardware AES / SHA via SCE
- Bluetooth Low Energy peripheral (controller landed; host stack pending)
- Sigma-delta ADC current sensing
- IEEE 1588 PTP master / slave (driver landed; example pending)
- HyperFlash boot via xSPI XIP

---

## 8. Summary

Snapshot at the close of sweep 8:

- 47 drivers in `libs/ra_hal/src/` are feature-complete vs FSP parity.
- 11 drivers remain partial (small gaps -- see DRIVER_STATUS.md).
- 14 drivers remain scaffold (register layout + bring-up only --
  ra_drw, ra_etha, ra_vin, ra_tsn, ra_rmac, ra_ssie, ra_rsip,
  ra_dotf, ra_cnecc, ra_ipc, ra_pdg, ra_bscan, ra_ble (host stack),
  ra_ceu).
- 12 example apps flash to hardware (was 6 at start of sweep 7).
- 4 top-level libs feature-complete (`ra_core`, `ra_nsc`,
  `ra_net_pal`, `ra_usb_pal`).
- 1 wholly-new top-level lib (`libs/ra_net` -- IPv4 stack).
- `libs/ra_fs`, TLS, font rendering: in flight by sibling agents.

The "everything FULLY implemented" target -- every FSP `r_*` driver
matched, every silicon block driven from an example, every veneer
exercised end-to-end on the EVM -- still needs roughly:

- 5-8 more sweeps of HAL completion (the residual partial drivers
  in section 2 plus the device-class USB gaps in section 4.1).
- 6-10 more sweeps of new drivers (the table in section 3).
- 10-15 more example apps (section 7).

Total estimated effort: 25-35 more sweeps to reach FSP feature
parity end-to-end. Each sweep is ~25 minutes of work in this codebase.
