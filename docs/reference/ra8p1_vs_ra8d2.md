# RA8P1 vs RA8D2 -- durable difference reference

Technical reference for the RA8 multi-chip build (both `R7KA8D2KFLCAC` and
`R7KA8P1KFLCAC` from one tree). This file is the durable memory-map /
register-base map; the **plan, rationale, and follow-ups live in the GitHub
epic** (search issues for "RA8P1 support: RA8D2-vs-RA8P1 difference analysis").
The device-selection code is `libs/ra8_core/inc/ra8_device.h`.

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
| DOC alias | `0x40311000` | alias of the shared DOC_B (cosmetic) |

There is **no** legacy ETHERC/EDMAC MAC at `0x40354000` on the RA8P1 -- an earlier
draft of this table listed one, but it does not exist (see "Correction" below).

## Complete delta set (RA8P1 vs RA8D2)

1. **+ Ethos-U55 NPU** (256 8x8 MACs, up to 500 MHz, ~256 GOPS, 8/16-bit CNN+RNN)
2. **- OFS3 / WDT1 option register**
3. **ADC 16-bit** (ADC16H x2, datasheet) vs 12-bit (FSP comment) -- base unchanged
4. **M85 double-precision-capable FPU** (datasheet) vs FSP CMSIS `__FPU_DP=0`
   -- we build `fpv5-sp-d16` (correctness-safe on both); DP is a perf follow-up
5. + DOC alias; + `IOPORT_PERIPHERAL_ESC` pin function; ADC-sensor sampling-time flag

## Correction: no legacy ETHERC/EDMAC MAC on the RA8P1 (issue #224)

An earlier revision of this reference (and roadmap issues #220 / #224) claimed the
RA8P1 adds a classic single-port ETHERC/EDMAC Ethernet MAC at `0x40354000`, in
addition to the shared R-Switch/ESWM fabric. **That was a misread; the RA8P1 has
no such peripheral.** Verified by full-text search of both primary manuals:

- **RA8P1 HUM R01UH1064EJ0130** and **RA8P1 datasheet R01DS0439EJ0130**: zero
  occurrences of "ETHERC" and zero of the classic ETHERC/EDMAC registers
  (`ECMR` / `EDMR` / ...); nothing is based at `0x40354000` (that window holds
  USBHS `0x40351000`, SCI `0x40358000 + 0x100*n`, SPI `0x4035C000 + 0x100*n`).
- The token **"EDMAC"** appears only in the Buses chapter, where **both** the
  RA8P1 HUM *and* the RA8D2 HUM (R01UH1065EJ) state verbatim: *"EDMAC in this
  chapter means the GWCA function of ESWM."* It is the descriptor-DMA bus-master
  alias of the shared R-Switch (Ethernet CPU Agent), present identically on
  **both** parts -- not an RA8P1-only MAC.
- The RA8P1's only Ethernet is the same R-Switch/ESWM subsystem as the RA8D2:
  identical HUM chapters **30-36** (ESWM / MFWD / COMA / ETHA / RMAC / GWCA /
  GPTP), page-shifted only by the inserted NPU chapter, with identical register
  bases (ETHA0 `0x403CA000`, etc.).

Consequently there is no `RA8_HAS_ETHERC_EDMAC` flag in
`libs/ra8_core/inc/ra8_device.h`, no `ra8_etherc` / `ra8_edmac` driver, and no
ra8_emulator ETHERC model to add. Because the "MAC" that motivated the "#21 large-
frame TX defect is a different IP" angle does not exist, that angle is moot: the
RA8P1's clean-vs-defect Ethernet story is identical to the RA8D2's R-Switch.

Identical on both: TrustZone-M v2 (NS-alias = address bit 28), `R_CPSCU`
`0x40008000`, `R_PSCU` `0x40204000`, **RSIP-E50D**, DLM 8-state, boot modes
(single-chip / JTAG / SCI / USB), secure boot (immutable FSBL in OTP), 14 port
groups (P0-P9, PA-PD), and the full peripheral set (GLCDC, MIPI DSI/CSI, CEU,
DRW 2D, CANFD x2, USB FS+HS, SDHI x2, OSPI x2, SCI x10, I3C, GPT x14, AGT, SSIE,
PDM, CAC, DMAC x8, DTC, ELC, IPC dual-core).

## Sources

RA8P1 HUM R01UH1064EJ0130 and datasheet R01DS0439EJ0130 (both read directly and
full-text searched -- see "Correction" above); FSP `github.com/renesas/fsp`
(`R7KA8{P1,D2}KF_core0.h`, `bsp/mcu/ra8{p1,d2}/{bsp_elc,bsp_feature,bsp_peripheral}.h`,
`bsp/mcu/all/bsp_module_stop.h`, `ra/board/ra8p1_ek/board.h`); Zephyr
`dts/arm/renesas/ra/ra8/r7ka8{p1,d2}kflcac*.dtsi`; Renesas part page
`r7ka8p1kflcac-uc0`.
