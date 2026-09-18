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

## Device selection

The build selects exactly one device through `libs/ra8_core/inc/ra8_device.h`:

| Define | Set by | Meaning |
|--------|--------|---------|
| `RA8_DEVICE_RA8D2` | Default when no device flag is supplied | RA8D2 and the current default build. |
| `RA8_DEVICE_RA8P1` | `cmake/toolchain-ra8p1.cmake` | RA8P1, including the NPU and double-precision FPU configuration. |

Feature code uses the derived `RA8_HAS_NPU` and `RA8_HAS_NPUCLK` capability
macros instead of testing the device name directly. `ra8_emulator` mirrors the
selection with `--device ra8p1`, which maps the RA8P1-only NPU window; the
default RA8D2 profile leaves that window unmapped.

## Memory map (identical on both parts unless noted)

| Region | Base | Size | Notes |
|---|---|---|---|
| Code MRAM | `0x02000000` | 1 MB | CM85 768 KB @`0x02000000` + CM33 256 KB @`0x020C0000` |
| System SRAM (user) | `0x22000000` | 1664 KB | SRAM0 1024 KB + SRAM1 640 KB @`0x22100000`, ECC; shared with NPU (AXI). This is USER SRAM, exclusive of TCM |
| ITCM (M85) | `0x00000000` | 128 KB, 64 KB declared* | *capacity is 128 KB on BOTH parts; 64 KB is the supported floor (see geometry section) |
| DTCM (M85) | `0x20000000` | 128 KB, 64 KB declared* | M33 adds 64 KB CTCM + 64 KB STCM; 1664+256+128 = 2048 KB, TCM carved OUT of the 2 MB island, not added to it |
| SDRAM (ext) | `0x68000000` | 64 MB (EK) | 32-bit external bus |
| OSPI/xSPI XIP | `0x80000000` (CS0), `0x90000000` (CS1) | ext | HyperRAM/HyperBus capable |
| Option-setting | `0x02C9F040`.. (+ `0x12C9F4C0` NS aliases, BPS `0x02C9F200`, OTP `0x02E07400`) | | identical on both parts, OFS0..OFS3 (see correction below) |
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
2. **ADC 16-bit** (ADC16H x2, datasheet) vs 12-bit (FSP comment) -- base unchanged
3. **M85 double-precision-capable FPU** (datasheet) vs FSP CMSIS `__FPU_DP=0`
   -- we build `fpv5-sp-d16` (correctness-safe on both); DP is a perf follow-up
4. + DOC alias; + `IOPORT_PERIPHERAL_ESC` pin function; ADC-sensor sampling-time flag

An earlier revision listed "- OFS3 / WDT1 option register" as delta 2. It is not
a delta -- see the correction below.

The host and emulator paths cover the device switch, OFS handling, FPU probe,
NPU driver and Ethos-U adapter. On-silicon NPU clock, interrupt and
Vela-compiled-model validation remain tracked by issue #229 because they
require an RA8P1 evaluation kit.

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

## Correction: the RA8P1 DOES have OFS3 (issue #516)

An earlier revision of this reference claimed `**RA8P1 has no OFS3/WDT1 option
register**`, and issue #223 acted on it -- deleting the OFS3 / OFS3_SEC /
OFS3_SEL family from the four RA8P1 app linker scripts and gating it out of
`ra8_ofs.{h,c}` behind `RA8_HAS_OFS3`. **That was wrong.** Verified by direct
extraction from both primary manuals:

- **RA8P1 HUM R01UH1064EJ0130 Rev.1.30 section 7.2.6** (printed p 288),
  `OFS3, OFS3_SEC : Option Function Select Register 3`, `Address: OFS3:
  0x12C9_F4C4 / OFS3_SEC: 02C9_F0C4`; and **7.2.7** (p 290) `OFS3_SEL :
  Option Function Select Register 3 for Security`, `02C9 F124h`. The RA8D2
  equivalents are 7.2.6 p 287 and 7.2.7 p 289 -- same addresses, same bit
  fields (`WDT1STRT`, `WDT1TOPS`, `WDT1CKS`, `WDT1RPES`, `WDT1RPSS`,
  `WDT1RSTIRQS`, `WDT1STPCTL`). Chapter 7's section list is identical
  (7.2.1-7.2.25) on both parts; `grep -c OFS3` is 59 in each manual.
- The RA8P1 has the **WDT1** that OFS3 configures: datasheet R01DS0439EJ0130
  lists `Watchdog Timer (WDT) x 2` with `WDT1` at `0x4020_2600`.
- The whole option-setting address column is character-for-character equal
  between the two HUMs.

The claim originated in FSP's `ra/fsp/src/bsp/mcu/ra8p1/bsp_feature.h`
(`BSP_FEATURE_BSP_HAS_OFS3 (0UL)`, vs `(1UL)` for ra8d2). That flag contradicts
Renesas' own manual, has **zero consumers** in open FSP source (it is metadata
for the closed RASC configurator), and FSP's own `bsp_linker.c` is byte-identical
between the two parts. A RASC-generated RA8P1 project emits the OFS3 sections at
exactly the HUM addresses. Treat the flag as an FSP data error, not evidence.

The old "option-setting region `0x0300A000`" address in the memory-map table was
also wrong -- that string appears **zero** times in either HUM.

Consequently there is no `RA8_HAS_OFS3` flag, no `k_ra8_feat_ofs3`, and no
`ra8_ofs_has_ofs3()`. `scripts/checks/check_linker_scripts.py` rule **LD008**
enforces that the option-setting family is all-or-nothing per script, so this
cannot regress a third time.

## Pin compatibility is now established, not asserted

The "same pin-compatible 289-pin BGA" line above was an assertion until
`scripts/gen/gen_pinouts.py` began parsing section 1.7 "Pin Lists" out of both
datasheets and diffing them. It is now a measurement: across all three packages
and both MIPI variants, every ball carries an identical function set on the two
groups, and the `pinout-freshness` gate re-establishes that on every run. See
[`docs/pinouts/README.md`](../pinouts/README.md).

Identical on both: TrustZone-M v2 (NS-alias = address bit 28), `R_CPSCU`
`0x40008000`, `R_PSCU` `0x40204000`, **RSIP-E50D**, DLM 8-state, boot modes
(single-chip / JTAG / SCI / USB), secure boot (immutable FSBL in OTP), 14 port
groups (P0-P9, PA-PD), and the full peripheral set (GLCDC, MIPI DSI/CSI, CEU,
DRW 2D, CANFD x2, USB FS+HS, SDHI x2, OSPI x2, SCI x10, I3C, GPT x14, AGT, SSIE,
PDM, CAC, DMAC x8, DTC, ELC, IPC dual-core).

## Cache / TCM geometry is identical on both parts (issue #850)

Two earlier claims in this file were wrong and are corrected here. The memory-map
table said the RA8P1 M85 TCM was "256 KB total (split unconfirmed)", implying a
larger-and-unknown RA8P1 budget; and the M33 was treated as cacheless. Neither
holds. The cache and TCM geometry is **byte-identical between the RA8D2 and the
RA8P1**, so it is not part of the delta set at all, and the M33 has two caches.

### Exact-SKU geometry: `R7KA8P1KFLCAC` (dual-core, 289-pin BGA, the part this build targets)

| Bank | Capacity | ECC | Source |
|---|---|---|---|
| Code MRAM | 1024 KB | n/a | DS Table 1.15 p 11 (`Code MRAM` = "1 MB, 512 KB") |
| User SRAM | 1664 KB | yes | DS Table 1.15 p 11 (`SRAM`, `R7KA8P1KxxCAC` column) |
| M85 (CPU0) ITCM | 128 KB, 16 blocks x 8 KB | yes | HUM 2.1.1 p 111 |
| M85 (CPU0) DTCM | 128 KB, 16 blocks x 8 KB | yes | HUM 2.1.1 p 111 |
| M85 (CPU0) L1 I-cache | 16 KB | yes | HUM 2.1.1 p 111 |
| M85 (CPU0) L1 D-cache | 16 KB | yes | HUM 2.1.1 p 111 |
| M33 (CPU1) CTCM | 64 KB | yes | HUM 2.1.1 p 112 |
| M33 (CPU1) STCM | 64 KB | yes | HUM 2.1.1 p 112 |
| M33 (CPU1) C-Cache (code bus) | 16 KB | yes | HUM 2.1.1 p 112 |
| M33 (CPU1) S-Cache (system bus) | 16 KB | yes | HUM 2.1.1 p 112 |

`DS` is the RA8P1 datasheet R01DS0439EJ0130 Rev.1.30, committed as
[`ra8p1-datasheet.pdf`](ra8p1-datasheet.pdf), so every DS row above is
re-checkable from this tree.

`HUM` is the per-bank split. **Read the substitution note below before citing
it.** It is re-derived here from the **RA8D2** HUM R01UH1065EJ0130 Rev.1.30,
committed as [`ra8d2-hardware-user-manual.pdf`](ra8d2-hardware-user-manual.pdf),
section 2.1.1 "CPU", printed pp 111-112 (corroborated by section 2.3
"Implementation Options", `TCM` row p 115 and `CACHE` row p 116) -- because the
RA8P1 HUM is ~49 MB and is **not** in the tree. Issue #850 cites RA8P1 HUM
R01UH1064EJ0130 sections 2.1.1 pp 111-112 and 2.16.1.1 Table 2.34 p 160 for the
same numbers.

Per this file's own standing rule ("verify per claim, not per document"), that
substitution is declared, bounded, and cross-checked rather than assumed:

- The two datasheets' function-comparison tables carry **identical** CPU0/CPU1
  cache and TCM rows: RA8P1 Table 1.15 p 11 and RA8D2 Table 1.14 p 11 both read
  `CPU0 TCM 256 KB`, `CPU1 TCM 128 KB`, `CPU0 I/D Caches 32 KB`,
  `CPU1 C/S Caches 32 KB`, `SRAM 1664 KB` for the `KxxCAC` column. Both files
  are in the tree, so that comparison is re-checkable.
- The RA8D2 per-bank split multiplies out to exactly those shared totals:
  128+128 = 256 KB CPU0 TCM, 16+16 = 32 KB CPU0 I/D caches, 64+64 = 128 KB CPU1
  TCM, 16+16 = 32 KB CPU1 C/S caches. Four independent products, four exact
  matches.
- **Still owed:** a direct read of RA8P1 HUM 2.1.1 / 2.16.1.1. The claim above
  is a cross-checked inference from two in-tree documents plus the issue's
  citation, not a page this tree can show you. Do not upgrade it to a direct
  RA8P1 HUM citation without opening that manual.

### Core-integrated vs bus cache

The M85's I-cache and D-cache are Arm **core-integrated** L1, and CMSIS reports
them. The M33's C-Cache (code bus) and S-Cache (system bus) are **Renesas bus
caches sitting outside the Arm core**. A CMSIS flag saying the M33 has no
core-integrated Arm L1 cache is therefore true and irrelevant: it does not
establish the absence of the Renesas bus caches. The earlier "M33 is cacheless"
reading took an Arm-core fact for a Renesas-part fact.

### Count every region exactly once

    1664 KB user SRAM  +  256 KB M85 TCM  +  128 KB M33 TCM  =  2048 KB

2048 KB is the "2 MB SRAM" of the datasheet headline (DS p 1, and DS p 2 spells
it out: *"2 MB SRAM (256 KB of CM85 TCM RAM, 128 KB CM33 TCM RAM, 1664 KB of
user SRAM)"*). TCM is **carved out of** that 2 MB island, never added on top of
it. "1664 KB user SRAM" and "2 MB total RAM" are both correct and must not be
summed. The single-core SKUs corroborate the carve-out exactly: 1792 KB user
SRAM + 256 KB M85 TCM + no M33 TCM = the same 2048 KB (DS Table 1.15 p 11).

### Silicon capacity is not supported allocation

`libs/ra8_core/inc/ra8_device.h` now keeps the two apart:
`ra8_device_mem_capacity_t` holds the capacities above;
`ra8_device_mem_size_t` holds what this firmware declares. The M85 TCM entries
in the second **stay at the 64 KB per-bank floor** that every app's
`MEMORY { }` block declares today, even though each bank is 128 KB.

That floor is retained deliberately, not by oversight. Raising it is not a table
correction:

- The ITCM and DTCM banks are ECC and 16-block-granular (8 KB per block), so a
  larger declared region changes what `Reset_Handler` must copy, zero and
  ECC-initialize before the first read of those blocks. That audit has not been
  done.
- 64 KB is what `ra8_emulator` maps (`k_dtcm_end == 0x20010000` in
  `tools/ra8_emulator/inc/emu_memmap.h`), so expanding the linker region without
  the emulator would silently split host and target behaviour.
- The HIL-validated images under `examples/ek_ra8d2/hw_validated/` were all
  linked against the 64 KB windows. Re-linking them is a re-validation, which
  needs a board.

So: **do not expand a region or enable a cache on the strength of this table.**
Region growth belongs to the startup/ECC audit plus a silicon run (issues #226 /
#229); linker/target composition belongs to #758 / #761; broader M85 cache and
MPU conversion belongs to #590 / #591. `tests/core/src/test_ra8_device_geometry.c`
pins the capacities, pins the floor, and fails if supported allocation ever
exceeds capacity, so the distinction cannot quietly erode.

### Known drift left alone, on purpose

The `MEMORY { }` header comment in the 78 per-app `linker_script.ld` files reads
`SRAM (ECC) 0x22000000 2 MiB` while the same script declares
`SRAM (rwx) : ORIGIN = 0x22000000, LENGTH = 1024K`. The comment is the
double-count this section corrects (user SRAM is 1664 KB, and 2 MiB is the whole
island including TCM); the declaration is a further deliberate floor. Both are
RA8D2-side linker composition, owned by #758 / #761, and touching 78 scripts
here would be region churn outside this reconciliation. Recorded, not changed.

## Sources

RA8P1 HUM R01UH1064EJ0130 Rev.1.30 and datasheet R01DS0439EJ0130 Rev.1.30 (both
read directly and full-text searched -- see the two "Correction" sections above).

> **Verify per claim, not per document.** The RA8P1 **datasheet** is now
> committed as `docs/reference/ra8p1-datasheet.pdf`, so any claim sourced from
> it is re-checkable from the tree. The RA8P1 **HUM** is not (it is ~49 MB), so
> "the manual was searched" is still not something a reader can re-check for
> HUM-sourced claims. Both corrections above are cases where a
> line in this file cited the RA8P1 HUM for something the RA8P1 HUM does not say
> -- the OFS3 claim came from an FSP feature flag alone. When a claim here drives
> a code change, re-derive it from the manual and record the section AND printed
> page, as the corrections do. FSP metadata is a lead, never a citation.

FSP `github.com/renesas/fsp`
(`R7KA8{P1,D2}KF_core0.h`, `bsp/mcu/ra8{p1,d2}/{bsp_elc,bsp_feature,bsp_peripheral}.h`,
`bsp/mcu/all/bsp_module_stop.h`, `ra/board/ra8p1_ek/board.h`); Zephyr
`dts/arm/renesas/ra/ra8/r7ka8{p1,d2}kflcac*.dtsi`; Renesas part page
`r7ka8p1kflcac-uc0`.
