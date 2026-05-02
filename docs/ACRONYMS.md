# RA8D2 Acronym Glossary

Alphabetical glossary of every chip-, board-, and Cortex-M85-specific
acronym used in this codebase. Each entry gives a one-line expansion
and the HAL driver under `libs/ra_hal/src/` that implements the
peripheral (when applicable). Entries grouped by category for browsing,
then a flat alphabetical index at the bottom.

Acronym scope: only acronyms that actually appear in
`libs/ra_hal/inc/`, `libs/ra_hal/src/`, or `examples/` source. Where an
acronym means different things in different vendors' docs, the
expansion below is the one Renesas uses in HUM R01UH1065EJ.

## 1. Clocks, power, reset

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| CGC   | Clock Generation Circuit                                | `ra_cgc.c` |
| CAC   | Clock-frequency Accuracy-measurement Circuit            | `ra_cac.c` |
| LPM   | Low Power Mode controller                               | `ra_lpm.c` |
| LVD   | Low-Voltage Detection                                   | `ra_lvd.c` |
| MSTP  | Module-Stop control (clock-gating)                      | `ra_mstp.c` |
| OFS   | Option-Function Select (boot configuration words)       | `ra_ofs.c` |
| PWR   | Power-management glue                                   | `ra_pwr.c` |
| RESET | Reset controller (RSTSR1/2 + cold/warm flags)           | `ra_reset.c` |
| SYSC  | SYSTEM Controller (R_SYSTEM register block)             | (used by `ra_pwr.c`, `ra_reset.c`, `ra_vreg.c`, `ra_lpm.c`) |
| VBATT | Battery-backup domain (VBATT pin / VBTBKR registers)    | `ra_bkup.c` |
| VREG  | Internal voltage regulator                              | `ra_vreg.c` |
| BKUP  | Battery-backup function (alias for VBATT block)         | `ra_bkup.c` |

## 2. IO and pin-mux

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| GPIO  | General-Purpose Input / Output                            | `gpio.c` |
| PORT  | Parallel I/O port (PORT0..PORT14 register banks)          | `gpio.c` |
| PFS   | Pin Function Select (per-pin alternate-function register) | `gpio.c` |
| PSEL  | Pin Select (PFS bitfield choosing the alternate function) | `gpio.c` |
| PMR   | Port Mode Register (digital vs peripheral)                | `gpio.c` |
| PDR   | Port Direction Register                                   | `gpio.c` |
| PODR  | Port Output Data Register                                 | `gpio.c` |
| PIDR  | Port Input Data Register                                  | `gpio.c` |
| PWPR  | Pin Write-Protect Register (PFS unlock)                   | `gpio.c` |
| PWPRS | Secure Pin Write-Protect Register                         | `gpio.c` |
| PMISC | Pin Miscellaneous (contains PWPR/PWPRS)                   | `gpio.c` |
| MPC   | Multi-function Pin Controller                             | `ra_mpc.c` |
| ELC   | Event Link Controller (peripheral-to-peripheral events)   | `ra_elc.c` |
| ICU   | Interrupt Controller Unit                                 | `ra_icu.c` |
| ISR   | Interrupt Service Routine (HAL ISR-table glue)            | `ra_isr.c` |
| IRQ   | Interrupt Request line (NVIC vector entry)                | `ra_icu.c` |
| WUPEN | Wake-Up Enable register                                   | `ra_lpm.c` |

## 3. Communication

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| SCI   | Serial Communications Interface (UART/I2C/SPI super-mode) | `ra_sci.c`, `uart.c` |
| UART  | Universal Asynchronous Receiver/Transmitter               | `uart.c` |
| SPI   | Serial Peripheral Interface (controller/peripheral)       | `ra_spi_b.c` |
| IIC_B | I2C bus controller, version B                             | `ra_iic_b.c`, `ra_iic_b_slave.c` |
| I3C   | Improved Inter-Integrated Circuit (MIPI I3C)              | `ra_i3c.c` |
| SMBUS | System Management Bus (I2C-compatible)                    | `ra_smbus.c` |
| CANFD | Controller Area Network with Flexible Data-rate           | `ra_canfd.c` |
| CNECC | CAN Message-RAM ECC controller                            | `ra_cnecc.c` |
| USB FS| USB Full-Speed (12 Mbps)                                  | `ra_usb.c`, `ra_usb_*.c` |
| USB HS| USB High-Speed (480 Mbps)                                 | `ra_usb.c`, `ra_usb_*.c` |
| CDC   | USB Communications Device Class (virtual COM)             | `ra_usb_cdc.c`, `ra_usb_hcdc.c`, `ra_usb_hcdc_ecm.c` |
| HID   | USB Human Interface Device                                | `ra_usb_phid.c`, `ra_usb_hhid.c` |
| MSC   | USB Mass Storage Class                                    | `ra_usb_pmsc.c`, `ra_usb_hmsc.c` |
| HHUB  | USB Host Hub class driver                                 | `ra_usb_hhub.c` |
| PVND  | USB Peripheral Vendor-class                               | `ra_usb_pvnd.c` |
| PAUD/HAUD | USB Peripheral / Host Audio class                     | `ra_usb_paud.c`, `ra_usb_haud.c` |
| PPRN  | USB Peripheral Printer class                              | `ra_usb_pprn.c` |
| ETHA  | Ethernet adapter (gigabit MAC top-level)                  | `ra_etha.c`, `ra_eth.c` |
| RMAC  | Reduced Media Access Controller (per-port MAC)            | `ra_rmac.c`, `ra_rmac_phy.c` |
| GWCA  | GateWay CPU Agent (Ethernet DMA gateway)                  | `ra_eth_gwca.c` |
| MFWD  | MAC ForWarDing engine                                     | `ra_eth_mfwd.c` |
| ESWM  | Ethernet SWitch Management                                | `ra_layer3_switch.c` |
| GPTP  | Generic Precision Time Protocol block                     | `ra_eth_gptp.c` |
| PTP   | Precision Time Protocol (IEEE 1588) SYNFP/STCA            | `ra_ptp.c` |
| TSN   | Time-Sensitive Networking                                 | `ra_tsn.c` |
| PHY   | Physical-layer transceiver (Ethernet PHY)                 | `ra_ether_phy.c`, `ra_rmac_phy.c` |
| BLE   | Bluetooth Low Energy controller (with patch RAM)          | `ra_ble.c`, `ra_ble_patch.c` |
| IPC   | Inter-Processor Communication (M85 <-> M33 mailbox)       | `ra_ipc.c` |

## 4. Crypto and secure-storage

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| RSIP  | Renesas Secure IP (HW crypto + key vault, RSIP-E50D)     | `ra_rsip.c`, `ra_rsip_protected.c`, `ra_rsip_key_injection.c` |
| DOTF  | Decryption-On-The-Fly (XIP-decrypt for xSPI)             | `ra_dotf.c` |
| CRC   | Cyclic-Redundancy-Check engine                           | `ra_crc.c` |
| DOC   | Data Operation Circuit (compare/add for tamper checks)   | `ra_doc.c` |
| MMPU  | Bus-Master Memory Protection Unit                        | (HAL init only) |
| CPSCU | Security Control Unit (per-peripheral S/NS attribution)   | `ra_lvd.c`, `ra_sram.c` |
| BBFSAR| Battery-Backup Full Security Attribute Register          | `ra_bkup.c` |

## 5. Display, video, graphics

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| GLCDC   | Graphics LCD Controller (parallel-RGB output)           | `ra_glcdc.c` |
| DRW     | 2D DRaWing engine (DAVE-2D core)                        | `ra_drw.c` |
| MIPI DSI| MIPI Display Serial Interface                           | `ra_mipi_dsi.c` |
| MIPI CSI| MIPI Camera Serial Interface                            | `ra_mipi_csi.c` |
| MIPI PHY| Shared MIPI D-PHY                                       | `ra_mipi_phy.c` |
| CEU     | Capture Engine Unit (parallel-camera input)             | `ra_ceu.c` |
| VIN     | Video INput module                                      | `ra_vin.c` |
| JPEG_SW | Software JPEG codec (no JPEG HW IP on RA8D2)            | `ra_jpeg_sw.c` |
| EPAPER  | E-Paper / EPD framebuffer driver                        | `ra_epaper.c` |
| TCON    | Timing CONtroller (GLCDC TCON0..3 outputs)              | `ra_glcdc.c` |

## 6. Audio

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| SSIE  | Serial Sound Interface Enhanced (I2S/TDM)                 | `ra_ssie.c` |
| PDM   | Pulse-Density Modulation microphone interface             | `ra_pdm.c` |
| DAI   | Digital Audio Interface (CODEC-side I2S signals)          | (used in board pin-mux) |

## 7. Analog

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| ADC_B | Analog-to-Digital Converter, version B                    | `adc.c` |
| DAC_B | Digital-to-Analog Converter, version B                    | `ra_dac_b.c` |
| ACMPHS| High-Speed Analog Comparator                              | `ra_acmphs.c` |

## 8. Timers, motor / power

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| GPT   | General PWM Timer (32-bit, motor / general-purpose)       | `ra_gpt.c`, `timer.c` |
| GTIOC | GPT IO Channel pin (GTIOCnA/B output)                     | `ra_gpt.c` |
| AGT   | Asynchronous General-purpose Timer (16-bit)               | `ra_agt.c` |
| ULPT  | Ultra-Low-Power Timer                                     | `ra_ulpt.c` |
| POEG  | Port Output Enable for GPT (motor-fault shut-off)         | `ra_poeg.c` |
| PDG   | Phase Delay Generator (multi-channel motor sync)          | `ra_pdg.c` |
| WDT   | Watchdog Timer                                            | `ra_wdt.c` |
| IWDT  | Independent Watchdog Timer                                | `ra_iwdt.c` |
| RTC   | Real-Time Clock                                           | `ra_rtc.c` |

## 9. Memory / storage

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| MRAM  | Magnetoresistive RAM (1 MiB on-chip, code memory)         | `ra_flash.c` |
| MRMS  | MRAM Module Sequencer (MRAM controller)                   | `ra_flash.c` |
| SRAM  | Static RAM (2 MiB on-chip, ECC-protected)                 | `ra_sram.c` |
| ECC   | Error-Correcting Code (SRAM/MRAM single-bit correction)   | `ra_sram.c`, `ra_cnecc.c` |
| DTCM  | Data Tightly-Coupled Memory                               | (linker only) |
| ITCM  | Instruction Tightly-Coupled Memory                        | (linker only) |
| TCM   | Tightly-Coupled Memory (umbrella for ITCM + DTCM)         | (linker only) |
| SDRAM | Synchronous Dynamic RAM (external, 64 MiB on EK)          | `ra_sdramc.c` |
| SDRAMC| SDRAM Controller                                          | `ra_sdramc.c` |
| OSPI  | Octo-SPI (Renesas register block name)                    | `ra_xspi.c` |
| XSPI  | eXpanded SPI (xSPI = HUM term for the OSPI controller)    | `ra_xspi.c` |
| XIP   | eXecute-In-Place (memory-mapped read of external flash)   | `ra_xspi.c` |
| FLASH | Generic flash controller surface                          | `ra_flash.c` |
| SDHI  | SD Host Interface                                         | `ra_sdhi.c`, `ra_sdcard.c` |
| DMA   | Direct Memory Access (top-level umbrella)                 | `ra_dma.c` |
| DMAC  | Direct Memory Access Controller                           | `ra_dmac.c` |
| DTC   | Data Transfer Controller (lighter-weight than DMAC)       | `ra_dtc.c` |
| DOTF  | Decryption-On-The-Fly (covered under crypto above)        | `ra_dotf.c` |
| DPDM  | (not used in this tree)                                   | -- |

## 10. Debug, test, NVIC / core

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| BSCAN | Boundary Scan controller                                  | `ra_bscan.c` |
| HW ERR| Hardware-Error reporter                                   | `ra_hw_err.c` |
| MMIO  | Memory-Mapped I/O (generic term, not a Renesas IP)        | -- |
| NVIC  | Nested Vectored Interrupt Controller (Cortex-M core)      | (used by `ra_icu.c`) |
| SCB   | System Control Block (Cortex-M core)                      | (used by HAL fault handlers) |
| MPU   | Memory Protection Unit (core MPU at `0xE000ED90`)         | (HAL init) |
| MMPU  | Bus-Master MPU (chip-level, distinct from core MPU)       | (HAL init) |
| FPU   | Floating-Point Unit (Cortex-M85 single+double precision)  | (toolchain flags) |
| MVE   | M-profile Vector Extension (a.k.a. Helium)                | (toolchain flags) |
| Helium| ARM marketing name for MVE                                | (toolchain flags) |
| SAU   | Security Attribution Unit (TrustZone partitioning)        | per-app `trustzone_init.c` |
| NSC   | Non-Secure Callable (TrustZone veneers)                   | `libs/ra_nsc/` |

## 11. Touch and sensors

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| TOUCH | Capacitive-touch driver (GT911 panel, parallel TFT)       | `ra_touch.c` |

## 12. Vendor / family acronyms

| Acronym | Expansion |
|---------|-----------|
| RA      | Renesas Advanced (32-bit Arm-based MCU family) |
| RA8D2   | RA8 family, "D" = Display-class, group 2 |
| FSP     | Flexible Software Package (Renesas reference SDK; not in this tree) |
| HUM     | Hardware User's Manual (R01UH1065EJ) |
| EK      | Evaluation Kit (EK-RA8D2 v1 board) |
| OB      | On-Board (J-Link OB on the EK) |
| Pmod    | Digilent Peripheral Module connector |
| TFT     | Thin-Film-Transistor LCD panel |

## Flat alphabetical index

ACMPHS, ADC_B, AGT, BBFSAR, BKUP, BLE, BSCAN, CAC, CANFD, CDC, CEU,
CGC, CNECC, CPSCU, CRC, DAC_B, DAI, DMA, DMAC, DOC, DOTF, DRW, DTC,
DTCM, ECC, EK, ELC, EPAPER, ESWM, ETHA, FLASH, FPU, FSP, GLCDC, GPIO,
GPT, GPTP, GTIOC, GWCA, HAUD, Helium, HID, HUM, HW ERR, I3C, ICU, IIC_B,
IPC, IRQ, ISR, ITCM, IWDT, JPEG_SW, LPM, LVD, MFWD, MIPI CSI, MIPI DSI,
MIPI PHY, MMIO, MMPU, MPC, MPU, MRAM, MRMS, MSC, MSTP, MVE, NSC, NVIC,
OFS, OSPI, PAUD, PDG, PDM, PFS, PHY, Pmod, PMISC, PMR, PODR, POEG,
PORT, PPRN, PSEL, PTP, PVND, PWPR, PWPRS, PWR, RA, RA8D2, RESET, RMAC,
RSIP, RTC, SAU, SCB, SCI, SDHI, SDRAM, SDRAMC, SMBUS, SPI, SRAM, SSIE,
SYSC, TCM, TCON, TFT, TOUCH, TSN, UART, ULPT, USB FS, USB HS, VBATT,
VIN, VREG, WDT, WUPEN, XIP, XSPI.
