# RA8P1 vs RA8D2 -- durable difference reference

Technical reference for the RA8 multi-chip build (both `R7KA8D2KFLCAC` and
`R7KA8P1KFLCAC` from one tree). This file is the durable memory-map /
register-base map; the **plan, rationale, and follow-ups live in the GitHub
epic** (search issues for "RA8P1 support: RA8D2-vs-RA8P1 difference analysis").
The device-selection code is `libs/ra_core/inc/ra_device.h`.

## Document numbers

| | Datasheet | Hardware User's Manual |
|---|---|---|
| RA8D2 | R01DS0493EJ | **R01UH1065EJ** (in `docs/reference/`) |
| RA8P1 | R01DS0439EJ0130 | **R01UH1064EJ0130** |

## One-line summary

The RA8P1 is **"RA8D2 + an Arm Ethos-U55 NPU"**. Same Cortex-M85 @ 1 GHz +
Cortex-M33 @ 250 MHz, same pin-compatible 289-pin BGA. The peripheral register
bases (155/155), the memory map, the ICU/ELC event numbers, and the MSTP
module-stop bits are **byte-identical**. The register headers therefore need no
device-conditional edits; only new peripherals get new headers.

## Memory map (identical on both parts unless noted)

| Region | Base | Size | Notes |
|---|---|---|---|
| Code MRAM | `0x02000000` | 1 MB | CM85 768 KB @`0x02000000` + CM33 256 KB @`0x020C0000` |
| System SRAM | `0x22000000` | 1664 KB | SRAM0 1024 KB + SRAM1 640 KB @`0x22100000`, ECC; shared with NPU (AXI) |
| ITCM (M85) | `0x00000000` | 64 KB* | *linker floor; RA8P1 M85 TCM budget is 256 KB total (split unconfirmed) |
| DTCM (M85) | `0x20000000` | 64 KB* | (+128 KB M33 TCM: 1664+256+128 = 2048 KB "total RAM") |
| SDRAM (ext) | `0x68000000` | 64 MB (EK) | 32-bit external bus |
| OSPI/xSPI XIP | `0x80000000` (CS0), `0x90000000` (CS1) | ext | HyperRAM/HyperBus capable |
| Option-setting | `0x0300A000` region | | **RA8P1 has no OFS3/WDT1 option register** |
| **NPU regs** | **`0x40140000`** | 4 KB | **RA8P1 only** (Ethos-U55) |

## Register-base additions on RA8P1 (all shared bases are identical)

| Peripheral | Base | Event / MSTP |
|---|---|---|
| Ethos-U55 NPU | `0x40140000` | `ELC_EVENT_NPU_IRQ = 0x067`; MSTPCRA bit 16; NPUCLK = SCKDIVCR2[11:8] |
| Legacy ETHERC/EDMAC | `0x40354000` | MSTP bit not in shared FSP file -- confirm vs HUM |
| DOC alias | `0x40311000` | alias of the shared DOC_B (cosmetic) |

## Complete delta set (RA8P1 vs RA8D2)

1. **+ Ethos-U55 NPU** (256 8x8 MACs, up to 500 MHz, ~256 GOPS, 8/16-bit CNN+RNN)
2. **+ legacy ETHERC/EDMAC MAC** (RA8P1 has this *and* the shared switch-fabric ETHA)
3. **- OFS3 / WDT1 option register**
4. **ADC 16-bit** (ADC16H x2, datasheet) vs 12-bit (FSP comment) -- base unchanged
5. **M85 double-precision-capable FPU** (datasheet) vs FSP CMSIS `__FPU_DP=0`
   -- we build `fpv5-sp-d16` (correctness-safe on both); DP is a perf follow-up
6. + DOC alias; + `IOPORT_PERIPHERAL_ESC` pin function; ADC-sensor sampling-time flag

Identical on both: TrustZone-M v2 (NS-alias = address bit 28), `R_CPSCU`
`0x40008000`, `R_PSCU` `0x40204000`, **RSIP-E50D**, DLM 8-state, boot modes
(single-chip / JTAG / SCI / USB), secure boot (immutable FSBL in OTP), 14 port
groups (P0-P9, PA-PD), and the full peripheral set (GLCDC, MIPI DSI/CSI, CEU,
DRW 2D, CANFD x2, USB FS+HS, SDHI x2, OSPI x2, SCI x10, I3C, GPT x14, AGT, SSIE,
PDM, CAC, DMAC x8, DTC, ELC, IPC dual-core).

## Sources

RA8P1 datasheet R01DS0439EJ0130 (read directly); FSP `github.com/renesas/fsp`
(`R7KA8{P1,D2}KF_core0.h`, `bsp/mcu/ra8{p1,d2}/{bsp_elc,bsp_feature,bsp_peripheral}.h`,
`bsp/mcu/all/bsp_module_stop.h`, `ra/board/ra8p1_ek/board.h`); Zephyr
`dts/arm/renesas/ra/ra8/r7ka8{p1,d2}kflcac*.dtsi`; Renesas part page
`r7ka8p1kflcac-uc0`.
