# RA8D2 Acronym Glossary

Glossary of the chip-, board-, and Cortex-M85-specific acronyms used in
this codebase. Each entry gives a one-line expansion and the HAL driver
under `libs/ra8_hal/src/` that implements the peripheral (when
applicable). Entries are grouped by category for browsing.

Acronym scope: only acronyms that actually appear in
`libs/ra8_hal/inc/`, `libs/ra8_hal/src/`, or `examples/` source. Where an
acronym means different things in different vendors' docs, the
expansion below is the one Renesas uses in HUM R01UH1065EJ.

## 1. Clocks, power, reset

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| CGC   | Clock Generation Circuit                                | `ra8_cgc.c` |
| CAC   | Clock-frequency Accuracy-measurement Circuit            | `cac.zig`   |
| LPM   | Low Power Mode controller                               | `lpm_abi.zig` |
| LVD   | Low-Voltage Detection                                   | `internal/lvd.zig` |
| MSTP  | Module-Stop control (clock-gating)                      | `ra8_mstp.c` |
| OFS   | Option-Function Select (boot configuration words)       | `ra8_ofs.c` |
| PWR   | Power-management glue                                   | `pwr.zig`   |
| RESET | Reset controller (RSTSR1/2 + cold/warm flags)           | `reset_abi.zig` |
| SYSC  | SYSTEM Controller (R_SYSTEM register block)             | (used by `pwr.zig`, `reset_abi.zig`, `vreg_abi.zig`, `lpm_abi.zig`) |
| VBATT | Battery-backup domain (VBATT pin / VBTBKR registers)    | `internal/bkup.zig` |
| VREG  | Internal voltage regulator                              | `vreg_abi.zig` |
| BKUP  | Battery-backup function (alias for VBATT block)         | `internal/bkup.zig` |

## 2. IO and pin-mux

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| GPIO  | General-Purpose Input / Output                            | `gpio_abi.zig`, `gpio_pins_abi.zig` |
| PORT  | Parallel I/O port (PORT0..PORT14 register banks)          | `gpio_abi.zig`, `gpio_pins_abi.zig` |
| PFS   | Pin Function Select (per-pin alternate-function register) | `gpio_abi.zig`, `gpio_pins_abi.zig` |
| PSEL  | Pin Select (PFS bitfield choosing the alternate function) | `gpio_abi.zig`, `gpio_pins_abi.zig` |
| PMR   | Port Mode Register (digital vs peripheral)                | `gpio_abi.zig`, `gpio_pins_abi.zig` |
| PDR   | Port Direction Register                                   | `gpio_abi.zig`, `gpio_pins_abi.zig` |
| PODR  | Port Output Data Register                                 | `gpio_abi.zig`, `gpio_pins_abi.zig` |
| PIDR  | Port Input Data Register                                  | `gpio_abi.zig`, `gpio_pins_abi.zig` |
| PWPR  | Pin Write-Protect Register (PFS unlock)                   | `gpio_abi.zig`, `gpio_pins_abi.zig` |
| PWPRS | Secure Pin Write-Protect Register                         | `gpio_abi.zig`, `gpio_pins_abi.zig` |
| PMISC | Pin Miscellaneous (contains PWPR/PWPRS)                   | `gpio_abi.zig`, `gpio_pins_abi.zig` |
| MPC   | Multi-function Pin Controller                             | `internal/mpc.zig` |
| ELC   | Event Link Controller (peripheral-to-peripheral events)   | `elc_abi.zig` |
| ICU   | Interrupt Controller Unit                                 | `internal/icu.zig` |
| ISR   | Interrupt Service Routine (HAL ISR-table glue)            | `isr_abi.zig` |
| IRQ   | Interrupt Request line (NVIC vector entry)                | `internal/icu.zig` |
| WUPEN | Wake-Up Enable register                                   | `lpm_abi.zig` |

## 3. Communication

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| SCI   | Serial Communications Interface (UART/I2C/SPI super-mode) | `ra8_sci.c` |
| UART  | Universal Asynchronous Receiver/Transmitter               | `ra8_sci.c` |
| SPI   | Serial Peripheral Interface (controller/peripheral)       | `ra8_spi_b.c` |
| IIC_B | I2C bus controller, version B (RIIC)                     | `ra8_i2c.c`, `i2c_target_abi.zig` |
| I3C   | Improved Inter-Integrated Circuit (MIPI I3C)              | `ra8_i3c.c` |
| SMBUS | System Management Bus (I2C-compatible)                    | `ra8_smbus.c` |
| CANFD | Controller Area Network with Flexible Data-rate           | `ra8_canfd.c` |
| CNECC | CAN Message-RAM ECC controller                            | `cnecc_abi.zig` |
| USB FS| USB Full-Speed (12 Mbps)                                  | `ra8_usb.c`, `ra8_usb_*.c` |
| USB HS| USB High-Speed (480 Mbps)                                 | `ra8_usb.c`, `ra8_usb_*.c` |
| CDC   | USB Communications Device Class (virtual COM)             | `ra8_usb_cdc.c`, `ra8_usb_hcdc.c`, `ra8_usb_hcdc_ecm.c` |
| HID   | USB Human Interface Device                                | `usb_phid_abi.zig`, `ra8_usb_hhid.c` |
| MSC   | USB Mass Storage Class                                    | `usb_pmsc_abi.zig`, `ra8_usb_hmsc.c` |
| HHUB  | USB Host Hub class driver                                 | `usb_hhub_abi.zig` |
| PVND  | USB Peripheral Vendor-class                               | `usb_pvnd_abi.zig` |
| PAUD/HAUD | USB Peripheral / Host Audio class                     | `internal/usb_paud.zig`, `ra8_usb_haud.c` |
| PPRN  | USB Peripheral Printer class                              | `usb_pprn.zig`   |
| ETHA  | Ethernet adapter (gigabit MAC top-level)                  | `ra8_etha.c`, `ra8_eth.c` |
| RMAC  | Reduced Media Access Controller (per-port MAC)            | `ra8_rmac.c`, `rmac_phy_drv_abi.zig` |
| GWCA  | GateWay CPU Agent (Ethernet DMA gateway)                  | `ra8_eth_gwca.c` |
| MFWD  | MAC ForWarDing engine                                     | `eth_mfwd_abi.zig`|
| ESWM  | Ethernet SWitch Management                                | `internal/layer3_switch.zig` |
| GPTP  | Generic Precision Time Protocol timer (HUM Ch 35; a timer, not a 1588 message engine) | `eth_gptp.zig`   |
| TSN   | Time-Sensitive Networking                                 | `internal/tsn.zig` |
| PHY   | Physical-layer transceiver (Ethernet PHY)                 | `internal/ether_phy.zig`, `internal/rmac_phy_drv.zig` |
| BLE   | Bluetooth Low Energy (HCI transport seam; controller on the ESP32-C6 companion) | `ble_abi.zig`, `port/nimble` |
| IPC   | Inter-Processor Communication (M85 <-> M33 mailbox)       | `ra8_ipc.c` |

## 4. Crypto and secure-storage

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| RSIP  | Renesas Secure IP (HW crypto + key vault, RSIP-E50D)     | `ra8_rsip.c`, `rsip_protected_abi.zig`, `ra8_rsip_key_injection.c` |
| DOTF  | Decryption-On-The-Fly (XIP-decrypt for xSPI)             | `ra8_dotf.c` |
| CRC   | Cyclic-Redundancy-Check engine                           | `internal/crc.zig` |
| DOC   | Data Operation Circuit (compare/add for tamper checks)   | `internal/doc.zig` |
| MMPU  | Bus-initiator Memory Protection Unit                     | (HAL init only) |
| CPSCU | Security Control Unit (per-peripheral S/NS attribution)   | `lvd_events.zig`, `internal/sram.zig` |
| BBFSAR| Battery-Backup Full Security Attribute Register          | `internal/bkup.zig` |

## 5. Display, video, graphics

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| GLCDC   | Graphics LCD Controller (parallel-RGB output)           | `ra8_glcdc.c` |
| DRW     | 2D DRaWing engine (DAVE-2D core)                        | `ra8_drw.c` |
| MIPI DSI| MIPI Display Serial Interface                           | `ra8_mipi_dsi.c` |
| MIPI CSI| MIPI Camera Serial Interface                            | `ra8_mipi_csi.c` |
| MIPI PHY| Shared MIPI D-PHY                                       | `ra8_mipi_phy.c` |
| CEU     | Capture Engine Unit (parallel-camera input)             | `ra8_ceu.c` |
| VIN     | Video INput module                                      | `ra8_vin.c` |
| JPEG_SW | Software JPEG codec (no JPEG HW IP on RA8D2)            | `libs/ra8_jpeg/src/ra8_jpeg_sw.c` |
| EPAPER  | E-Paper / EPD framebuffer driver                        | `ra8_epaper.c` |
| TCON    | Timing CONtroller (GLCDC TCON0..3 outputs)              | `ra8_glcdc.c` |

## 6. Audio

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| SSIE  | Serial Sound Interface Enhanced (I2S/TDM)                 | `ra8_ssie.c` |
| PDM   | Pulse-Density Modulation microphone interface             | `internal/pdm.zig` |
| DAI   | Digital Audio Interface (CODEC-side I2S signals)          | (used in board pin-mux) |

## 7. Analog

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| ADC_B | Analog-to-Digital Converter, version B                    | `adc.c` |
| DAC_B | Digital-to-Analog Converter, version B                    | `dac_b.zig`   |
| ACMPHS| High-Speed Analog Comparator                              | `internal/acmphs.zig` |

## 8. Timers, motor / power

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| GPT   | General PWM Timer (32-bit, motor / general-purpose)       | `ra8_gpt.c`, `timer.c` |
| GTIOC | GPT IO Channel pin (GTIOCnA/B output)                     | `ra8_gpt.c` |
| AGT   | Asynchronous General-purpose Timer (16-bit)               | `ra8_agt.c` |
| ULPT  | Ultra-Low-Power Timer                                     | `internal/ulpt.zig` |
| POEG  | Port Output Enable for GPT (motor-fault shut-off)         | `internal/poeg.zig` |
| PDG   | Phase Delay Generator (multi-channel motor sync)          | `ra8_pdg.c` |
| WDT   | Watchdog Timer                                            | `ra8_wdt.c` |
| IWDT  | Independent Watchdog Timer                                | `internal/iwdt.zig` |
| RTC   | Real-Time Clock                                           | `ra8_rtc.c` |

## 9. Memory / storage

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| MRAM  | Magnetoresistive RAM (1 MiB on-chip, code memory)         | `ra8_flash.c` |
| MRMS  | MRAM Module Sequencer (MRAM controller)                   | `ra8_flash.c` |
| SRAM  | Static RAM (1664 KiB on-chip, ECC-protected)              | `internal/sram.zig` |
| ECC   | Error-Correcting Code (SRAM/MRAM single-bit correction)   | `internal/sram.zig`, `cnecc_abi.zig` |
| DTCM  | Data Tightly-Coupled Memory                               | (linker only) |
| ITCM  | Instruction Tightly-Coupled Memory                        | (linker only) |
| TCM   | Tightly-Coupled Memory (umbrella for ITCM + DTCM)         | (linker only) |
| SDRAM | Synchronous Dynamic RAM (external, 64 MiB on EK)          | `internal/sdramc.zig` |
| SDRAMC| SDRAM Controller                                          | `internal/sdramc.zig` |
| OSPI  | Octo-SPI (Renesas register block name)                    | `ra8_xspi.c` |
| XSPI  | eXpanded SPI (xSPI = HUM term for the OSPI controller)    | `ra8_xspi.c` |
| XIP   | eXecute-In-Place (memory-mapped read of external flash)   | `ra8_xspi.c` |
| FLASH | Generic flash controller surface                          | `ra8_flash.c` |
| SDHI  | SD Host Interface                                         | `ra8_sdhi.c`, `sdcard_abi.zig` |
| DMA   | Direct Memory Access (top-level umbrella)                 | `dma_abi.zig` |
| DMAC  | Direct Memory Access Controller                           | `dmac.zig` |
| DTC   | Data Transfer Controller (lighter-weight than DMAC)       | `dtc.zig`   |
| DOTF  | Decryption-On-The-Fly (covered under crypto above)        | `ra8_dotf.c` |

## 10. Debug, test, NVIC / core

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| BSCAN | Boundary Scan controller                                  | `bscan_abi.zig`|
| HW ERR| Hardware-Error reporter                                   | `ra8_hw_err.h` |
| MMIO  | Memory-Mapped I/O (generic term, not a Renesas IP)        | -- |
| NVIC  | Nested Vectored Interrupt Controller (Cortex-M core)      | (used by `internal/icu.zig`) |
| SCB   | System Control Block (Cortex-M core)                      | (used by HAL fault handlers) |
| MPU   | Memory Protection Unit (core MPU at `0xE000ED90`)         | (HAL init) |
| MMPU  | Bus-initiator MPU (chip-level, distinct from core MPU)    | (HAL init) |
| FPU   | Floating-Point Unit (Cortex-M85 single+double precision)  | (toolchain flags) |
| MVE   | M-profile Vector Extension (a.k.a. Helium)                | (toolchain flags) |
| Helium| ARM marketing name for MVE                                | (toolchain flags) |
| SAU   | Security Attribution Unit (TrustZone partitioning)        | per-app `trustzone_init.c` |
| NSC   | Non-Secure Callable (TrustZone veneers)                   | `libs/ra8_nsc/` |

## 11. Touch and sensors

| Acronym | Expansion | HAL driver |
|---------|-----------|------------|
| TOUCH | Capacitive-touch driver (GT911 panel, parallel TFT)       | `touch_abi.zig` |

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
