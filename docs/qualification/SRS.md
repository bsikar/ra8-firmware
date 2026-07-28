# Software Requirements Specification (SRS)

**Last refreshed**: 2026-05-03 (REQ-SAFE-016 met against the
reachable-MC/DC gate; HIL posture re-stated as developer-laptop
pre-push).

**Status**: First draft, 2026-05-02. Authored against the Phase 7 schedule
in [`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md) Section 3.
**DO-178C reference**: Section 11.9 (Software Requirements Data).
**IEC 61508-3 reference**: Clause 7.2 (Software safety requirements
specification).
**ISO 26262-6 reference**: Clause 6 (Specification of software safety
requirements).
**Project**: `ra8-firmware`.
**Maintainer**: Brighton Sikarskie (single developer / requirements
author / verifier).

This SRS is referenced by [`./PSAC.md`](./PSAC.md) Section 5 (Software
life cycle data, row "Software requirements data") and by
[`./SVP.md`](./SVP.md) Section 1.1 (Table A-3 verification of the
outputs of the software requirements process). The contents enumerate
**every requirement that an implementation in this tree is obliged to
satisfy** and bind each REQ-XXX item to a specific source file plus a
test artefact (or "no test yet" / "BLOCKED-VENDOR" where coverage is
absent).

The numbering scheme is `REQ-<RING>-<NNN>` where `<RING>` is the
architectural ring per [`../RING_AND_WORLD.md`](../RING_AND_WORLD.md)
and `<NNN>` is a zero-padded serial. Numbers are never reused. New
requirements are appended; obsolete requirements are marked
`[REMOVED]` rather than deleted so that historical traceability is
preserved.

---

## 1. Purpose and scope

### 1.1 Purpose

`ra8-firmware` is bare-metal firmware for the Renesas RA8D2 MCU
group, exercised on the EK-RA8D2 evaluation kit (Renesas part number
968-K7EKA8D2S01001BE). The firmware is a personal in-house exploration
codebase whose long-horizon goal is to be qualifiable against the
assurance levels in Section 1.3.

This SRS captures **what** the firmware must do, in numbered REQ-XXX
form. It deliberately does not specify **how** -- the design rationale,
module decomposition, and algorithm choices live in
[`./SDD.md`](./SDD.md).

### 1.2 Scope

In scope:

- The first-party C/C++ code under [`../../libs/`](../../libs/) and
  [`../../src/`](../../src/).
- The application binaries under
  [`../../examples/ek_ra8d2/`](../../examples/ek_ra8d2/).
- The TrustZone-M Secure / Non-Secure partition described in
  [`../RING_AND_WORLD.md`](../RING_AND_WORLD.md).
- Host-side verification artefacts under [`../../tests/`](../../tests/).

Out of scope:

- The Renesas FSP source tree (used as reference only; no FSP code is
  shipped in this tree, see [`../../CLAUDE.md`](../../CLAUDE.md)
  Section "Development Approach").
- The Cortex-M33 secondary core (not currently built; reserved for
  future work).
- Vendor binary blobs (RSIP-E50D firmware image, BLE controller patch
  image) -- vendored under
  [`../../libs/third_party/fsp_blobs/`](../../libs/third_party/fsp_blobs/)
  and tracked under SOUP per `docs/SOUP/`.

### 1.3 Target assurance levels

Per [`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md)
Section 1, the anchor is **IEC 61508-3:2010 SIL 3**, with **DO-178C
Level B** and **ISO 26262-6 ASIL C/D** mapped in parallel. Every
requirement in Section 4 below is taggable against the side-by-side
objective map in the roadmap.

### 1.4 Document conventions

- Requirement IDs use the `REQ-<RING>-<NNN>` pattern.
- Each requirement carries a **source** column (the file that
  implements it) and a **test** column (the file that verifies it).
- `TBD` = no test exists yet; tracked as a verification gap by
  [`./SVP.md`](./SVP.md).
- `BLOCKED-VENDOR` = verification depends on a vendor blob that is not
  available in this tree.
- All file paths are repository-relative.

---

## 2. System overview

### 2.1 Target hardware

| Item              | Value                                                                                          |
|-------------------|------------------------------------------------------------------------------------------------|
| MCU               | Renesas R7KA8D2KFLCAC, RA8D2 group, 289-pin BGA, 12 mm x 12 mm, 0.65 mm pitch.                |
| Primary core      | Arm Cortex-M85 @ 1 GHz with the Helium / MVE extension.                                        |
| Secondary core    | Arm Cortex-M33 @ 250 MHz (currently unused; no M33 image is built).                            |
| Code memory       | 1 MiB MRAM (non-volatile), secure alias `0x02000000`, NS alias `0x02080000`.                   |
| System RAM        | 2 MiB ECC SRAM, secure alias `0x22000000`, NS alias `0x22100000`.                              |
| TCM               | 64 KiB ITCM @ `0x00000000`, 64 KiB DTCM @ `0x20000000` (per-core, M85).                        |
| External flash    | 64 MiB Octo-SPI NOR (EK-RA8D2 v1 population).                                                  |
| External RAM      | 64 MiB SDRAM @ `0x68000000` (EK-RA8D2 v1 population).                                          |
| Display           | 7.0-inch 1024x600 parallel TFT, OV5640 5 MP camera.                                            |
| Debugger          | On-board SEGGER J-Link OB (SWD/JTAG), VCOM via J10.                                            |
| Toolchain         | ARM GNU Toolchain (arm-none-eabi-gcc 13) + CMake >= 3.20.                                      |
| RTOS              | None first-party (bare-metal). ThreadX 6.5.0 admitted as SOUP.                                 |

The authoritative chip-level reference is the Renesas Hardware User's
Manual (HUM) **R01UH1065EJ**, committed under
[`../reference/`](../reference/). Every register-level requirement in
Section 4 cites a HUM chapter via the source file's `@cite` doxygen
tags, audited by `scripts/checks/cite_check.py`.

### 2.2 Memory map summary

The full memory map is in [`../MEMORY_MAP.md`](../MEMORY_MAP.md). The
salient regions for requirement traceability are:

| Region              | Base         | Length     | Owner                                                                        |
|---------------------|--------------|------------|------------------------------------------------------------------------------|
| ITCM                | `0x00000000` | 64 KiB     | M85 hot-path code (placed by per-app linker script).                         |
| MRAM (S)            | `0x02000000` | 1 MiB      | Vector table, code, rodata, OFS bytes.                                       |
| MRAM (NS alias)     | `0x02080000` | 512 KiB    | NS image alias.                                                              |
| Factory cal (TSN)   | `0x02C1EDA0` | --         | TSN factory-calibration window.                                              |
| DTCM                | `0x20000000` | 64 KiB     | Hot-path data (DMA descriptors, scratch buffers).                            |
| SRAM (S)            | `0x22000000` | 2 MiB      | Stacks, .data, .bss, framebuffers; ECC enabled by `ra8_sram_init`.            |
| SRAM (NS alias)     | `0x22100000` | 1 MiB      | NS image SRAM partition.                                                     |
| SDRAM               | `0x68000000` | 64 MiB     | Driven by `ra8_sdramc.c`.                                                     |
| Peripheral window   | `0x40000000` | --         | Per-peripheral base addresses in `libs/ra8_hal/inc/ra8d2_*_regs.h`.           |
| Core MPU registers  | `0xE000ED90` | --         | Cortex-M85 MPU control.                                                      |

### 2.3 Peripheral inventory

Enumerated from the Ring-2 register headers under
[`../../libs/ra8_hal/inc/`](../../libs/ra8_hal/inc/) (62 `_regs.h`
files) and the Ring-3 driver sources under
[`../../libs/ra8_hal/src/`](../../libs/ra8_hal/src/) (93 driver TUs).
The full driver-by-driver requirement table is Section 4.3 below.

Peripheral families covered: ACMPHS, ADC, AGT, BKUP, BLE, BSCAN, CAC,
CANFD, CEU, CGC, CNECC, CRC, DAC-B, DMA/DMAC, DOC, DOTF, DRW, DTC,
ELC, ePaper (CMI), Ethernet (ETH/ETHA/RMAC/PHY/PTP/coma/gptp/gwca/mfwd),
Flash, GLCDC, GPT, I3C, ICU, IIC-B (controller + peripheral), IPC,
ISR, IWDT, JPEG-SW, Layer-3 switch, LPM, LVD, MIPI (CSI/DSI/PHY),
MPC, MSTP, OFS, PDG, PDM, POEG, PWR, RESET, RMAC PHY, RSIP (3 sub-
modules), RTC, SCI, SD card, SDHI, SDRAMC, SMBus, SPI-B, SRAM,
SSIE, Touch, TSN, ULPT, USB device + host (CDC, HID, MSC, audio,
printer, vendor, hub, composite), VIN, VREG, WDT, xSPI.

---

## 3. Glossary and acronyms

The full acronym list is in [`../ACRONYMS.md`](../ACRONYMS.md). Within
this SRS the recurring abbreviations are:

| Term      | Meaning                                                                       |
|-----------|-------------------------------------------------------------------------------|
| BSP       | Board Support Package (per-app boot files + linker script).                   |
| CGC       | Clock Generation Circuit.                                                     |
| ECC       | Error-Correcting Code (SRAM).                                                 |
| EVM       | Evaluation Module (the EK-RA8D2 board).                                       |
| HAL       | Hardware Abstraction Layer.                                                   |
| HUM       | Hardware User's Manual (R01UH1065EJ).                                         |
| ISR       | Interrupt Service Routine.                                                    |
| MC/DC     | Modified Condition / Decision Coverage.                                       |
| MPU       | Memory Protection Unit (core MPU at `0xE000ED90`, separate from bus MMPU).    |
| NS / NSC  | Non-Secure / Non-Secure-Callable (Armv8-M Security Extension).                |
| OFS       | Option-Function Select bytes (boot-time configuration).                       |
| OTA       | Over-The-Air firmware update.                                                 |
| PAL       | Platform Abstraction Layer.                                                   |
| PSA       | Platform Security Architecture (PSA Crypto API).                              |
| PFS       | Pin Function Select (IOPORT register).                                        |
| RSIP      | Renesas Secure IP block.                                                      |
| S         | Secure world.                                                                 |
| SAU       | Security Attribution Unit.                                                    |
| SOUP      | Software Of Unknown Provenance.                                               |

---

## 4. Software requirements

The requirements are grouped by architectural ring per
[`../RING_AND_WORLD.md`](../RING_AND_WORLD.md). Higher rings may only
call lower rings (Section 4.1 of this document re-states the rule).

### 4.1 Ring 0 -- chip / BSP requirements (REQ-CHIP-XXX)

Ring 0 is silicon register definitions plus the per-app boot files
(`vector_table.c`, `system_init.c`, `secure_exception.c`,
`trustzone_init.c`, `linker_script.ld`).

| ID               | Requirement                                                                                              | Source                                                                                          | Test                                                |
|------------------|----------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------|-----------------------------------------------------|
| REQ-CHIP-001     | Each app SHALL provide a vector table at the MRAM base populated with the Reset_Handler entry point.     | `examples/ek_ra8d2/blink/vector_table.c` (template; copied per app)                            | `tests/test_app_blink_hal.c`                         |
| REQ-CHIP-002     | Each app SHALL provide a `SystemInit` that runs before `main` and brings the CGC up to PLL-driven mode.  | `examples/ek_ra8d2/blink/system_init.c`                                                         | `tests/test_app_clock_check.c`                       |
| REQ-CHIP-003     | Each app SHALL provide a SecureFault handler that traps to `ra8_error_handler`.                           | `examples/ek_ra8d2/blink/secure_exception.c`                                                    | `tests/test_ra8_error_handler.c`                      |
| REQ-CHIP-004     | Each app SHALL bring up the SAU per `trustzone_init.c` before transitioning to NS code (where applicable).| `examples/ek_ra8d2/blink/trustzone_init.c`                                                      | `tests/test_ra8_fake_world.c`                          |
| REQ-CHIP-005     | The linker script SHALL place `.vectors` at `0x02000000` (MRAM-S base) and reserve OFS bytes per HUM Ch 6. | `examples/ek_ra8d2/blink/linker_script.ld`                                                      | `tests/test_ra8_ofs.c`                                |
| REQ-CHIP-006     | Each Ring-2 register header SHALL declare the peripheral base address as a `uintptr_t` typed enum.       | `libs/ra8_hal/inc/ra8d2_*_regs.h` (62 files)                                                     | `tests/test_coverage_compile_all.c`                  |
| REQ-CHIP-007     | OFS bytes SHALL be initialized to a state that disables the IWDT and selects the M85 boot mode at reset. | `libs/ra8_hal/src/ra8_ofs.c`                                                                       | `tests/test_ra8_ofs.c`                                |

### 4.2 Ring 1 -- core utilities (REQ-CORE-XXX)

Ring 1 is the host-clean core under
[`../../libs/ra8_core/`](../../libs/ra8_core/). Compiles identically on
target and on the host test runner.

| ID               | Requirement                                                                                                                          | Source                                                          | Test                                  |
|------------------|--------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------|---------------------------------------|
| REQ-CORE-001     | The error-code domain `ra8_err_t` SHALL be a typed enum and SHALL include `k_ra8_ok` as the unique success value.                       | `libs/ra8_core/inc/ra8_err.h`                                     | `tests/test_ra8_err.c`                 |
| REQ-CORE-002     | A textual rendering function SHALL exist for every `ra8_err_t` value.                                                                  | `libs/ra8_core/inc/ra8_err.h` (`ra8_err_to_str`)                   | `tests/test_ra8_err_to_str.c`          |
| REQ-CORE-003     | The `RA8_RETURN_ON_ERROR(expr,tag,msg)` macro SHALL log on non-`k_ra8_ok` and propagate the original code unchanged.                    | `libs/ra8_core/inc/ra8_check.h`                                   | `tests/test_ra8_err.c`                 |
| REQ-CORE-004     | The log subsystem SHALL provide `ra8_log_{error,warn,info,debug}` with compile-time level gating.                                      | `libs/ra8_core/inc/ra8_log.h`, `libs/ra8_core/src/ra8_log.c`        | `tests/test_ra8_log.c`                 |
| REQ-CORE-005     | A monotonic millisecond time source SHALL exist via `ra8_time_now_ms()` driven by SysTick.                                              | `libs/ra8_core/inc/ra8_time.h`, `libs/ra8_core/src/ra8_time.c`      | `tests/test_ra8_time.c`, `tests/test_ra8_fake_time.c` |
| REQ-CORE-006     | A pin-validator SHALL refuse double-allocation of any IOPORT pin during system init.                                                   | `libs/ra8_core/inc/ra8_pin_validator.h`, `libs/ra8_core/src/ra8_pin_validator.c` | `tests/test_ra8_pin_validator.c` |
| REQ-CORE-007     | A register-protection helper SHALL unlock PRCR / PWPR around protected writes and re-lock on exit.                                     | `libs/ra8_core/inc/ra8_register_protection.h`                     | `tests/test_ra8_register_protection.c` |
| REQ-CORE-008     | A register-guard helper SHALL detect re-entrant attempts to enter a protection window and assert.                                      | `libs/ra8_core/inc/ra8_register_guard.h`                          | `tests/test_ra8_register_guard.c`      |
| REQ-CORE-009     | A central exception entry point SHALL exist for HardFault / BusFault / UsageFault / MemManage / SecureFault.                           | `libs/ra8_core/inc/ra8_exception.h`, `libs/ra8_core/src/ra8_exception.c` | `tests/test_ra8_exception.c`      |
| REQ-CORE-010     | A central error handler SHALL provide a single bottleneck (`ra8_error_handler`) that logs context and halts in a controlled way.        | `libs/ra8_core/inc/ra8_error_handler.h`, `libs/ra8_core/src/ra8_error_handler.c` | `tests/test_ra8_error_handler.c` |
| REQ-CORE-011     | An infrastructure init function SHALL bring up logging, time, pin validator, and register-protection before any HAL driver runs.        | `libs/ra8_core/inc/ra8_infrastructure.h`, `libs/ra8_core/src/ra8_infrastructure.c` | `tests/test_ra8_infrastructure.c` |
| REQ-CORE-012     | A `sbrk` trap SHALL refuse all heap allocation requests at link time (NASA P10 Rule 3).                                                | `libs/ra8_core/src/ra8_sbrk_trap.c`                               | (compile-time) `scripts/checks/check_no_dynamic_alloc.py` |
| REQ-CORE-013     | A static stack-budget header SHALL declare per-task stack sizes in one place.                                                          | `libs/ra8_core/inc/ra8_stack_budget.h`                            | TBD (manual review against `docs/STACK_USAGE.md`) |
| REQ-CORE-014     | All bit-shift / mask / GPIO constants used by Ring-2/3 code SHALL be declared as typed enums in a single core header per concern.       | `libs/ra8_core/inc/ra8_bit_constants.h`, `ra8_gpio_constants.h`, `ra8_port_constants.h`, `ra8_time_constants.h` | `tests/test_ra8_bit_constants.c`, `tests/test_ra8_port_constants.c` |

### 4.3 Ring 2/3 -- HAL drivers (REQ-DRV-XXX)

Ring 2 is the silicon register layout headers
(`libs/ra8_hal/inc/ra8d2_*_regs.h`) and Ring 3 is the driver
implementations (`libs/ra8_hal/src/ra8_*.c`). One driver = one REQ-DRV
row. Where the public header bundles multiple registers (e.g. `ra8_eth.h`
covers ETH/ETHA/RMAC/PHY) the driver is split across multiple `.c`
files but a single REQ-DRV ID applies.

| ID               | Driver           | Requirement summary                                                                                                       | Source (`libs/ra8_hal/`)         | Test (`tests/`)                          |
|------------------|------------------|---------------------------------------------------------------------------------------------------------------------------|----------------------------------|------------------------------------------|
| REQ-DRV-001      | ra8_acmphs        | High-speed analog comparator init, channel enable, polarity select.                                                        | `src/ra8_acmphs.c`                | `test_ra8_acmphs.c`                       |
| REQ-DRV-002      | ra8_adc           | 12-bit ADC channel scan, single-shot conversion, result read.                                                              | `src/adc.c` + raw register access | `test_ra8_adc.c`, `test_adc.c`           |
| REQ-DRV-003      | ra8_agt           | Asynchronous General Purpose Timer init + period set.                                                                      | `src/ra8_agt.c`                   | `test_ra8_agt.c`                          |
| REQ-DRV-004      | ra8_bkup          | Battery-backup register read / write across VBATT.                                                                         | `src/ra8_bkup.c`                  | `test_ra8_bkup.c`                         |
| REQ-DRV-005      | ra8_ble           | BLE HCI transport seam (in-memory loopback; controller resides on the ESP32-C6 companion).                                  | `src/ra8_ble.c`                   | `test_ra8_ble.c`                          |
| REQ-DRV-007      | ra8_bscan         | Boundary-scan / bus-monitor configuration.                                                                                  | `src/ra8_bscan.c`                 | `test_ra8_bscan.c`                        |
| REQ-DRV-008      | ra8_cac           | Clock Frequency Accuracy Measurement Circuit init + measurement.                                                            | `src/ra8_cac.c`                   | `test_ra8_cac.c`                          |
| REQ-DRV-009      | ra8_canfd         | CAN-FD controller init, bit-timing, frame transmit/receive.                                                                 | `src/ra8_canfd.c`                 | `test_ra8_canfd.c`                        |
| REQ-DRV-010      | ra8_ceu           | Camera Encoding Unit init + frame capture path setup.                                                                       | `src/ra8_ceu.c`                   | `test_ra8_ceu_capture.c`, `test_ra8_ceu_config.c` |
| REQ-DRV-011      | ra8_cgc           | Clock Generation Circuit: PLL setup, source select, peripheral-clock gating.                                                | `src/ra8_cgc.c`                   | `test_ra8_cgc.c`                          |
| REQ-DRV-012      | ra8_cnecc         | Code/Number ECC controller init + scrub.                                                                                    | `src/ra8_cnecc.c`                 | `test_ra8_cnecc.c`                        |
| REQ-DRV-013      | ra8_crc           | Hardware CRC engine: 8/16/32-bit polynomial selection, calc.                                                                | `src/ra8_crc.c`                   | `test_ra8_crc.c`                          |
| REQ-DRV-014      | ra8_dac_b         | 12-bit DAC channel write.                                                                                                   | `src/ra8_dac_b.c`                 | `test_ra8_dac_b.c`                        |
| REQ-DRV-015      | ra8_dma           | DMA shared-state init.                                                                                                       | `src/ra8_dma.c`                   | `test_ra8_dma.c`                          |
| REQ-DRV-016      | ra8_dmac          | DMAC channel init + transfer descriptor.                                                                                    | `src/ra8_dmac.c`                  | `test_ra8_dmac.c`                         |
| REQ-DRV-017      | ra8_doc           | Data Operation Circuit (compare / accumulate).                                                                              | `src/ra8_doc.c`                   | `test_ra8_doc.c`                          |
| REQ-DRV-018      | ra8_dotf          | Decryption-On-The-Fly (XIP-decrypt) configuration.                                                                          | `src/ra8_dotf.c`                  | `test_ra8_dotf.c`                         |
| REQ-DRV-019      | ra8_drw           | DRW (2D draw engine) init + blit op.                                                                                         | `src/ra8_drw.c`                   | `test_ra8_drw.c`                          |
| REQ-DRV-020      | ra8_dtc           | Data Transfer Controller init + descriptor-list installation.                                                                | `src/ra8_dtc.c`                   | `test_ra8_dtc.c`                          |
| REQ-DRV-021      | ra8_elc           | Event Link Controller wiring (peripheral-to-peripheral events).                                                              | `src/ra8_elc.c`                   | `test_ra8_elc.c`                          |
| REQ-DRV-022      | ra8_epaper        | Parallel ePaper / CMI panel init + refresh.                                                                                  | `src/ra8_epaper.c`                | `test_ra8_epaper.c`                       |
| REQ-DRV-023      | ra8_eth           | Top-level Ethernet aggregation (calls coma/gptp/gwca/mfwd/etha as needed).                                                   | `src/ra8_eth.c`                   | `test_ra8_eth.c`                          |
| REQ-DRV-024      | ra8_eth_coma      | Ethernet Common-Manager (COMA) init.                                                                                         | `src/ra8_eth_coma.c`              | `test_ra8_eth_coma.c`                     |
| REQ-DRV-025      | ra8_eth_gptp      | Generic PTP (gPTP) hardware time-stamping setup.                                                                             | `src/ra8_eth_gptp.c`              | `test_ra8_eth_gptp.c`                     |
| REQ-DRV-026      | ra8_eth_gwca      | Gateway CPU Agent init (descriptor rings).                                                                                   | `src/ra8_eth_gwca.c`              | `test_ra8_eth_gwca.c`                     |
| REQ-DRV-027      | ra8_eth_mfwd      | Multi-port forwarding configuration.                                                                                          | `src/ra8_eth_mfwd.c`              | `test_ra8_eth_mfwd.c`                     |
| REQ-DRV-028      | ra8_etha          | Ethernet Agent (per-port DMA + MAC).                                                                                          | `src/ra8_etha.c`                  | `test_ra8_etha.c`, `test_ra8_etha_rmac_edge_cases.c` |
| REQ-DRV-029      | ra8_ether_phy     | MII/RMII PHY register access via management frame.                                                                            | `src/ra8_ether_phy.c`             | `test_ra8_ether_phy.c`                    |
| REQ-DRV-030      | ra8_flash         | On-chip MRAM erase + program (HP-flash semantics).                                                                            | `src/ra8_flash.c`                 | `test_ra8_flash.c`, `test_ra8_flash_edge_cases.c` |
| REQ-DRV-031      | ra8_glcdc         | Graphics LCD Controller: layer config, framebuffer pointer, line/dot timing.                                                 | `src/ra8_glcdc.c`                 | `test_ra8_glcdc.c`                        |
| REQ-DRV-032      | ra8_gpt           | General PWM Timer init + duty/frequency set.                                                                                  | `src/ra8_gpt.c`                   | `test_ra8_gpt.c`                          |
| REQ-DRV-033      | ra8_gpio          | IOPORT pin direction + drive-level helpers.                                                                                   | `src/gpio.c`                     | `test_ra8_gpio.c`                         |
| REQ-DRV-034      | ra8_hw_err        | Hardware-error aggregation (NMI / parity / bus error decode).                                                                  | `inc/ra8_hw_err.h`                | `test_ra8_hw_err.c`                       |
| REQ-DRV-035      | ra8_i3c           | I3C controller init + dynamic-address assignment.                                                                              | `src/ra8_i3c.c`                   | `test_ra8_i3c.c`                          |
| REQ-DRV-036      | ra8_icu           | ICU IRQ-line + edge-select configuration.                                                                                      | `src/ra8_icu.c`                   | `test_ra8_icu.c`                          |
| REQ-DRV-037      | ra8_iic_b         | I2C controller-mode driver (target = peripheral).                                                                              | `src/ra8_iic_b.c`                 | `test_ra8_iic_b.c`, `test_ra8_iic_b_edge_cases.c` |
| REQ-DRV-038      | ra8_i2c_peripheral   | I2C peripheral-mode driver.                                                                                                    | `src/ra8_i2c_peripheral.c`           | `test_ra8_riic_peripheral.c`                  |
| REQ-DRV-039      | ra8_ipc           | Inter-processor communication (M85 <-> M33) channel init.                                                                      | `src/ra8_ipc.c`                   | `test_ra8_ipc.c`                          |
| REQ-DRV-040      | ra8_isr           | NVIC priority assignment, vector installation, masked-region helper.                                                            | `src/ra8_isr.c`                   | `test_ra8_isr.c`                          |
| REQ-DRV-041      | ra8_iwdt          | Independent Watchdog enable + refresh.                                                                                          | `src/ra8_iwdt.c`                  | `test_ra8_iwdt.c`                         |
| REQ-DRV-042      | ra8_jpeg_sw       | Software JPEG decode (when no HW JCU is enabled).                                                                               | `src/ra8_jpeg_sw.c`               | `test_ra8_jpeg_sw.c`                      |
| REQ-DRV-043      | ra8_layer3_switch | L3 switch table programming.                                                                                                    | `src/ra8_layer3_switch.c`         | `test_ra8_layer3_switch.c`                |
| REQ-DRV-044      | ra8_lpm           | Low-Power-Mode entry/exit (sleep / standby / deep-standby).                                                                     | `src/ra8_lpm.c`                   | `test_ra8_lpm.c`                          |
| REQ-DRV-045      | ra8_lvd           | Low-Voltage Detection threshold + interrupt setup.                                                                              | `src/ra8_lvd.c`                   | `test_ra8_lvd.c`                          |
| REQ-DRV-046      | ra8_mipi_csi      | MIPI CSI-2 receiver (camera path) init.                                                                                          | `src/ra8_mipi_csi.c`              | `test_ra8_mipi_csi_init.c`, `test_ra8_mipi_csi_events.c` |
| REQ-DRV-047      | ra8_mipi_dsi      | MIPI DSI display-side init + commands.                                                                                          | `src/ra8_mipi_dsi.c`              | `test_ra8_mipi_dsi_cmd.c`, `test_ra8_mipi_dsi_video.c`, `test_ra8_mipi_dsi_mcdc.c` |
| REQ-DRV-048      | ra8_mipi_phy      | Shared MIPI D-PHY analog setup.                                                                                                  | `src/ra8_mipi_phy.c`              | `test_ra8_mipi_phy_init.c`, `test_ra8_mipi_phy_lanes.c` |
| REQ-DRV-049      | ra8_mpc           | Multi-Function Pin Controller (PFS) write helpers.                                                                              | `src/ra8_mpc.c`                   | `test_ra8_mpc.c`                          |
| REQ-DRV-050      | ra8_mstp          | Module-Stop register clear / set per peripheral.                                                                                | `src/ra8_mstp.c`                  | `test_ra8_mstp.c`                         |
| REQ-DRV-051      | ra8_ofs           | OFS register read / write helpers.                                                                                               | `src/ra8_ofs.c`                   | `test_ra8_ofs.c`                          |
| REQ-DRV-052      | ra8_pdg           | PDG (Programmable Delay Generator) configuration.                                                                                | `src/ra8_pdg.c`                   | `test_ra8_pdg.c`                          |
| REQ-DRV-053      | ra8_pdm           | PDM microphone interface.                                                                                                         | `src/ra8_pdm.c`                   | `test_ra8_pdm.c`                          |
| REQ-DRV-054      | ra8_poeg          | Port-Output-Enable Gate (motor-safety) configuration.                                                                            | `src/ra8_poeg.c`                  | `test_ra8_poeg.c`                         |
| REQ-DRV-055      | ra8_ptp           | Precision Time Protocol (IEEE 1588) HW timestamp.                                                                                 | `src/ra8_ptp.c`                   | `test_ra8_ptp.c`                          |
| REQ-DRV-056      | ra8_pwr           | Power / regulator / VBATT control.                                                                                                | `src/ra8_pwr.c`                   | `test_ra8_pwr.c`                          |
| REQ-DRV-057      | ra8_reset         | Software reset trigger + reset-cause readout.                                                                                     | `src/ra8_reset.c`                 | `test_ra8_reset.c`                        |
| REQ-DRV-058      | ra8_rmac          | RMAC (Renesas-specific MAC subset) init.                                                                                           | `src/ra8_rmac.c`                  | `test_ra8_rmac.c`                         |
| REQ-DRV-059      | ra8_rmac_phy      | RMAC PHY-side helpers.                                                                                                             | `src/ra8_rmac_phy.c`              | `test_ra8_rmac_phy.c`                     |
| REQ-DRV-060      | ra8_rsip          | Renesas Secure IP API surface (BLOCKED-VENDOR for production-grade key wrap).                                                      | `src/ra8_rsip.c`                  | `test_ra8_rsip_core.c`, `test_ra8_rsip_sym.c`, `test_ra8_rsip_devsec.c`, `test_ra8_rsip_edge_cases.c` (software emulator only) |
| REQ-DRV-061      | ra8_rsip_key_injection | RSIP key-injection sub-API.                                                                                                  | `src/ra8_rsip_key_injection.c`    | `test_ra8_rsip_key_injection.c` (BLOCKED-VENDOR for HW path) |
| REQ-DRV-062      | ra8_rsip_protected | RSIP protected-mode session API.                                                                                                  | `src/ra8_rsip_protected.c`        | `test_ra8_rsip_protected.c` (BLOCKED-VENDOR for HW path) |
| REQ-DRV-063      | ra8_rtc           | Real-Time Clock init + alarm.                                                                                                     | `src/ra8_rtc.c`                   | `test_ra8_rtc.c`                          |
| REQ-DRV-064      | ra8_sci           | Serial Communication Interface (UART/SPI/I2C-mode) init + transfer.                                                                | `src/ra8_sci.c`, `src/uart.c`     | `test_ra8_sci.c`, `test_ra8_uart.c`        |
| REQ-DRV-065      | ra8_sdcard        | SD-card protocol layer over SDHI.                                                                                                  | `src/ra8_sdcard.c`                | `test_ra8_sdcard.c`                       |
| REQ-DRV-066      | ra8_sdhi          | SD/MMC Host Interface init + R/W block.                                                                                            | `src/ra8_sdhi.c`                  | `test_ra8_sdhi.c`                         |
| REQ-DRV-067      | ra8_sdramc        | SDRAM controller init for the EK-RA8D2 64 MiB part at `0x68000000`.                                                                  | `src/ra8_sdramc.c`                | `test_ra8_sdramc.c`                       |
| REQ-DRV-068      | ra8_smbus         | SMBus protocol layer over IIC-B.                                                                                                    | `src/ra8_smbus.c`                 | `test_ra8_smbus.c`                        |
| REQ-DRV-069      | ra8_spi_b         | SPI controller-mode driver (B-variant).                                                                                              | `src/ra8_spi_b.c`                 | `test_ra8_spi.c`                          |
| REQ-DRV-070      | ra8_sram          | SRAM ECC enable + scrub.                                                                                                              | `src/ra8_sram.c`                  | `test_ra8_sram.c`                         |
| REQ-DRV-071      | ra8_ssie          | Serial Sound Interface init.                                                                                                          | `src/ra8_ssie.c`                  | `test_ra8_ssie.c`                         |
| REQ-DRV-072      | ra8_touch         | Capacitive-touch (CTSU) channel scan.                                                                                                  | `src/ra8_touch.c`                 | `test_ra8_touch.c`                        |
| REQ-DRV-073      | ra8_tsn           | Time-Sensitive Networking factory-cal read + setup.                                                                                    | `src/ra8_tsn.c`                   | `test_ra8_tsn.c`                          |
| REQ-DRV-074      | ra8_ulpt          | Ultra-low-power timer init.                                                                                                              | `src/ra8_ulpt.c`                  | `test_ra8_ulpt.c`                         |
| REQ-DRV-075      | ra8_usb           | Top-level USB aggregation (selects host vs device, FS vs HS).                                                                            | `src/ra8_usb.c`                   | `test_ra8_usb.c`                          |
| REQ-DRV-076      | ra8_usb_cdc       | USB device CDC-ACM class.                                                                                                                  | `src/ra8_usb_cdc.c`               | `test_ra8_usb_cdc.c`                      |
| REQ-DRV-077      | ra8_usb_composite | USB composite-device descriptor builder.                                                                                                    | `src/ra8_usb_composite.c`         | `test_ra8_usb_composite.c`                |
| REQ-DRV-078      | ra8_usb_haud      | USB host audio class.                                                                                                                       | `src/ra8_usb_haud.c`              | `test_ra8_usb_haud.c`                     |
| REQ-DRV-079      | ra8_usb_hcdc      | USB host CDC class.                                                                                                                         | `src/ra8_usb_hcdc.c`              | `test_ra8_usb_hcdc.c`                     |
| REQ-DRV-080      | ra8_usb_hcdc_ecm  | USB host CDC-ECM (Ethernet) class.                                                                                                          | `src/ra8_usb_hcdc_ecm.c`          | `test_ra8_usb_hcdc_ecm.c`                 |
| REQ-DRV-081      | ra8_usb_hhid      | USB host HID class.                                                                                                                          | `src/ra8_usb_hhid.c`              | `test_ra8_usb_hhid.c`                     |
| REQ-DRV-082      | ra8_usb_hhub      | USB host hub class.                                                                                                                          | `src/ra8_usb_hhub.c`              | `test_ra8_usb_hhub.c`                     |
| REQ-DRV-083      | ra8_usb_hmsc      | USB host MSC class.                                                                                                                          | `src/ra8_usb_hmsc.c`              | `test_ra8_usb_hmsc.c`                     |
| REQ-DRV-084      | ra8_usb_paud      | USB device audio class.                                                                                                                       | `src/ra8_usb_paud.c`              | `test_ra8_usb_paud.c`                     |
| REQ-DRV-085      | ra8_usb_phid      | USB device HID class.                                                                                                                          | `src/ra8_usb_phid.c`              | `test_ra8_usb_phid.c`                     |
| REQ-DRV-086      | ra8_usb_pmsc      | USB device MSC class.                                                                                                                          | `src/ra8_usb_pmsc.c`              | `test_ra8_usb_pmsc.c`                     |
| REQ-DRV-087      | ra8_usb_pprn      | USB device printer class.                                                                                                                       | `src/ra8_usb_pprn.c`              | `test_ra8_usb_pprn.c`                     |
| REQ-DRV-088      | ra8_usb_pvnd      | USB device vendor class.                                                                                                                         | `src/ra8_usb_pvnd.c`              | `test_ra8_usb_pvnd.c`                     |
| REQ-DRV-089      | ra8_vin           | Video Input (parallel-camera) controller init.                                                                                                    | `src/ra8_vin.c`                   | `test_ra8_vin_capture.c`, `test_ra8_vin_config.c`, `test_ra8_vin_mcdc.c` |
| REQ-DRV-090      | ra8_vreg          | Internal voltage-regulator setup.                                                                                                                  | `src/ra8_vreg.c`                  | `test_ra8_vreg.c`                         |
| REQ-DRV-091      | ra8_wdt           | Watchdog (WDT0/WDT1) enable + refresh.                                                                                                              | `src/ra8_wdt.c`                   | `test_ra8_wdt.c`                          |
| REQ-DRV-092      | ra8_xspi          | xSPI / Octo-SPI controller init + memory-mapped read configuration.                                                                                  | `src/ra8_xspi.c`                  | `test_ra8_xspi.c`                         |
| REQ-DRV-093      | ra8_timer         | Generic timer-driver shim used by examples.                                                                                                           | `src/timer.c`                    | `test_ra8_timer.c`                        |

### 4.4 Ring 3 -- HAL aggregations and PALs (REQ-HAL-XXX)

| ID               | Requirement                                                                                                                          | Source                                          | Test                                          |
|------------------|--------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------|-----------------------------------------------|
| REQ-HAL-001      | A graphics-text rendering layer SHALL provide ASCII string draw primitives over a framebuffer.                                       | `libs/ra8_gfx/src/ra8_gfx_text.c`, `ra8_gfx_font_8x16.c` | `tests/test_ra8_gfx.c`, `tests/test_ra8_gfx_text.c` |
| REQ-HAL-002      | A FAT-filesystem facade SHALL wrap FileX with project error semantics (`ra8_err_t` translation).                                       | `libs/ra8_fs/src/ra8_fs_fat.c`                    | `tests/test_ra8_fs.c`, `tests/test_ra8_fs_fat.c` |
| REQ-HAL-003      | An MPU configuration helper SHALL build region tables for the bus MMPU and Cortex-M85 core MPU.                                       | `libs/ra8_mpu/src/ra8_mpu.c`                      | `tests/test_ra8_mpu.c`                         |
| REQ-HAL-004      | A watchdog supervisor SHALL refresh IWDT/WDT from a single bottleneck monitored against task heartbeats.                               | `libs/ra8_wdt_supervisor/src/ra8_wdt_supervisor.c`| `tests/test_ra8_wdt_supervisor.c`              |
| REQ-HAL-005      | A power-profile module SHALL select between "Run", "Sleep", "Standby", "Deep Standby" with explicit transitions through `ra8_lpm`.       | `libs/ra8_power_profile/src/ra8_power_profile.c` | `tests/test_ra8_power_profile.c`               |
| REQ-HAL-006      | A network PAL SHALL hide the underlying transport (Ethernet, USB-CDC-ECM, modem) behind a uniform packet I/O interface.                  | `libs/ra8_net_pal/src/ra8_net_pal.c`              | `tests/test_ra8_net_pal.c`                     |
| REQ-HAL-007      | A USB PAL SHALL provide a unified host/device descriptor + endpoint API consumable by either USBX or the in-tree USB drivers.           | `libs/ra8_usb_pal/src/ra8_usb_pal.c`              | `tests/test_ra8_usb_pal.c`                     |
| REQ-HAL-008      | A first-party network stack SHALL implement ARP/IPv4/ICMP/UDP/TCP for the loopback + one Ethernet path.                                  | `libs/ra8_net/src/ra8_net_arp.c`, `ra8_net_ipv4.c`, `ra8_net_icmp.c`, `ra8_net_udp.c`, `ra8_net_tcp.c` | `tests/test_ra8_net.c`, `test_ra8_net_arp.c`, `test_ra8_net_ipv4.c`, `test_ra8_net_udp.c`, `test_ra8_net_tcp.c` |
| REQ-HAL-009      | A TLS facade SHALL wrap Mbed TLS with project error semantics and a fixed cipher suite.                                                  | `libs/ra8_tls/src/ra8_tls.c`                      | `tests/test_ra8_tls.c`                         |
| REQ-HAL-010      | A PSA-Crypto integration SHALL expose the canonical PSA APIs through the project's logging/error pipeline.                                | `libs/ra8_psa_crypto/src/ra8_psa_crypto.c`        | `tests/test_ra8_psa_crypto_api.c` + siblings   |
| REQ-HAL-011      | An OTA orchestrator SHALL coordinate fetch, signature check, stage, and commit-to-MRAM through the secure veneer.                          | `libs/ra8_ota/src/ra8_ota.c`                      | `tests/test_ra8_ota.c`                         |
| REQ-HAL-012      | A BLE host stack SHALL provide ATT, GATT (server + client), L2CAP, security and mesh surfaces.                                              | Apache NimBLE (SOUP, `libs/third_party/nimble/`) consumed directly via `port/nimble/`; HCI transport seam `libs/ra8_hal/src/ra8_ble.c` | `tests/test_ra8_ble.c` (HCI seam); NimBLE host is SOUP (see `docs/SOUP/nimble.md`); end-to-end HW-blocked: ESP32-C6 companion |
| REQ-HAL-013      | A modem-AT module SHALL provide URC parsing + command-response sequencing over a UART back-end.                                              | `libs/ra8_modem_at/src/ra8_modem_at.c`            | `tests/test_ra8_modem_at.c`                    |
| REQ-HAL-014      | An EPUB content reader SHALL parse OPF + spine and return chapter text.                                                                       | `libs/ra8_epub/src/ra8_epub_open.c`, `ra8_epub_chapter.c`, `ra8_epub_xml_shim.cpp` | `tests/test_ra8_epub.c`, `test_ra8_epub_open.c`, `test_ra8_epub_chapter.c` |
| REQ-HAL-015      | A reflow renderer SHALL parse simple XHTML and produce a glyph layout for the GLCDC framebuffer.                                              | `libs/ra8_reflow/src/ra8_reflow_parse.c`, `ra8_reflow_layout.c`, `ra8_reflow_render.c`, `ra8_reflow_xml_shim.cpp` | `tests/test_ra8_reflow*.c` |
| REQ-HAL-016      | A touch calibration helper SHALL convert raw resistive-touch ADC samples to display coordinates via a 3-point affine transform.                 | `libs/ra8_touch_cal/src/ra8_touch_cal.c`          | `tests/test_ra8_touch_cal.c`                   |

### 4.5 Ring 4 -- board support (REQ-BSP-XXX)

| ID               | Requirement                                                                                                                          | Source                                          | Test                                          |
|------------------|--------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------|-----------------------------------------------|
| REQ-BSP-001      | The EK-RA8D2 board-init function SHALL configure board-only GPIO (LEDs, buttons, Pmod aliases) per the EK-RA8D2 v1 schematic.         | `libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2.c`| `tests/test_ra8_board_ek_ra8d2.c`              |
| REQ-BSP-002      | Board init SHALL bring up the EK-RA8D2 64 MiB SDRAM through `ra8_sdramc` and report the populated size.                                  | `libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2.c`| `tests/test_ra8_sdramc.c`                      |
| REQ-BSP-003      | Board init SHALL bring up the EK-RA8D2 1024x600 parallel TFT panel via `ra8_glcdc` after SDRAM is live.                                   | `libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2.c`| `tests/test_ra8_glcdc.c`, `tests/test_app_lcd_demo.c` |
| REQ-BSP-004      | Board init SHALL leave the J11 USB-FS, J12 USB-HS, J7 Ethernet, and J10 J-Link OB VCOM connectors in their power-on default state.       | `libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2.c`| TBD (covered indirectly by per-app integration) |

### 4.6 Ring 4/5 -- TrustZone NSC veneers and secure-side substrate (REQ-PORT-XXX)

| ID               | Requirement                                                                                                                          | Source                                          | Test                                          |
|------------------|--------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------|-----------------------------------------------|
| REQ-PORT-001     | All NSC veneers SHALL live under `libs/ra8_nsc/` and SHALL carry `__attribute__((cmse_nonsecure_entry))`.                                | `libs/ra8_nsc/src/*.c`                           | `tests/test_ra8_nsc.c`                         |
| REQ-PORT-002     | A communications NSC veneer SHALL expose UART/SPI/I2C calls to the NS world without leaking secure handles.                             | `libs/ra8_nsc/src/ra8_nsc_comms.c`                | `tests/test_ra8_nsc_comms.c`                   |
| REQ-PORT-003     | An I/O NSC veneer SHALL expose GPIO drive-level + read calls to the NS world with whitelisted pins only.                                  | `libs/ra8_nsc/src/ra8_nsc_io.c`                   | `tests/test_ra8_nsc_io.c`                      |
| REQ-PORT-004     | An xSPI NSC veneer SHALL expose memory-mapped-read configuration to NS without exposing erase/program.                                   | `libs/ra8_nsc/src/ra8_nsc_xspi.c`                 | `tests/test_ra8_nsc_xspi.c`                    |
| REQ-PORT-005     | An OTA NSC veneer SHALL accept a staged image hash from NS and commit it to the active MRAM bank.                                          | `libs/ra8_nsc/src/ra8_nsc_ota.c`                  | `tests/test_ra8_nsc_ota.c`                     |
| REQ-PORT-006     | An Ethernet NSC veneer SHALL marshal frame buffers from NS into a secure-side DMA descriptor pool.                                          | `libs/ra8_nsc/src/ra8_nsc_eth.c`                  | `tests/test_ra8_nsc_eth.c`                     |
| REQ-PORT-007     | A key-vault NSC veneer SHALL expose a SHA-256-XOR challenge-response API to NS without revealing key material.                              | `libs/ra8_nsc/src/ra8_nsc_key_vault.c`            | `tests/test_ra8_key_vault.c`                   |
| REQ-PORT-008     | A log NSC veneer SHALL forward NS log messages into the secure-side `ra8_log` sink.                                                            | `libs/ra8_nsc/src/ra8_nsc_log.c`                  | TBD                                            |
| REQ-PORT-009     | A peripheral-init NSC veneer SHALL expose a one-shot secure-side init for shared peripherals before NS bring-up.                              | `libs/ra8_nsc/src/ra8_nsc_periph_init.c`          | TBD                                            |
| REQ-PORT-010     | The secure key vault SHALL hold all 256-bit symmetric keys in a static array unreachable from NS after the SAU partition is enabled.            | `src/secure_app/key_vault.c`                    | `tests/test_ra8_key_vault.c`                   |
| REQ-PORT-011     | A key-import secure-app SHALL accept wrapped key blobs and install them into the key vault under a documented enum-typed key-class.              | `src/secure_app/key_import.c`                   | `tests/test_secure_app_key_import.c`          |
| REQ-PORT-012     | An OTA-commit secure-app SHALL verify the staged image hash and atomically swap the active MRAM bank.                                              | `src/secure_app/ota_commit.c`                   | `tests/test_secure_app_ota_commit.c`          |
| REQ-PORT-013     | A secure TRNG path SHALL provide entropy bytes to PSA-Crypto on the secure side.                                                                    | `src/secure_app/secure_trng.c`                  | `tests/test_secure_app_secure_trng.c`         |

### 4.7 Ring 6 -- applications (REQ-APP-XXX)

One requirement per app under `examples/ek_ra8d2/`. Each app's
contractual behaviour is "binary boots, performs the demo loop, and
either prints PASS/FAIL on UART or reaches its documented steady
state". The smoke harness under `make smoke` (referenced in
[`../HARDWARE_BRINGUP.md`](../HARDWARE_BRINGUP.md)) is the on-target
verification for these.

| ID               | App                              | Source                                                  | Test                                       |
|------------------|----------------------------------|---------------------------------------------------------|--------------------------------------------|
| REQ-APP-001      | blink                             | `examples/ek_ra8d2/blink/main.c`                        | (HW smoke only, no host test)              |
| REQ-APP-002      | blink_hal                         | `examples/ek_ra8d2/blink_hal/main.c`                    | `tests/test_app_blink_hal.c`               |
| REQ-APP-003      | clock_check                       | `examples/ek_ra8d2/clock_check/main.c`                  | `tests/test_app_clock_check.c`             |
| REQ-APP-004      | ereader                           | `examples/ek_ra8d2/ereader/main.c`                      | `tests/test_app_ereader.c`                 |
| REQ-APP-005      | ethernet_tcp_echo                 | `examples/ek_ra8d2/ethernet_tcp_echo/main.c`            | `tests/test_app_ethernet_tcp_echo.c`       |
| REQ-APP-006      | lcd_demo                          | `examples/ek_ra8d2/lcd_demo/main.c`                     | `tests/test_app_lcd_demo.c`                |
| REQ-APP-007      | ra8_bootloader                     | `examples/ek_ra8d2/ra8_bootloader/main.c`                | `tests/test_app_ra8_bootloader.c`           |
| REQ-APP-008      | threadx_blink                     | `examples/ek_ra8d2/threadx_blink/main.c`                | `tests/test_app_threadx_blink.c`           |
| REQ-APP-009      | threadx_canfd_demo                | `examples/ek_ra8d2/threadx_canfd_demo/main.c`           | `tests/test_app_threadx_canfd_demo.c`      |
| REQ-APP-010      | threadx_filex_demo                | `examples/ek_ra8d2/threadx_filex_demo/main.c`           | `tests/test_app_threadx_filex_demo.c`      |
| REQ-APP-011      | threadx_filex_levelx_demo         | `examples/ek_ra8d2/threadx_filex_levelx_demo/main.c`    | `tests/test_app_threadx_filex_levelx_demo.c` |
| REQ-APP-013      | threadx_ipc_demo                  | `examples/ek_ra8d2/threadx_ipc_demo/main.c`             | `tests/test_app_threadx_ipc_demo.c`        |
| REQ-APP-014      | threadx_levelx_demo               | `examples/ek_ra8d2/threadx_levelx_demo/main.c`          | `tests/test_app_threadx_levelx_demo.c`     |
| REQ-APP-015      | threadx_lwip_tcp_echo             | `examples/ek_ra8d2/threadx_lwip_tcp_echo/main.c`        | `tests/test_app_threadx_lwip_tcp_echo.c`   |
| REQ-APP-016      | threadx_mpu_partition_demo        | `examples/ek_ra8d2/threadx_mpu_partition_demo/main.c`   | `tests/test_app_threadx_mpu_partition_demo.c` |
| REQ-APP-017      | threadx_netx_tcp_echo             | `examples/ek_ra8d2/threadx_netx_tcp_echo/main.c`        | `tests/test_app_threadx_netx_tcp_echo.c`   |
| REQ-APP-018      | threadx_ota_demo                  | `examples/ek_ra8d2/threadx_ota_demo/main.c`             | `tests/test_app_threadx_ota_demo.c`        |
| REQ-APP-019      | threadx_usbx_cdc_demo             | `examples/ek_ra8d2/threadx_usbx_cdc_demo/main.c`        | `tests/test_app_threadx_usbx_cdc_demo.c`   |
| REQ-APP-020      | uart_hello                        | `examples/ek_ra8d2/uart_hello/main.c`                   | `tests/test_app_uart_hello.c`              |
| REQ-APP-021      | usb_cdc_echo                      | `examples/ek_ra8d2/usb_cdc_echo/main.c`                 | `tests/test_app_usb_cdc_echo.c`            |
| REQ-APP-022      | usb_hid_device                    | `examples/ek_ra8d2/usb_hid_device/main.c`               | `tests/test_app_usb_hid_device.c`          |
| REQ-APP-024      | usb_host_keyboard                 | `examples/ek_ra8d2/usb_host_keyboard/main.c`            | `tests/test_app_usb_host_keyboard.c`       |
| REQ-APP-025      | usb_host_msc_browse               | `examples/ek_ra8d2/usb_host_msc_browse/main.c`          | `tests/test_app_usb_host_msc_browse.c`     |
| REQ-APP-026      | usb_msc_device                    | `examples/ek_ra8d2/usb_msc_device/main.c`               | `tests/test_app_usb_msc_device.c`          |

---

## 5. Safety requirements (REQ-SAFE-XXX)

These derive from the project-wide rules in
[`../../CLAUDE.md`](../../CLAUDE.md) (NASA Power-of-10 mapping) and
the deviation register at
[`./MISRA_DEVIATIONS.md`](./MISRA_DEVIATIONS.md). They apply to every
first-party translation unit unless an explicit per-file exemption is
recorded.

### 5.1 NASA Power of 10 (REQ-SAFE-001..010)

| ID               | Rule                                                                                                | Enforcement / source                                                                                          | Test                                                          |
|------------------|-----------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------|
| REQ-SAFE-001     | P10 Rule 1 -- no `goto`, `setjmp`/`longjmp`, recursion.                                              | clang-tidy + manual review (`CLAUDE.md` "NASA Power of 10")                                                    | TBD (lint-only)                                               |
| REQ-SAFE-002     | P10 Rule 2 -- all loops carry a statically provable upper bound.                                     | clang-tidy LineThreshold = 60; manual review                                                                    | TBD                                                           |
| REQ-SAFE-003     | P10 Rule 3 -- zero dynamic allocation after init.                                                    | `libs/ra8_core/src/ra8_sbrk_trap.c` + `scripts/checks/check_no_dynamic_alloc.py`                                  | (compile-time gate)                                           |
| REQ-SAFE-004     | P10 Rule 4 -- functions <= ~60 source lines.                                                         | `.clang-tidy` LineThreshold = 60                                                                               | (lint gate)                                                   |
| REQ-SAFE-005     | P10 Rule 5 -- minimum 2 validation checks per function.                                                | `RA8_CHECK_*` macros in `libs/ra8_core/inc/ra8_check.h`                                                            | per-driver test files exercise the precondition path           |
| REQ-SAFE-006     | P10 Rule 6 -- variables declared at the smallest possible scope.                                       | clang-tidy + manual review                                                                                       | TBD                                                           |
| REQ-SAFE-007     | P10 Rule 7 -- all return values checked or explicitly cast `(void)`.                                  | `RA8_RETURN_ON_ERROR` macro idiom (REQ-CORE-003)                                                                  | `tests/test_ra8_err.c`                                         |
| REQ-SAFE-008     | P10 Rule 8 -- macros only for duplicated code, conditional compilation, or build flags.                | manual review against `CLAUDE.md` "Constants and Macros"                                                          | TBD                                                           |
| REQ-SAFE-009     | P10 Rule 9 -- function pointers permitted only as DIP injection seams (intentional deviation).         | recorded in `CLAUDE.md` "Rule 9"                                                                                   | (deviation; documented)                                       |
| REQ-SAFE-010     | P10 Rule 10 -- `-Wall -Wextra -Werror`; build fails on any warning.                                    | `cmake/toolchain-ra8d2.cmake`, CI matrix                                                                            | (CI gate)                                                     |

### 5.2 MISRA-C 2012 deviations (REQ-SAFE-011..015)

The deviation register is
[`./MISRA_DEVIATIONS.md`](./MISRA_DEVIATIONS.md). Each row is mirrored
here as a software requirement so the SVP can pick it up.

| ID               | Deviation                                                                                          | Disposition                                                | Source / test                              |
|------------------|----------------------------------------------------------------------------------------------------|------------------------------------------------------------|--------------------------------------------|
| REQ-SAFE-011     | D-001 Rule 15.5 single-exit -- accepted under NASA P10 Rule 7 + `RA8_RETURN_ON_ERROR`.              | Project deviation (formal)                                 | `MISRA_DEVIATIONS.md` D-001                |
| REQ-SAFE-012     | D-002 Rule 17.3 implicit declaration -- tooling false positive; compiler is the authoritative gate. | Tooling gap                                                | `MISRA_DEVIATIONS.md` D-002                |
| REQ-SAFE-013     | D-003 Rule 9.2 braced-aggregate-init -- tooling gap (C23 `= {}` permitted by project standard).      | Tooling gap                                                | `MISRA_DEVIATIONS.md` D-003                |
| REQ-SAFE-014     | D-004 Rule 12.1 explicit-precedence -- partial deviation; bracket-where-ambiguous remains required.  | Partial deviation                                          | `MISRA_DEVIATIONS.md` D-004                |
| REQ-SAFE-015     | D-005 Rule 8.4 declaration-before-definition -- tooling gap; compiler `-Wmissing-prototypes` covers. | Tooling gap                                                | `MISRA_DEVIATIONS.md` D-005                |

### 5.3 IEC 61508 SIL 3 evidence requirements (REQ-SAFE-016..020)

| ID               | Requirement                                                                                          | Source                                                                                                          | Test / artefact                                       |
|------------------|------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------|-------------------------------------------------------|
| REQ-SAFE-016 | First-party reachable MC/DC SHALL reach 100 % per IEC 61508-3 Annex C / DO-178C 6.4.4.2; deactivated conditions per DO-178C 6.4.4.3 are exempted. **Met (100.00 % reachable, 92.29 % absolute, 2026-05-03).** | `make mcdc` driver `scripts/report/mcdc_report.sh`; deactivations in `docs/MCDC_DEACTIVATIONS.md` | `build/mcdc-report/`, gate at `.github/mcdc-baseline.txt` |
| REQ-SAFE-017     | First-party branch + statement coverage SHALL reach 90/90 (IEC 61508 Annex C minimum).                | `scripts/checks/coverage.sh --gate`                                                                                     | CI job `firmware.yml::coverage`                       |
| REQ-SAFE-018     | The architecture SHALL provide ECC-protected SRAM (IEC 61508-2 hardware integrity contribution).      | `libs/ra8_hal/src/ra8_sram.c` (ECC enable)                                                                         | `tests/test_ra8_sram.c`                                |
| REQ-SAFE-019     | An IWDT SHALL be enabled in production builds and refreshed by `ra8_wdt_supervisor`.                   | `libs/ra8_hal/src/ra8_iwdt.c`, `libs/ra8_wdt_supervisor/src/ra8_wdt_supervisor.c`                                   | `tests/test_ra8_iwdt.c`, `tests/test_ra8_wdt_supervisor.c` |
| REQ-SAFE-020     | A documented SOUP register SHALL list every third-party component with re-review cadence <= 12 months. | `docs/SOUP/`                                                                                                     | `docs/SOUP/README.md` index                            |

---

## 6. Performance requirements (REQ-PERF-XXX)

These are the headline performance budgets that downstream
verification must measure. Numbers without measurements today are
flagged `TBD-MEASURE`. The closure path is the developer-laptop
HIL workflow in `docs/HIL_DEVELOPER_WORKFLOW.md` (a self-hosted CI
runner is out of scope per `docs/CERTIFICATION_SCOPE.md`).

| ID               | Requirement                                                                                          | Source                                                                                                          | Test / artefact                                       |
|------------------|------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------|-------------------------------------------------------|
| REQ-PERF-001     | Cold-boot to first `main` instruction SHALL complete in under 100 ms at 1 GHz core clock.             | `examples/ek_ra8d2/blink/system_init.c` (CGC bring-up dominates)                                                 | TBD-MEASURE (HW smoke)                                |
| REQ-PERF-002     | NVIC IRQ latency for an enabled prio-0 source SHALL be <= 250 ns (per Cortex-M85 12-cycle baseline).  | `libs/ra8_hal/src/ra8_isr.c`                                                                                       | TBD-MEASURE (logic-analyser hook)                      |
| REQ-PERF-003     | SCI UART throughput at 115200 8N1 SHALL sustain >= 11 KiB/s without DMA.                              | `libs/ra8_hal/src/ra8_sci.c`, `libs/ra8_hal/src/uart.c`                                                              | `tests/test_ra8_sci.c`, `tests/test_ra8_uart.c` (logic-level only) |
| REQ-PERF-004     | xSPI memory-mapped read SHALL achieve >= 80 MiB/s in 8-line DDR mode at 100 MHz xSPI clock.            | `libs/ra8_hal/src/ra8_xspi.c`                                                                                      | TBD-MEASURE                                            |
| REQ-PERF-005     | Ethernet TX throughput on 100BASE-TX SHALL exceed 80 Mbit/s for 1500-byte frames.                      | `libs/ra8_hal/src/ra8_etha.c`                                                                                       | TBD-MEASURE (`examples/ek_ra8d2/ethernet_tcp_echo/`)   |
| REQ-PERF-006     | GLCDC SHALL refresh the EK-RA8D2 1024x600 panel at >= 60 Hz with two layers.                            | `libs/ra8_hal/src/ra8_glcdc.c`                                                                                       | `examples/ek_ra8d2/lcd_demo/`                          |
| REQ-PERF-007     | OTA commit SHALL complete in <= 2 s for a 256 KiB image excluding network transfer.                    | `libs/ra8_ota/src/ra8_ota.c`, `src/secure_app/ota_commit.c`                                                          | `tests/test_ra8_ota.c`, `tests/test_secure_app_ota_commit.c` |
| REQ-PERF-008     | Static stack budget per task SHALL not exceed values declared in `libs/ra8_core/inc/ra8_stack_budget.h`. | `-Wstack-usage`, `scripts/checks/stack_usage_check.py`                                                              | (build-time gate); `docs/STACK_USAGE.md`               |

---

## 7. External interfaces (REQ-EXT-XXX)

The board-side connector inventory is taken from the EK-RA8D2 v1
User's Manual (R20UT5523EG0101) committed under
[`../reference/`](../reference/).

| ID               | Connector / interface                                            | Driver / source                                                       | Test                                          |
|------------------|------------------------------------------------------------------|-----------------------------------------------------------------------|-----------------------------------------------|
| REQ-EXT-001      | J11 USB-FS device port (USB 2.0 full-speed)                       | `libs/ra8_hal/src/ra8_usb.c`, `ra8_usb_cdc.c`, `ra8_usb_phid.c`, `ra8_usb_pmsc.c` | `tests/test_app_usb_cdc_echo.c`, etc.        |
| REQ-EXT-002      | J12 USB-HS host/device port (USB 2.0 high-speed)                  | `libs/ra8_hal/src/ra8_usb.c`, `ra8_usb_h*.c`                              | `tests/test_app_usb_host_*.c`                  |
| REQ-EXT-003      | J7 RJ45 Ethernet (100BASE-TX)                                      | `libs/ra8_hal/src/ra8_eth*.c`, `libs/ra8_net/src/*`                       | `tests/test_app_ethernet_tcp_echo.c`           |
| REQ-EXT-004      | J10 J-Link OB VCOM (UART debug console)                            | `libs/ra8_hal/src/ra8_sci.c`, `libs/ra8_hal/src/uart.c`                   | `tests/test_app_uart_hello.c`                  |
| REQ-EXT-005      | Pmod Type 6A header (SPI + GPIO)                                    | `libs/ra8_hal/src/ra8_spi_b.c`, `gpio.c`                                  | `tests/test_ra8_spi.c`, `tests/test_ra8_gpio.c`  |
| REQ-EXT-006      | Pmod Type 6B header (I2C + GPIO)                                    | `libs/ra8_hal/src/ra8_iic_b.c`, `gpio.c`                                   | `tests/test_ra8_iic_b.c`, `tests/test_ra8_gpio.c` |
| REQ-EXT-007      | Arduino Uno R3 header (digital + analog + I2C + SPI + UART)         | `libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2.c` (pin map)               | TBD                                            |
| REQ-EXT-008      | 7.0-inch parallel TFT (1024x600)                                     | `libs/ra8_hal/src/ra8_glcdc.c`                                              | `tests/test_app_lcd_demo.c`                    |
| REQ-EXT-009      | OV5640 5 MP camera                                                    | `libs/ra8_hal/src/ra8_ceu.c`, `ra8_mipi_csi.c`, `ra8_vin.c`                   | `tests/test_ra8_ceu_capture.c`, `tests/test_ra8_mipi_csi_init.c`, `tests/test_ra8_vin_capture.c` (+ their split siblings) |
| REQ-EXT-010      | On-board 64 MiB Octo-SPI NOR flash                                    | `libs/ra8_hal/src/ra8_xspi.c`                                                | `tests/test_ra8_xspi.c`, `tests/test_lx_nor_driver_ra8_xspi.c` |
| REQ-EXT-011      | On-board 64 MiB SDRAM                                                 | `libs/ra8_hal/src/ra8_sdramc.c`                                              | `tests/test_ra8_sdramc.c`                       |
| REQ-EXT-012      | EK-RA8D2 user-button + user-LED set                                    | `libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2.c`                            | `tests/test_ra8_board_ek_ra8d2.c`                |

---

## 8. Traceability matrix

The full forward-trace from REQ-XXX to source + test is the per-row
"Source" + "Test" columns in Section 4 through Section 7. The
backward-trace (file -> requirements) is generated on demand by
`scripts/checks/cite_check.py` walking the `@cite` doxygen tags. Both
directions are required by IEC 61508-3 Clause 7.4.4.6 and DO-178C
Section 6.5 (Traceability Data).

### 8.1 Coverage summary

| Category                | Total | Traced to test | Untraced (TBD) | Blocked-vendor |
|-------------------------|-------|----------------|-----------------|----------------|
| REQ-CHIP                | 7     | 7              | 0               | 0              |
| REQ-CORE                | 14    | 13             | 1               | 0              |
| REQ-DRV                 | 93    | 90             | 0               | 3 (BLE patch + 2 RSIP) |
| REQ-HAL                 | 16    | 15             | 0               | 1 (BLE host end-to-end) |
| REQ-BSP                 | 4     | 3              | 1               | 0              |
| REQ-PORT                | 13    | 11             | 2               | 0              |
| REQ-APP                 | 26    | 25             | 1 (blink)       | 0              |
| REQ-SAFE                | 20    | 14             | 6 (lint-only)   | 0              |
| REQ-PERF                | 8     | 3              | 5 (TBD-MEASURE) | 0              |
| REQ-EXT                 | 12    | 10             | 2               | 0              |
| **Total**               | **213**| **191**       | **18**          | **4**          |

### 8.2 Untraced and blocked items

- **REQ-CORE-013** (`ra8_stack_budget.h`) -- no host test; relies on
  `-Wstack-usage` build-time gate plus `docs/STACK_USAGE.md`.
- **REQ-BSP-004** (board-init connector defaults) -- covered indirectly
  by app-level smoke; no isolated host test today.
- **REQ-PORT-008** (NSC log forwarding) -- no test yet.
- **REQ-PORT-009** (NSC peripheral-init) -- no test yet.
- **REQ-APP-001** (blink) -- HW smoke only; no host test (the `blink`
  app is a register-poke smoke, not HAL-driven).
- **REQ-SAFE-001/002/004/006/008** -- enforced by lint, not by host
  test.
- **REQ-PERF-001/002/004/005** -- require HW-in-the-loop measurement;
  closure path is the developer-laptop pre-push HIL workflow in
  `docs/HIL_DEVELOPER_WORKFLOW.md`.
- **REQ-EXT-007** (Arduino header) -- no targeted test today.
- **REQ-DRV-061/062** (RSIP key injection / protected on real
  hardware) -- `BLOCKED-VENDOR`. Tracked in
  [`../VENDOR_BLOBS.md`](../VENDOR_BLOBS.md).
- **REQ-HAL-012** (BLE host stack end-to-end) -- HW-blocked: on-wire
  BLE needs the ESP32-C6 companion controller across the HCI transport
  seam; host-side ATT/GATT/L2CAP/security/mesh logic is tested.

The coverage gap is the input to [`./SVP.md`](./SVP.md) Section 1
(verification objective tables). Phase 5 (per-app integration test
layer) is now at 25/26; HIL is the developer-laptop pre-push workflow
per `docs/HIL_DEVELOPER_WORKFLOW.md`.

---

## 9. References

- [`../../CLAUDE.md`](../../CLAUDE.md) -- coding rules and NASA P10 mapping.
- [`../STYLE_GUIDE.md`](../STYLE_GUIDE.md) -- human-facing style guide.
- [`../RING_AND_WORLD.md`](../RING_AND_WORLD.md) -- architectural-ring + TrustZone-world tagging.
- [`../MEMORY_MAP.md`](../MEMORY_MAP.md) -- memory map and partition assignments.
- [`../STACK_USAGE.md`](../STACK_USAGE.md) -- per-task stack budgets.
- [`../MCDC.md`](../MCDC.md), [`../MCDC_GAPS.md`](../MCDC_GAPS.md) -- structural-coverage program.
- [`../MISRA.md`](../MISRA.md), [`./MISRA_DEVIATIONS.md`](./MISRA_DEVIATIONS.md) -- language-subset conformance.
- [`../HARDWARE_BRINGUP.md`](../HARDWARE_BRINGUP.md) -- hardware-in-the-loop sweep.
- [`../SOUP/`](../SOUP/) -- pre-existing software register.
- [`../VENDOR_BLOBS.md`](../VENDOR_BLOBS.md) -- vendor-blob blocker register.
- [`./PSAC.md`](./PSAC.md) -- gateway artefact; references this SRS in Section 5.
- [`./SDD.md`](./SDD.md) -- design description that consumes this SRS.
- [`./SVP.md`](./SVP.md) -- verification plan that traces against this SRS in Table A-3.
- [`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md) -- 22-week schedule and gap analysis.
- IEC 61508-3:2010 Clause 7.2 (Software safety requirements).
- RTCA DO-178C:2011 Section 11.9 (Software Requirements Data).
- ISO 26262-6:2018 Clause 6 (Specification of software safety requirements).
