# Missing / Incomplete Driver Surface

Audit date: 2026-04-29 (post-iter-9 commit `4bb7cea`).

This document inventories everything that is **NOT yet implemented or
not yet at FSP-grade feature parity** in `ra8d2-firmware`. Compiled by
walking `/Users/bsikar/Documents/github/_reference/fsp/ra/fsp/src/` and
diffing against `libs/ra_hal/src/` plus each driver's public header.

The cross-verification work in iterations 2-9 fixed register-layout
bugs and stood up native USB device + host class layers. It did NOT
bring driver public APIs to feature parity with FSP. That work remains.

---

## 1. Drivers we have, but as scaffolds (real bugs gone, but missing
   feature surface)

### 1.1 SCI / UART (`libs/ra_hal/src/ra_sci.c`)

Have:
- `ra_sci_init`, `ra_sci_deinit`
- `ra_sci_putc_polling` / `ra_sci_getc_polling` / `ra_sci_write_polling`
- `ra_sci_attach_rx_handler` / `ra_sci_attach_tx_handler` (ISR-driven)
- `ra_sci_set_baud`, `ra_sci_get_errors`, `ra_sci_clear_errors`
- `ra_sci_enter_stop` / `ra_sci_exit_stop`

Missing vs FSP `r_sci_b_uart`:
- `R_SCI_B_UART_Read` / `_Write` async APIs returning per-byte progress
- `R_SCI_B_UART_ReadStop` with remaining-bytes callback
- `R_SCI_B_UART_Abort(uart_dir_t)` to cancel TX or RX in flight
- `R_SCI_B_UART_ReceiveSuspend` / `_Resume` (flow control)
- `R_SCI_B_UART_BaudCalculate` (compute BRR from desired rate + clock)
- `R_SCI_B_UART_InfoGet` (returns clock source, baud, transfer dir)
- `R_SCI_B_UART_CallbackSet` (runtime callback rebind)
- DMAC-backed read/write for high-throughput streams

Missing peer drivers (SCI in non-UART mode):
- `r_sci_b_lin` (LIN protocol)
- `r_sci_b_i2c` (SCI as I2C master/slave)
- `r_sci_b_spi` (SCI as SPI master/slave)
- `r_sci_b_smci` (Smart Card interface)

### 1.2 IIC / I2C (`libs/ra_hal/src/ra_iic_b.c`)

Have:
- `ra_iic_b_init`, `ra_iic_b_deinit`
- `ra_iic_b_set_clock`
- `ra_iic_b_scan` (single-byte address probe)
- `ra_iic_b_get_errors`

**Critically missing**: there is no `ra_iic_b_write` / `ra_iic_b_read`
in the public header. The driver cannot move bytes on the bus today.

Missing vs FSP `r_iic_b_master`:
- `R_IIC_B_MASTER_Read(addr, buf, len, restart)`
- `R_IIC_B_MASTER_Write(addr, buf, len, restart)`
- `R_IIC_B_MASTER_Abort` (in-flight cancel)
- `R_IIC_B_MASTER_SlaveAddressSet` (10-bit / 7-bit, addressing-mode select)
- `R_IIC_B_MASTER_StatusGet` (busy / event)
- `R_IIC_B_MASTER_CallbackSet`

Missing entirely:
- `r_iic_b_slave` (IIC peripheral mode)
- 10-bit addressing
- General-call address handling
- High-speed mode (3.4 MHz)

### 1.3 SPI (`libs/ra_hal/src/ra_spi_b.c`)

Have:
- `ra_spi_init`, `ra_spi_deinit`
- `ra_spi_master_init`
- `ra_spi_xfer8` (single-byte simultaneous TX+RX)
- `ra_spi_set_clock`, `ra_spi_get_errors`, `ra_spi_clear_errors`

Missing vs FSP `r_spi_b`:
- `R_SPI_B_Read(buf, len, bit_width)` async, 8/16/32-bit
- `R_SPI_B_Write(buf, len, bit_width)` async
- `R_SPI_B_WriteRead(tx, rx, len, bit_width)` full-duplex multi-byte
- `R_SPI_B_CallbackSet`
- DMAC-backed transfers (FSP `R_SPI_B_DTC_*`)

Missing entirely:
- SPI slave mode
- TI synchronous SPI mode
- Microwire mode
- 4-wire vs 3-wire (CS-as-data) configuration

### 1.4 I3C (`libs/ra_hal/src/ra_i3c.c`)

Have:
- `ra_i3c_init`, `ra_i3c_deinit`
- `ra_i3c_set_address`, `ra_i3c_bus_enable`
- `ra_i3c_get_status` / `_clear_status` / `_attach_handler`

What FSP provides that we don't (the driver is < 5 % feature-complete):
- Dynamic Address Assignment (ENTDAA / SETDASA / RSTDAA / SETNEWDA)
- Common Command Codes (CCC) engine: ENTHDR0/1, ENEC, DISEC, ENTAS0..3,
  RSTACT, GETSTATUS, GETBCR, GETDCR, GETPID, GETMRL/MWL/XTIME
- In-Band Interrupt (IBI) inbound queue (IBINCTL / NTIBIQP / NTIBIVCTL)
- HDR-DDR (Double Data Rate) mode
- FIFO-port command queue NCMDQP / NTDTBP0 / NTDTBP1 / NTSTOPCQ
- Device address table (DATBASn, DEVCTL)
- Slave-mode operation entirely
- Hot-join handling

### 1.5 ADC (`libs/ra_hal/src/adc.c`)

Have:
- `ra_adc_init`, `ra_adc_init_configured`, `ra_adc_deinit`
- `ra_adc_read_channel(ch)` single-shot one channel
- `ra_adc_set_resolution`
- `ra_adc_get_status` / `_clear_status` / `_attach_handler`

Missing vs FSP `r_adc_b`:
- Scan group A / scan group B priority + interrupt routing
- Continuous-scan mode (free-running)
- Hardware trigger sources via ELC (start on GPT compare match etc.)
- Sample-and-hold groups (S&H register programming)
- Oversampling / accumulation modes (ADADC.AVEE bit)
- Comparator-window mode (compare data against ADCMPLR/ADCMPHR limits,
  fire interrupt on out-of-range)
- DMAC-backed scan-result transfer
- Self-diagnostic and offset calibration paths

Missing peer:
- `r_adc_d` (differential ADC)
- `r_sdadc` / `r_sdadc_b` (sigma-delta ADC)

### 1.6 DAC_B (`libs/ra_hal/src/ra_dac_b.c`)

Have (post iter 5 fixes):
- `ra_dac_b_init`, `ra_dac_b_deinit`
- `ra_dac_b_write(ch, value)`
- `ra_dac_b_set_vref`, `ra_dac_b_set_output_enable`

Missing:
- Synchronized two-channel update (DAE / batch convert mode)
- ADC-trigger-driven sample updates (ADC_SYNC bit)
- Charge-pump enable (DACR0.DAOUTEN per FSP)
- Internal-output route (use DAC output as ADC input without pin)

Missing peers:
- `r_dac` (legacy 12-bit DAC)
- `r_dac8` (8-bit auxiliary DAC)

### 1.7 GLCDC (`libs/ra_hal/src/ra_glcdc.c`)

Have:
- `ra_glcdc_init`, `ra_glcdc_start`, `ra_glcdc_deinit`
- `ra_glcdc_set_buffer`, `ra_glcdc_set_layer`, `ra_glcdc_set_clut`
- `ra_glcdc_get_status` / `_clear_status` / `_attach_handler`

Missing:
- Layer-2 graphics layer (alpha-blended overlay)
- Background-color register programming
- CLUT runtime update with double-buffering (avoid glitch during update)
- Color-space conversion (RGB <-> YCbCr) registers
- Brightness / contrast / gamma correction registers
- Dithering matrix programming
- Output color reduction (24bpp -> 18bpp / 16bpp panel)
- Vertical/horizontal scaling (VSCAL / HSCAL)
- Panel-clock fine adjustment (DCDR full set of dividers)
- Detect-panel + hot-plug handling
- Direct 2D acceleration handoff to DRW (we have `ra_drw.c` but it's
  not stitched into GLCDC's frame pipeline)

### 1.8 GPT (`libs/ra_hal/src/ra_gpt.c`)

Have:
- `ra_gpt_init`, `ra_gpt_start`, `ra_gpt_stop`
- `ra_gpt_get_status` / `_clear_status` / `_attach_handler`

Missing vs FSP `r_gpt`:
- `R_GPT_PeriodSet` runtime period change
- `R_GPT_DutyCycleSet` runtime duty-cycle change
- `R_GPT_CounterSet` (force-load count)
- PWM output configuration on GTIOCnA / GTIOCnB pins (output disable
  on POEG fault, polarity, dead-time)
- Three-phase synchronized output mode (FSP `r_gpt_three_phase`)
- Hardware noise filter on input capture
- Phase-counting mode (encoder input)
- Synchronous start / stop across multiple channels
- One-shot pulse output mode
- DMAC trigger on capture event

### 1.9 SDHI (`libs/ra_hal/src/ra_sdhi.c`)

Have (post iter 4 fixes):
- `ra_sdhi_init`, `ra_sdhi_deinit`
- `ra_sdhi_send_command`, `ra_sdhi_set_clock`
- `ra_sdhi_get_status`, `ra_sdhi_clear_status`

**Critically missing**: data-phase block transfer. The driver can issue
SD CMD0..CMD55 control commands but cannot READ_SINGLE_BLOCK / WRITE_
SINGLE_BLOCK / READ_MULTIPLE_BLOCK / WRITE_MULTIPLE_BLOCK. Without this
SDHI cannot move sector data to / from an SD card.

Missing vs FSP `r_sdhi`:
- Block-transfer engine driving SD_BUF0 in 4-byte chunks
- DMAC-backed read/write (SD_DMAEN)
- SDIO interrupt routing through SDIO_INFO1
- SD-card identification protocol stack (CMD0 -> CMD8 -> ACMD41 ->
  CMD2 -> CMD3 -> SCR/CSD parse)
- Bus-width switch CMD6 (SD_OPTION.WIDTH / WIDTH8)
- Speed switch CMD6 (default / high-speed / SDR50 / DDR50 / SDR104)
- Card-detect signal (CD pin level + GP-IO interrupt)
- Write-protect signal (WP pin level)
- Card removal recovery

### 1.10 CANFD (`libs/ra_hal/src/ra_canfd.c`)

Have (post iter 3 fixes):
- `ra_canfd_init`, `ra_canfd_deinit`
- `ra_canfd_send_msg`, `ra_canfd_recv_msg` (FIFO 0 only)
- `ra_canfd_get_status`, `_clear_status`, `_attach_handler`

Missing:
- RX-FIFO interrupt model (we poll today)
- Classical CAN (non-FD) compatibility mode
- Error-passive / bus-off detection + recovery
- Transmit-mailbox queue (we send through TX-MB 0 only)
- Transmit-history queue
- GAFL (Global Acceptance Filter List) programming for hardware filtering
- Bit-rate-switching mode for CAN-FD payload phase
- CAN-FD ISO vs non-ISO mode select
- Time-triggered transmission (TTCAN)

Missing peer:
- `r_can` (Classical-only CAN, separate IP block)

### 1.11 Ethernet stack (`libs/ra_hal/src/ra_eth*.c`)

Have:
- ETHA / GWCA / MFWD / COMA controller bring-up paths
- `ra_eth_*_get_status`, `_clear_status`, `_attach_handler`

**Critically missing**: send and receive descriptor rings. Cannot
transmit a frame today.

Missing vs FSP `r_ether`:
- TX descriptor ring setup + ring-pointer advance
- RX descriptor ring setup + ring-pointer advance
- Frame transmit `R_ETHER_Write(buf, len)` with completion callback
- Frame receive `R_ETHER_Read(buf, max_len, *got_len)`
- Buffer pool management (FSP uses ether_zerocopy_buf_alloc)
- PHY auto-negotiation state machine
- Multicast filter table (HASH_HW)
- MAC-address filter table
- Magic-packet wake-on-LAN
- IEEE 1588 PTP timestamp insertion (separate `r_ptp` driver missing)
- Pause-frame support
- Flow control RX/TX
- Statistics counters readout

Missing peers:
- `r_ptp` IEEE 1588 Precision Time Protocol
- `r_ether_phy`  PHY-side abstraction
- `r_ethercat_phy` EtherCAT slave PHY
- `r_layer3_switch` L3 switching

### 1.12 OSPI / xSPI (`libs/ra_hal/src/ra_xspi.c`)

Have (post iter 2 fixes):
- Single-command issue path with FSP-aligned CDT layout
- Status-register read

Missing:
- Memory-mapped (XIP) read mode (CMRES register)
- Direct-read path (linear address window)
- DTR (double-data-rate) mode for 1.6x throughput on HyperFlash
- DQS (data strobe) calibration sequence
- OPI (Octal Peripheral Interface) handshake
- Per-die write enable / status polling for HyperFlash
- DMAC integration for high-throughput transfers
- Suspend / resume during long erase

Missing peer:
- `r_qspi` (legacy QSPI, RA8 may still expose for compatibility)

### 1.13 MIPI-DSI / CSI / PHY (`libs/ra_hal/src/ra_mipi_*.c`)

Have:
- PHY init with FSP-aligned RFREQ encoding (post iter 3 fix)
- DSI / CSI init scaffolds

Missing:
- DSI video-mode timing setup (HSA / HBP / HACT / HFP / VSA / VBP /
  VACT / VFP register programming)
- DSI command-mode payload transfer
- CSI-RX virtual-channel configuration
- ECC and CRC checking on incoming CSI frames
- Lane-swap configuration
- Continuous-clock mode
- ULPS (Ultra Low Power State) entry / exit
- Error reporting state machine

### 1.14 Flash (MRAM) (`libs/ra_hal/src/ra_flash.c`)

Have (post iter 5 fixes):
- ARC (Anti-Rollback Counter) read / increment
- Basic register layout

Missing vs FSP `r_mram`:
- Block-erase
- Page-program (write)
- Blank-check
- Suspend / resume during long ops (so a slow erase doesn't block IRQs)
- Code-bank-swap (if the silicon supports dual-bank)
- Background ops with completion callback
- Lock-bit programming for read protection

Missing peer:
- `r_flash_lp` (low-power flash, different command sequence)

### 1.15 DMAC (`libs/ra_hal/src/ra_dmac.c`)

Have:
- `ra_dmac_start`, `ra_dmac_stop`

Missing vs FSP `r_dmac`:
- Repeat-area transfer mode
- Block-transfer mode (multiple repeats, periodic)
- Extended-address mode (>4GB pseudo addressing)
- Source / destination address-update modes (fixed / inc / dec /
  offset-add)
- Half-block-complete interrupt
- Daisy-chained DMAC instances
- Callback registration

Missing peer:
- `r_dtc` exists in our tree as a scaffold but lacks the actual
  vector-table-driven transfer setup that FSP has

### 1.16 GPIO (`libs/ra_hal/src/gpio.c`)

Have:
- Output / input init, write / read, toggle

Missing:
- Edge / level interrupt configuration via ICU.IRQCR
- Open-drain output mode (NCODR PFS bit)
- Pull-up enable runtime (PCR PFS bit)
- Drive-strength setting (DSCR / DSCR2)
- Event Output (EOFR for ELC)
- Group I/O port atomic update (PCNTR1 single-write atomic)
- Pin-state retention through deep-software-standby

---

## 2. Drivers entirely missing (FSP has them, we have nothing)

| FSP driver | What it does | Why we should care |
|---|---|---|
| `r_acmphs_b` | High-channel-count comparator | RA8D2 has > 4 comparator channels |
| `r_acmplp` | Low-power comparator | Wake-from-standby on analog event |
| `r_adc_d` | Differential ADC | RA8 supports differential mode |
| `r_can` | Classical CAN | Non-FD CAN bus interop |
| `r_cec` | HDMI-CEC | If LCD has HDMI input |
| `r_ctsu` | Capacitive touch sensing | EK-RA8D2 has touch electrodes on board |
| `r_dac` | Legacy 12-bit DAC | Some examples need it |
| `r_dac8` | 8-bit DAC | Auxiliary analog out |
| `r_dsmif` | Delta-sigma demodulator | Motor current sensing |
| `r_flash_lp` | Low-power flash variant | Separate from MRAM path |
| `r_gpt_three_phase` | Three-phase motor PWM | Motor control demos |
| `r_iic_b_slave` | IIC peripheral mode | Acting as an I2C slave |
| `r_iic_master` | Legacy IIC master | Older IP, some examples need it |
| `r_iic_slave` | Legacy IIC slave | |
| `r_iica_master` | yet another IIC variant | |
| `r_iica_slave` | | |
| `r_iirfa` | IIR Filter Accelerator | RA8-specific DSP block |
| `r_jpeg` | Hardware JPEG codec | Camera / display pipelines |
| `r_kint` | Key Interrupt matrix | Matrix keyboards |
| `r_layer3_switch` | L3 packet switch | If multiple GMACs in use |
| `r_opamp` | Op-amp routing | Analog signal conditioning |
| `r_pdc` | Parallel Data Capture | Camera bridge (we have `ra_ceu.c` but no PDC) |
| `r_ptp` | IEEE 1588 PTP | Time-synchronized networking |
| `r_qspi` | Legacy QSPI | Pre-OSPI Flash devices |
| `r_rmac_phy` | Reduced-MAC PHY | Network configuration |
| `r_rsip_key_injection` | Secure key injection | TrustZone key management |
| `r_rsip_protected` | Protected RSIP ops | We have `ra_rsip.c` but it's the unprotected path |
| `r_rtc_c` | Calendar-mode RTC | Alternate to current calendar mode |
| `r_sau_i2c`, `r_sau_lin`, `r_sau_spi`, `r_sau_uart` | SAU sub-protocols | Serial Array Unit (some RA8 variants) |
| `r_sce`, `r_sce_key_injection`, `r_sce_protected` | Secure Crypto Engine | Hardware AES/SHA/RSA/ECC |
| `r_slcdc` | Segment LCD controller | If display is segment LCD |
| `r_tau`, `r_tau_pwm` | Timer Array Unit | Higher-channel-count timers |
| `r_tml` | Timer Module Library | Higher-level timer abstractions |
| `r_uarta` | UARTA | Pre-SCI legacy UART |

---

## 3. USB stack gaps

### 3.1 Device side

We have `ra_usb_cdc` (CDC ACM device class). Missing:

- `r_usb_phid` -> `ra_usb_phid` device HID class. **The EVM cannot
  enumerate as a USB keyboard / mouse / gamepad to a host yet.**
- `r_usb_pmsc` -> `ra_usb_pmsc` device MSC class. Cannot expose
  internal MRAM / SRAM / SD card as a USB drive.
- `r_usb_paud` -> device Audio class (UAC 1.0 / 2.0). USB headset out.
- `r_usb_pprn` -> device Printer class.
- `r_usb_pvnd` -> device Vendor-defined (raw bulk-only). Often used
  for application-specific protocols.
- `r_usb_composite` -> Multi-class composite device. Today our
  `ra_usb_cdc` assumes single-class.

### 3.2 Host side

We have `ra_usb_hcdc`, `ra_usb_hmsc`, `ra_usb_hhid`. Missing:

- `r_usb_haud` -> host Audio. Common for USB headphones / speakers.
- `r_usb_hcdc_ecm` -> CDC-ECM (Ethernet over USB!). Would let the
  EK-RA8D2 bring up a USB ethernet adapter.
- `ra_usb_hhid::ra_usb_hhid_get_report` IN data phase is deliberately
  stubbed with a NOLINT in iter 9. Returns the SETUP only; does not
  collect the response payload.
- Hub support. Today the host stack assumes one device on the root
  port; we don't enumerate hub topology.

### 3.3 USB Type-C / PD

`r_usb_typec` exists in FSP and the RA8D2 silicon has a Type-C
controller block. We have ZERO coverage of this. Implications:

- Cannot negotiate USB Power Delivery contracts
- Cannot detect Type-C orientation
- Cannot do Alt-Mode (DisplayPort over USB-C, etc.)
- Cannot act as a Type-C source or sink

### 3.4 BLE

`r_ble` exists in FSP. RA8D2 has a BLE radio block. We have nothing.

---

## 4. Things tested only at the simulator level

Our 91 host-side tests are register-write semantics + some happy-path
flows in a memory-mapped simulator. Real-world coverage gaps:

- Driver behavior under back-to-back calls (state reset between)
- Driver interaction with ICU interrupt latency
- Driver behavior when MSTP is gated off mid-operation
- DMAC + driver pairing: verify DMAC-backed reads / writes complete
  without losing bytes under load
- Multi-core IPC: RA8D2 has Cortex-M33 secondary core; `ra_ipc.c` is
  a stub
- Trace unit (4-bit ETM trace port on RA8) - no driver, no example
- Boundary-scan / debug beyond raw register dump

---

## 5. Examples we should have but don't

Have: `blink`, `blink_hal`, `clock_check`, `uart_hello`,
`usb_cdc_echo`, `usb_host_cdc_echo`.

Missing FSP-style demo apps:
- LCD demo on the EK-RA8D2 7-inch panel through GLCDC
- Camera capture from OV5640 through CEU + GLCDC display
- Audio loopback through SSIE
- USB MIDI device (uses `r_usb_pvnd` we don't have)
- USB HID composite mouse + keyboard
- USB MSC device exposing internal RAM as a fake drive
- USB host MSC reading a flash drive (we have the class layer in
  `ra_usb_hmsc.c`; need an example app)
- USB host HID printing keystrokes from a USB keyboard (we have the
  class layer in `ra_usb_hhid.c`; need an example app)
- Motor control via three-phase GPT
- Capacitive-touch demo via CTSU
- Hardware AES / SHA via SCE
- Bluetooth Low Energy peripheral
- Ethernet TCP/IP (lwIP integration on top of `ra_eth*`)
- Sigma-delta ADC current sensing
- IEEE 1588 PTP master / slave
- HyperFlash boot via xSPI XIP

---

## 6. Documentation gaps

- No driver matrix showing (peripheral) x (state: scaffold / partial /
  feature-complete) so users can tell at a glance what works
- No per-driver architecture doc explaining the mock-injection /
  ra_sim_mmap pattern for someone adding a new driver
- ROADMAP.md says "drivers=45 DONE=45" which is misleading - "DONE"
  means "register layout cross-verified", not "feature complete"
- Hardware test instructions only exist for 2 of 6 example apps

---

## 7. Priority order if we resume implementation

If the goal is "everything FULLY implemented":

**Highest priority** (drivers that already exist but are unusable as-is):
1. IIC: add `read` / `write` / `transfer` to public API
2. SDHI: add block-transfer data phase
3. Ethernet: add TX / RX descriptor rings
4. SPI: add multi-byte read / write
5. SCI: add async read / write returning per-byte progress

**Medium priority** (drivers we have but with major gaps):
6. ADC: scan groups + ELC-triggered scans
7. GPT: PWM duty-cycle / period runtime change + three-phase mode
8. GLCDC: layer-2 + CLUT double-buffering + scaling
9. I3C: CCC engine + dynamic address assignment
10. Flash: erase / write / blank-check

**USB completion** (each is its own driver):
11. Device HID
12. Device MSC
13. USB composite device
14. USB host CDC-ECM
15. USB Type-C / PD

**New drivers** (FSP has, we have nothing):
16. CTSU (capacitive touch)
17. PDC (parallel camera bridge)
18. SCE (hardware crypto)
19. PTP (IEEE 1588)
20. JPEG codec
21. BLE stack

**Examples**:
22. lcd_demo
23. camera_capture
24. usb_msc_device
25. usb_host_msc_browse
26. usb_host_keyboard
27. ethernet_tcp_echo

Each of (1) through (5) is a one-iteration project. Each of (11)
through (15) is a one-iteration project. New drivers (16) through (21)
are likely two iterations each. Total estimated effort: 30-40 more
iterations to reach FSP feature parity, plus 10-15 iterations for the
example apps.

---

## 8. Summary

What we have: 22 drivers cross-verified for register layout, full native
USB device-CDC + host-CDC/MSC/HID class scaffolding, 6 hardware-flashable
example apps, 91 simulator-level tests.

What "FULLY implemented" would look like: the table above filled in,
driver public APIs that match FSP's parity, USB device classes for HID
and MSC, USB Type-C support, hardware crypto, and example apps that
exercise each peripheral on the EK-RA8D2's actual hardware.

Gap estimate: roughly 40-55 more cross-verify-cadence iterations to
close. The cross-verify loop has been at ~25 minutes per iteration.
