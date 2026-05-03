# RA8D2 Memory Map -- Quick Reference

Authoritative source: Renesas RA8D2 Group Hardware User's Manual
(R01UH1065EJ), Chapter 5 "Address Space" (pages 236-243). Board-side
population is from the EK-RA8D2 v1 User's Manual (R20UT5523EG0101 Rev
1.01, Oct 2025) committed at
`docs/reference/ek-ra8d2-v1-users-manual.pdf`.

This document only lists addresses that are actually referenced by code
in this tree (linker scripts under `examples/<app>/linker_script.ld`
and the typed `_base_addr` enums under `libs/ra_hal/inc/ra8d2_*_regs.h`).
Anything not grounded in the codebase is intentionally omitted.

## 1. System memory regions

| Region        | Base         | Length on RA8D2 | Notes                                        | Code reference                          |
|---------------|--------------|-----------------|----------------------------------------------|------------------------------------------|
| ITCM          | `0x00000000` | 64 KiB          | Cortex-M85 instruction tightly-coupled mem   | `examples/blink/linker_script.ld:11,30` |
| MRAM (code)   | `0x02000000` | 1 MiB           | Non-volatile code + rodata + vectors + OFS   | `examples/blink/linker_script.ld:9,28`  |
| Factory cal   | `0x02C1EDA0` | (TSN cal block) | TSN factory-calibration data window          | `libs/ra_hal/inc/ra8d2_tsn_regs.h`      |
| DTCM          | `0x20000000` | 64 KiB          | Cortex-M85 data tightly-coupled mem          | `examples/blink/linker_script.ld:12,31` |
| SRAM (ECC)    | `0x22000000` | 2 MiB           | Main SRAM, ECC-protected, secure alias       | `examples/blink/linker_script.ld:13,32` |
| SRAM NS alias | `0x22100000` | 1 MiB           | Non-secure single-image alias of SRAM        | `examples/blink/linker_script.ld:42`    |
| MRAM NS alias | `0x02080000` | 512 KiB         | Non-secure single-image alias of MRAM        | `examples/blink/linker_script.ld:41`    |
| External SDRAM| `0x68000000` | 64 MiB on EK    | Driven by SDRAMC, EK-RA8D2 v1 populates 64MB | `examples/blink/linker_script.ld:17,33` |
| System ctrl   | `0xE000ED90` | (MPU/SCS)       | Cortex-M85 MPU control (core MPU)            | `libs/ra_hal/inc/ra8d2_mpu_regs.h`      |

Notes on the core memory layout:

- The MRAM secure alias is at `0x02000000` and the non-secure alias is
  at `0x02080000` (offset `+512K`). Linker scripts in
  `examples/<app>/linker_script.ld` define both `MRAM` and `NS_MRAM`
  regions for the single-image TrustZone build.
- SRAM is mapped at the secure alias `0x22000000` (also exposed via the
  data alias enum `k_ra_sram_data_base_addr = 0x22000000` in
  `libs/ra_hal/inc/ra8d2_sram_regs.h`). The non-secure alias is at
  `0x22100000`.
- External SDRAM is at `0x68000000` (NOT `0x90000000`). On EK-RA8D2 v1
  the SDRAM controller drives a populated 64 MiB SDRAM at this address;
  see `libs/ra_hal/src/ra_sdramc.c`
  (`"sdramc_init (64 MiB @ 0x68000000)"`).
- The xSPI / Octo-SPI memory-mapped (XIP) read window address is NOT
  defined as a typed enum anywhere in this tree. Only the xSPI register
  windows (`0x40268000` / `0x40268400`) are pinned in code (see
  `libs/ra_hal/inc/ra8d2_ospi_regs.h`). Treat the XIP base as
  "look up in HUM Ch 5 once we add an XIP example" rather than
  hard-coding a guess here.

## 2. Peripheral base addresses (cited from `libs/ra_hal/inc/`)

Every value below is a verbatim copy of a typed enum
`k_ra_<peripheral>_base_addr` declared in
`libs/ra_hal/inc/ra8d2_<peripheral>_regs.h`. The "HAL driver" column is
the corresponding driver source under `libs/ra_hal/src/`.

### 2.1 Bus / system / DMA

| Peripheral      | Secure base   | Notes                                                | HAL driver                              |
|-----------------|---------------|------------------------------------------------------|------------------------------------------|
| MMPU            | `0x40000000`  | Bus-initiator MPU                                       | (no driver, used by HAL init)           |
| SPMON           | `0x40000D00`  | Bus / stack monitor                                  | (no driver yet)                          |
| SRAM control    | `0x40002000`  | SRAM register window                                  | `ra_sram.c`                              |
| SDRAMC          | `0x40003C00`  | Bus.SDRAM sub-block                                   | `ra_sdramc.c`                            |
| ICU             | `0x40006000`  | Interrupt Controller Unit                            | `ra_icu.c`                               |
| CPSCU           | `0x40008000`  | Secure security control (LVD/SRAM CPSCU window)      | `ra_lvd.c`, `ra_sram.c`                  |
| LPM SYSC alias  | `0x4001E000`  | SYSC base (also used by LVD, BKUP, RESET, VREG)      | `ra_lpm.c`, `ra_pwr.c`                   |
| BKUP / VBATT    | `0x4001E000`  | Battery backup, shares SYSC window                   | `ra_bkup.c`                              |
| SYSTEM (SYSC)   | `0x4001E000`  | R_SYSTEM register block                              | `ra_pwr.c`, `ra_reset.c`                 |
| VREG            | `0x4001E000`  | Voltage regulator (within SYSC)                      | `ra_vreg.c`                              |
| IPC             | `0x40020000`  | Inter-processor communication (M85 <-> M33)          | `ra_ipc.c`                               |
| LPM ICU/WUPEN   | `0x4000C000`  | Wake-up enable                                       | `ra_lpm.c`                               |
| DMAC0           | `0x4000A000`  | Direct Memory Access Controller, ch 0                | `ra_dmac.c`                              |
| DMA shared      | `0x4000A800`  | Shared DMA module regs                               | `ra_dma.c`                               |
| DTC0            | `0x4000AC00`  | Data Transfer Controller                             | `ra_dtc.c`                               |
| RTC             | `0x40202000`  | Real-Time Clock                                      | `ra_rtc.c`                               |
| IWDT            | `0x40202200`  | Independent Watchdog                                 | `ra_iwdt.c`                              |
| CAC             | `0x40202400`  | Clock Frequency Accuracy Measurement Circuit         | `ra_cac.c`                               |
| WDT0            | `0x40202600`  | Watchdog (M85 side)                                  | `ra_wdt.c`                               |
| WDT1            | `0x40202700`  | Watchdog (M33 side)                                  | `ra_wdt.c`                               |
| MSTP            | `0x40203000`  | Module-stop (clock-gate) registers                   | `ra_mstp.c`                              |
| ELC             | `0x40201000`  | Event Link Controller                                | `ra_elc.c`                               |
| MRMS / MRAM     | `0x4013C000`  | MRAM control / R_MRMS                                | `ra_flash.c`                             |
| RESET (SYSC)    | `0x4001E000`  | Reset control via SYSC                               | `ra_reset.c`                             |

### 2.2 General-purpose IO and timers

| Peripheral | Secure base | Notes                          | HAL driver        |
|------------|-------------|--------------------------------|-------------------|
| PORT0      | `0x40400000`| 0x20-byte stride per PORTn     | `gpio.c`          |
| PORT1      | `0x40400020`|                                | `gpio.c`          |
| PORT2      | `0x40400040`|                                | `gpio.c`          |
| PORT3      | `0x40400060`|                                | `gpio.c`          |
| PORT4      | `0x40400080`|                                | `gpio.c`          |
| PORT5      | `0x404000A0`|                                | `gpio.c`          |
| PORT6      | `0x404000C0`|                                | `gpio.c`          |
| PORT7      | `0x404000E0`|                                | `gpio.c`          |
| PORT8      | `0x40400100`|                                | `gpio.c`          |
| PORT9      | `0x40400120`|                                | `gpio.c`          |
| PORT10     | `0x40400140`|                                | `gpio.c`          |
| PORT11     | `0x40400160`|                                | `gpio.c`          |
| PORT12     | `0x40400180`|                                | `gpio.c`          |
| PORT13     | `0x404001A0`|                                | `gpio.c`          |
| PORT14     | `0x404001C0`|                                | `gpio.c`          |
| PFS        | `0x40400800`| Pin Function Select array      | `gpio.c`          |
| PMISC      | `0x40400D00`| PWPR / PWPRS write-protect     | `gpio.c`          |
| GPT0       | `0x40322000`| GPT channel 0 (rest by stride) | `ra_gpt.c`, `timer.c` |
| GPT OPS    | `0x40323F00`| Output Phase Switching         | `ra_gpt.c`        |
| GPT ODC    | `0x40324000`| Output Disable Control         | `ra_gpt.c`        |
| AGT0       | `0x40221000`| Async General-Purpose Timer    | `ra_agt.c`        |
| ULPT0      | `0x40220000`| Ultra-Low-Power Timer          | `ra_ulpt.c`       |
| ULPT1      | `0x40220100`|                                | `ra_ulpt.c`       |
| POEG0..3   | `0x40212000` + `n*0x100` | Port Output Enable for GPT | `ra_poeg.c` |
| PDG        | `0x40324000`| GPT Phase Delay Generator (S)  | `ra_pdg.c`        |
| PDG NS     | `0x50324000`| Non-secure alias               | `ra_pdg.c`        |

### 2.3 Communication peripherals

| Peripheral | Secure base | Notes                        | HAL driver        |
|------------|-------------|------------------------------|-------------------|
| SCI0..SCI9 | `0x40358000` + `n*0x100` | UART/I2C/SPI super-mode | `ra_sci.c`, `uart.c` |
| SPI0       | `0x4035C000`| HUM Ch 43, SPI0              | `ra_spi_b.c`      |
| SPI1       | `0x4035C100`|                              | `ra_spi_b.c`      |
| IIC_B0     | `0x4035F000`| HUM Ch 40.2 p 2452           | `ra_iic_b.c`      |
| I3C0       | `0x4035F000`| Shares window with IIC_B0    | `ra_i3c.c`        |
| I3C1       | `0x4035F100`|                              | `ra_i3c.c`        |
| CANFD0     | `0x40380000`| HUM Ch 41 p 2702             | `ra_canfd.c`      |
| CANFD1     | `0x40382000`|                              | `ra_canfd.c`      |
| CNECC0     | `0x4036F200`| ECCMB0 (CAN0 MRAM ECC)       | `ra_cnecc.c`      |
| CNECC1     | `0x4036F300`| ECCMB1 (CAN1 MRAM ECC)       | `ra_cnecc.c`      |
| USB FS     | `0x40250000`| Full-Speed                   | `ra_usb.c`, `ra_usb_*.c` |
| USB HS     | `0x40351000`| High-Speed                   | `ra_usb.c`, `ra_usb_*.c` |
| ETHA0      | `0x403CA000`| Ethernet adapter ch 0        | `ra_etha.c`, `ra_eth.c` |
| ETHA1      | `0x403CC000`|                              | `ra_etha.c`       |
| RMAC0      | `0x403CB000`| Reduced MAC ch 0             | `ra_rmac.c`       |
| RMAC1      | `0x403CD000`|                              | `ra_rmac.c`       |
| GWCA0      | `0x403CE000`| Gateway CPU agent            | `ra_eth_gwca.c`   |
| MFWD       | `0x403C0000`| MAC forwarding               | `ra_eth_mfwd.c`   |
| ESWM       | `0x403C8000`| Ethernet switch mgmt         | `ra_layer3_switch.c` |
| GPTP       | `0x403E0000`| Generic PTP                  | `ra_eth_gptp.c`   |
| PTP        | `0x403E0100`| PTP SYNFP/STCA window        | `ra_ptp.c`        |
| TSN ctrl   | `0x40235000`| TSN control block            | `ra_tsn.c`        |
| BLE        | `0x40700000`| Placeholder; vendor RPC loads patch RAM | `ra_ble.c`, `ra_ble_patch.c` |

### 2.4 Display, video, audio

| Peripheral | Secure base | Notes                 | HAL driver        |
|------------|-------------|-----------------------|-------------------|
| GLCDC      | `0x40342000`| Graphics LCD ctrl     | `ra_glcdc.c`      |
| DRW (S)    | `0x40444000`| 2D drawing engine     | `ra_drw.c`        |
| DRW (NS)   | `0x50444000`| Non-secure alias      | `ra_drw.c`        |
| MIPI DSI   | `0x40346000`| Display serial intf   | `ra_mipi_dsi.c`   |
| MIPI PHY   | `0x40346C00`| Shared D-PHY          | `ra_mipi_phy.c`   |
| MIPI CSI   | `0x40347000`| Camera serial intf    | `ra_mipi_csi.c`   |
| VIN0 (S)   | `0x40347400`| Video-in (HUM 67.2)   | `ra_vin.c`        |
| VIN0 (NS)  | `0x50347400`| Non-secure alias      | `ra_vin.c`        |
| CEU        | `0x40348000`| Capture Engine Unit (parallel camera) | `ra_ceu.c` |
| SSIE0      | `0x4025D000`| Serial Sound full-dup | `ra_ssie.c`       |
| SSIE1      | `0x4025D100`| Serial Sound half-dup | `ra_ssie.c`       |
| PDM        | `0x40256000`| Pulse-density mic     | `ra_pdm.c`        |
| ADC_B      | `0x40338000`| FSP R_ADC_B0_BASE     | `adc.c`           |
| DAC_B0     | `0x40233000`| FSP R_DAC_B0_BASE     | `ra_dac_b.c`      |
| DAC_B1     | `0x40233100`| FSP R_DAC_B1_BASE     | `ra_dac_b.c`      |
| ACMPHS0    | `0x40236000`| Hi-speed comparator   | `ra_acmphs.c`     |

### 2.5 Crypto and external flash

| Peripheral   | Secure base   | Notes                                     | HAL driver               |
|--------------|---------------|-------------------------------------------|--------------------------|
| RSIP-E50D    | `0x403B0000`  | Renesas Secure IP mailbox                 | `ra_rsip.c`, `ra_rsip_protected.c`, `ra_rsip_key_injection.c` |
| DOTF0 (S)    | `0x40268800`  | Decryption-on-the-fly ch 0 secure         | `ra_dotf.c`              |
| DOTF0 (NS)   | `0x50268800`  | DOTF ch 0 non-secure alias                | `ra_dotf.c`              |
| DOTF1 (S)    | `0x40268900`  | DOTF ch 1 secure                          | `ra_dotf.c`              |
| DOTF1 (NS)   | `0x50268900`  | DOTF ch 1 non-secure alias                | `ra_dotf.c`              |
| XSPI0 regs   | `0x40268000`  | xSPI / Octo-SPI controller 0 reg window   | `ra_xspi.c`              |
| XSPI1 regs   | `0x40268400`  | xSPI / Octo-SPI controller 1 reg window   | `ra_xspi.c`              |
| SDHI0        | `0x40252000`  | SD host interface ch 0                    | `ra_sdhi.c`, `ra_sdcard.c` |
| SDHI1        | `0x40252400`  | SD host interface ch 1                    | `ra_sdhi.c`              |
| FLASH ctrl   | `0x4013C000`  | MRMS / MRAM control                       | `ra_flash.c`             |

### 2.6 Misc

| Peripheral | Secure base | Notes                | HAL driver |
|------------|-------------|----------------------|------------|
| CRC        | `0x40310000`| HUM Ch 48 p 3180     | `ra_crc.c` |
| DOC        | `0x40311000`| Data Operation Ckt   | `ra_doc.c` |
| Core MPU   | `0xE000ED90`| Cortex-M85 MPU regs  | (HAL init) |

## 3. EK-RA8D2 v1 board population (what's actually wired)

Source: `docs/reference/ek-ra8d2-v1-users-manual.pdf` and the audit
notes in `docs/reference/EK-RA8D2-board-manual-PLACEHOLDER.md`.

| Region / IP      | Address                         | EK-RA8D2 v1 status                                                    |
|------------------|---------------------------------|------------------------------------------------------------------------|
| MRAM (1 MiB)     | `0x02000000`                    | On-chip, always present                                                |
| SRAM ECC (2 MiB) | `0x22000000`                    | On-chip, always present                                                |
| ITCM / DTCM      | `0x00000000` / `0x20000000`     | On-chip                                                                |
| External SDRAM   | `0x68000000`                    | Populated, 64 MiB, driven by SDRAMC (`ra_sdramc.c`)                    |
| External xSPI    | xSPI0 regs at `0x40268000`      | xSPI controller present; XIP memory window not pinned in this tree    |
| GLCDC parallel   | `0x40342000` -> connector J1    | Parallel Graphics Expansion Port; 7.0" 1024x600 TFT via add-on board   |
| MIPI DSI / CSI   | `0x40346000` / `0x40347000`     | Pads brought out via MIPI Graphics Expansion Board (separate add-on)   |
| OV5640 camera    | CEU `0x40348000` or VIN `0x40347400` | Wired to parallel camera connector                                |
| SDHI             | `0x40252000`                    | NOT POPULATED on EK-RA8D2 v1 -- no microSD socket on board             |
| USB FS / HS      | `0x40250000` / `0x40351000`     | Both USB connectors populated                                          |
| Ethernet         | ETHA + RMAC + GWCA              | RJ-45 populated                                                        |
| BLE              | `0x40700000` (placeholder)      | On-board BLE transceiver, vendor patch loaded via RPC                  |
| Audio CODEC      | SSIE0 `0x4025D000`              | DA7212 (U14), wired per board UM Table 32                              |
| User switches    | PORT regs + ICU                 | SW1 -> P009 / IRQ13-DS, SW2 -> P008 / IRQ12-DS                         |

## 4. References

- HUM Ch 5 "Address Space" pp 236-243 -- canonical address map
- HUM Ch 11 "Low Power Modes" -- SYSC base usage
- HUM Ch 12 "Battery Backup" -- VBATT register layout
- HUM Ch 40-44 -- IIC_B / CANFD / SPI / OSPI base addresses
- HUM Ch 60 "CEU" / Ch 67 "VIN" -- camera bases
- HUM Ch 63 "GLCDC", Ch 64-66 "MIPI"
- EK-RA8D2 v1 User's Manual R20UT5523EG0101 -- board population
