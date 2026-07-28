# RA8D2 vs RA8P1 -- difference analysis + multi-chip plan

> Reference for GitHub issue #220. This is the authoritative single-page
> account of how this tree targets **two** parts from one source: the Renesas
> **RA8D2** (`R7KA8D2KFLCAC`, HUM `R01UH1065EJ`) that the HAL was written for,
> and the **RA8P1** (`R7KA8P1KFLCAC`, HUM `R01UH1064EJ`). The mechanism lives in
> [`libs/ra8_core/inc/ra8_device.h`](../libs/ra8_core/inc/ra8_device.h); this doc
> explains the deltas and points at the code that implements each one.

## 1. The one-sentence model

**RA8P1 == "RA8D2 + an Arm Ethos-U55 micro-NPU"**, in the same pin-compatible
289-BGA package, with **byte-identical peripheral register bases** and a handful
of small option/capability deltas. Everything the HAL already knows about the
RA8D2 (clock tree, IOPORT, SCI/SPI/I2C, GLCDC, SDHI, USB, TrustZone) applies
unchanged; only the items in the table below differ.

## 2. The multi-chip switch

Selection is a single compile define, defaulted so nothing existing changes:

| Define | Set by | Meaning |
|--------|--------|---------|
| `RA8_DEVICE_RA8D2` | default (no flag) | RA8D2. Host unit-test + every current build. |
| `RA8_DEVICE_RA8P1` | `cmake/toolchain-ra8p1.cmake` | RA8P1 (adds the NPU + DP-FPU). |

`ra8_device.h` enforces "exactly one" (`#error` on both, defaults to RA8D2 on
neither) and derives:

- **Feature-presence macros** for `#if` guards: `RA8_HAS_NPU`, `RA8_HAS_NPUCLK`
  (RA8P1 only).
- **A typed-enum runtime mirror** `k_ra8_feat_*` (0/1) and `k_ra8_device_t`
  (`k_ra8_device_ra8d2 = 0x8D2`, `k_ra8_device_ra8p1 = 0x8F1`) for code that needs
  the selection at run time rather than in the preprocessor.

Register headers that carry a base-address delta guard it with
`#if defined(RA8_DEVICE_RA8P1)`; because the bases are byte-identical today, only
the NPU window is actually device-specific. Prefer the named `RA8_HAS_*` flag
over a raw `RA8_DEVICE_RA8P1` check in feature code so the intent ("this chip has
an NPU") survives the arrival of a future part.

`ra8_emulator` mirrors the switch at runtime: `ra8_emulator <elf> --device ra8p1`
maps the Ethos-U55 register window (see section 4); the default RA8D2 profile
leaves it unmapped.

## 3. Delta table

| Area | RA8D2 | RA8P1 | Gate | Status / code |
|------|-------|-------|------|---------------|
| Ethos-U55 NPU | absent | present @ `0x40140000` | `RA8_HAS_NPU` | ra8_npu driver + `ra8_npu_regs.h`; emulator-modelled (#222). **Done.** |
| NPUCLK domain | absent | present | `RA8_HAS_NPUCLK` | CGC NPUCLK; on-silicon wiring #229. |
| M85 FPU width | single (`fpv5-sp-d16`) | **double** (`fpv5-d16`) | toolchain | `cmake/toolchain-ra8p1.cmake` appends `-mfpu=fpv5-d16`; witnessed by `ra8_fpu_probe` (#225). **Done.** |
| ADC resolution | 12-bit default | **16-bit** (ADC16H) | `ADDOPCRC.ADPRC` | Same ADC16H block on both; `ra8_adc_resolution_t` carries 10/12/14/16-bit and `ra8_adc` programs the per-channel `ADDOPCRCn.ADPRC` data-format; emulator-modelled (#225). **Done.** |
| HUM | `R01UH1065EJ` | `R01UH1064EJ` | -- | cite the matching manual per device. |

Package, 1 MB code MRAM, 1.6 MB dual-core ECC SRAM, and every non-NPU
peripheral base are the same across the two parts.

### Not a delta: the OFS3 / WDT1 option register (#516)

This table previously listed OFS3 as RA8D2-only behind an `RA8_HAS_OFS3` flag,
on the strength of Renesas FSP's `BSP_FEATURE_BSP_HAS_OFS3 == 0` for `ra8p1`.
**That was wrong, and the flag is gone.** Both parts carry OFS3, OFS3_SEC and
OFS3_SEL at the same addresses with the same WDT1 bit fields:

| | Chapter / section | Printed page |
|---|---|---|
| RA8D2 `R01UH1065EJ0130` | 7.2.6 `OFS3, OFS3_SEC` / 7.2.7 `OFS3_SEL` | 287 / 289 |
| RA8P1 `R01UH1064EJ0130` | 7.2.6 `OFS3, OFS3_SEC` / 7.2.7 `OFS3_SEL` | 288 / 290 |

Both parts also have the WDT1 that OFS3 configures (RA8P1 datasheet
`R01DS0439EJ0130`: "Watchdog Timer (WDT) x 2", WDT1 at `0x4020_2600`). The FSP
flag contradicts Renesas' own manual, has no consumer anywhere in open FSP
source, and a RASC-generated RA8P1 project emits the OFS3 sections regardless.
`scripts/checks/check_linker_scripts.py` rule **LD008** now fails any linker
script that declares a partial option-setting family, so the gating cannot be
reintroduced silently. Do not re-add an OFS3 delta without a primary-source
citation showing the word absent.

## 4. NPU (the headline delta)

The Ethos-U55 is a memory-mapped command/queue engine at `0x40140000`. The
driver ([`ra8_npu.h`](../libs/ra8_hal/inc/ra8_npu.h)) is `submit -> run -> poll ->
read-output`; it is gated behind `RA8_HAS_NPU` so the TU is empty on the RA8D2.
The inference path is layered:

1. **Driver** `ra8_npu` -- programs QBASE/QSIZE (command stream) + BASEPn (tensor
   arena bases), kicks the job, waits for `STATUS.cmd_end`. **Done.**
2. **Adapter** `ra8_ethosu_shim` -- provides the Arm ethos-u-core-driver C API
   (`ethosu_reserve_driver` / `ethosu_invoke_v3` / `ethosu_release_driver`) on
   top of `ra8_npu`, so TFLite-micro's Ethos-U op can drive the NPU (#228).
   **Done** (host-tested).
3. **Runtime** TFLite-micro (vendored SOUP, dormant behind `RA8_USE_TFLITE_MICRO`)
   + the offline **Vela** `.tflite -> command-stream` compiler (#227). **Pending.**
4. **On-silicon** NPUCLK enable + IRQ wiring + a real inference (#229).
   **Hardware-blocked** (needs an RA8P1 part).

`ra8_emulator` models the window (`board_periph_npu.c`, `--device ra8p1`) using the
`ra8_npu_fake_cmd.h` SE55 command convention, so the whole driver + adapter path is
verifiable headless (`npu_smoke` runs to `verdict=PASS`). The fake executes the
SE55 convention, **not** real Vela command streams -- real-model inference is a
silicon (#229) step.

## 5. Bring-up + validation plan

- **Host (device-agnostic):** `ra8_ofs`, `ra8_fpu_probe`, `ra8_npu`,
  `ra8_ethosu_shim` unit tests build with `-DRA8_DEVICE_RA8P1` and pass on the
  Linux test host.
- **EIL (headless ra8_emulator):** RA8P1 apps under `examples/ra8p1_foundation/`
  run with `--device ra8p1`; `npu_smoke` is the end-to-end NPU witness. Note
  ra8_emulator requires **gcc-13+** (C23 typed enums).
- **On-silicon (#229):** blocked on obtaining an RA8P1 EK; needs NPUCLK bring-up,
  NPU IRQ wiring, and a Vela-compiled model. Tracked separately.

## 6. Open RA8P1 issues

`#220` (this analysis) - `#223` OFS/boot (done) - `#225` DP-FPU (done) + 16-bit
ADC (done; `fpv5-d16` silicon benchmark remains, part-blocked) - `#226`
`ra8_board_ra8p1` board layer + bring-up - `#227` Vela
integration - `#228` inference adapter (done) - `#229` on-silicon Ethos-U55 -
`#203` PCB / memory-hierarchy spike.
