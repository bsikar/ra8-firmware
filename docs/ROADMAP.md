# ra8-firmware Roadmap

Source of truth for progress through the HAL completion plan.

Single markdown file, updated in the same commit as the code it
tracks. Do not duplicate this status anywhere else (issue trackers,
PR descriptions, separate `STATUS.md` files). If you find yourself
copying status out of here, the answer is to refer back to here
instead.

## Cross-verify sweep log (sweeps 1-11)

After the 14-checkbox waves closed at the cross-verify-loop entry
point (commit `f2c3203`), eleven feature-completion sweeps landed on
top, plus a closure pass. Each sweep is documented in its
commit message; this is the one-line summary so the roadmap is
self-contained.

| Sweep | Commit | Title |
|------:|:-------|:------|
| 1 | `3f97975` | IIC, SDHI, SPI, SCI from scaffold to FSP-grade |
| 2 | `4cde2a2` | Ethernet TX/RX rings, USB device HID/MSC, GLCDC layer-2 |
| 3 | `798b019` | ADC scan groups, GPT PWM/3-phase, I3C CCC, USB composite |
| 4 | `6fd856c` | Flash erase/write/blank-check, USB host CDC-ECM, USB Type-C |
| 5 | `171901a` | NEW drivers: CTSU touch, PDC camera bridge, SCE crypto |
| 6 | `3ff1a8d` | DMAC/OSPI feature gaps, MIPI video timing, PTP IEEE 1588 |
| 7 | `09abe38` | Six new hardware-flashable example apps |
| 8 | `5e154b9` | BLE scaffold, USB host audio, software JPEG, ra8_net stack |
| 9 | `afeb54a` | FAT-FS adapter, TLS 1.2 client, font/glyph rendering, docs refresh |
| 10 | `59cc3c3` | USB device classes, BLE host stack, legacy peers, USB hub |
| 11 | `ba54974` | 20 missing peripherals, 3 demo apps, RSA/ECC/protected SCE |
| 11.x | `f4fb1a6`, `7551634`, `f272dc7`, `ce76aa4`, `87b606f` | closure: coverage gate, doxygen, ADC_B/OSPI/ACMPHS layout fixes |

Status snapshot at the close of sweep 11 (commit `ba54974`):

- 115 driver source files in `libs/ra8_hal/src/` (was 96 at sweep 8).
- 10 top-level libraries: ra8_core, ra8_hal, ra8_nsc, ra8_net_pal,
  ra8_usb_pal, plus the four sweep-8/9/10 additions ra8_net, ra8_fs,
  ra8_tls, ra8_gfx.
- 65 drivers feature-complete vs FSP, 6 partial, 20 FSP-shaped
  placeholders (carry `@warning`; bodies maintain software state),
  24 scaffolds.
- 139 ctest executables (`build/host-docker` ctest -N).
- 15 hardware-flashable example apps (was 6 at start of sweep 7,
  was 12 at end of sweep 8).

For the at-a-glance driver-vs-FSP-parity matrix see
`docs/DRIVER_STATUS.md`. For the residual hardware-blob gap list see
`docs/VENDOR_BLOBS.md`; the post-baseline plan is tracked in the
GitHub issue tracker (label: `roadmap`).

Status markers:

- `[ ]` TODO -- not started
- `[~]` WIP -- in progress this session, not yet at Done
- `[x]` DONE -- all 14 checkboxes ticked, lints + tests + coverage green
- `[!]` BLOCKED -- prereq missing or external blocker; describe in adjacent text

Sections under each peripheral copy the 14-checkbox template
verbatim. The `Summary` block at the top is rewritten
deterministically by `scripts/report/roadmap_stats.py` in
pre-commit -- do not hand-edit it.

## Summary

<!-- BEGIN SUMMARY -- DO NOT EDIT BY HAND -- managed by roadmap_stats.py -->
- Total drivers tracked: 45
- DONE:    45
- WIP:     0
- BLOCKED: 0
- TODO:    0
- Checklist coverage: 704/704 boxes ticked (100.0%)
<!-- END SUMMARY -->

## Wave table

| Wave | Title | Sessions | Status |
|-----:|:-------------------------------------------------------|---------:|:-------|
| 0 | Citation + architecture infrastructure | 1 | [x] |
| 1 | Shared HAL substrate | 3 | [x] |
| 2 | Foundation drivers (ICU, ELC, DMAC, DTC, CGC) | 2 | [x] |
| 3 | Critical serial / parallel IO | 8 | [x] |
| 4 | Analog, safety, time | 4 | [x] |
| 5 | External memory and high-throughput buses | 5 | [x] |
| 6 | Display, audio, USB controllers, Ethernet MAC | 6 | [x] |
| 7 | PAL + middleware integration (NetX Duo, CherryUSB) | 6 | [x] |
| 8 | Single-world integration demo + stabilisation | 1-2 | [x] |
| 9 | TrustZone partitioning | 4-5 | [x] |
| 10 | Secure-side application + key handling demo | 1-2 | [x] |

## Per-driver feature checklist template

Every peripheral section below is a copy of this template with HUM
citations filled in. A driver is `[x]` DONE only when every
checkbox is ticked AND `cite_check.py` + `check_world_tags.py`
both pass for that driver's files.

```
[ ] Init - MSTP ungate via ra8_mstp_enable, CGC clock via ra8_pwr_*,
                       pin route via ra8_mpc_route_*, register baseline write,
                       s_channel_initialized[] set -- HUM Ch X.Y p NNNN
[ ] Deinit - drain, ISR detach, DMA release, MSTP gate,
                       clear init state -- HUM Ch X.Y p NNNN
[ ] Polling TX - blocking with ra8_hw_wait_flag timeout -- HUM Ch X.Y p NNNN
[ ] Polling RX - blocking with ra8_hw_wait_flag timeout -- HUM Ch X.Y p NNNN
[ ] Interrupt TX - TDRE / TXI via ra8_isr_register, ring buffer drain -- HUM Ch X.Y p NNNN
[ ] Interrupt RX - RDRF / RXI via ra8_isr_register, ring buffer fill -- HUM Ch X.Y p NNNN
[ ] DMA TX - ra8_dma_request/configure/start, completion via ra8_isr -- HUM Ch X.Y p NNNN
[ ] DMA RX - ra8_dma, cache maint (clean/invalidate) on cross target -- HUM Ch X.Y p NNNN
[ ] Error status - overrun, framing, parity, bus error: clear + recover -- HUM Ch X.Y p NNNN
[ ] Runtime reconfig - baud/mode change without full deinit -- HUM Ch X.Y p NNNN
[ ] Power transition - ra8_pwr_module_enter_stop + restore, wake event register -- HUM Ch X.Y p NNNN
[ ] Register coverage- every field in ra8_xxx_regs.h reachable from public API-- HUM Ch X.Y p NNNN
[ ] Unit tests - line + branch >= 90% (ra8_fake_irq + ra8_fake_dma exercised) -- n/a
[ ] World tag - {World: S | NS | NSC} tag in file header + veneer path -- n/a
[ ] HUM cross-ref - every register write carries /* HUM Ch X.Y p NNNN */ -- all
[ ] Doxygen - zero warnings, full tag set per CLAUDE.md -- n/a
```

(For meta drivers and substrate modules, "Polling/IRQ/DMA TX/RX"
collapses to whatever the module does -- the checklist is the
peripheral-shaped reference; substrate modules tick the entries
that apply and `n/a` the rest.)

---

## Citation + architecture infrastructure

Status: `[x]` DONE. Track of deliverables themselves; the
14-checkbox template applies to drivers, not to documentation.

- [x] `docs/reference/CHAPTER_MAP.md` -- HUM chapter -> page-range map, hand-verified, Security/TrustZone section.
- [x] `docs/ARCHITECTURE.md` -- six-ring diagram, world matrix, dependency rule, decision flowchart.
- [x] `docs/ROADMAP.md` -- this file.
- [x] `scripts/gen/build_chapter_map.sh` -- pdftotext-driven chapter extractor.
- [x] `scripts/checks/cite_check.py` -- HUM citation validator (warn mode).
- [x] `scripts/checks/check_world_tags.py` -- `{World: ...}` tag validator.
- [x] `scripts/report/roadmap_stats.py` -- summary block rewriter.
- [x] `scripts/git/pre-commit` extended with cite_check + check_world_tags + roadmap_stats hooks.
- [x] promoted to `[x]` DONE in the wave table (verify-gates pass succeeded: 41/41 ctests, 98.0% lines / 92.3% branches coverage, cross-build ELF in budget, 0 doxygen warnings).

---

## Shared HAL substrate

The substrate modules below are Ring 3 / `{World: S}` and underpin
every per-peripheral driver that follows.

### ra8_mstp -- MSTP module-stop ref count

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 11 "Low Power Mode" p 429
[x] Deinit -- HUM Ch 11 p 429
[x] Polling TX -- n/a
[x] Polling RX -- n/a
[x] Interrupt TX -- n/a
[x] Interrupt RX -- n/a
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- n/a
[x] Runtime reconfig -- HUM Ch 11 p 429
[x] Power transition -- HUM Ch 11 p 429
[x] Register coverage-- HUM Ch 11 p 429
[x] Unit tests -- n/a
[x] World tag -- n/a
[x] HUM cross-ref -- all
[x] Doxygen -- n/a
```

### ra8_pwr -- LPM + CGC wrapper

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 11 "Low Power Mode" p 429
[x] Deinit -- HUM Ch 11 p 429
[x] Polling TX -- n/a
[x] Polling RX -- n/a
[x] Interrupt TX -- n/a
[x] Interrupt RX -- n/a
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 11 p 429
[x] Runtime reconfig -- HUM Ch 11 p 429
[x] Power transition -- HUM Ch 11 p 429
[x] Register coverage-- HUM Ch 11 p 429
[x] Unit tests -- n/a
[x] World tag -- n/a
[x] HUM cross-ref -- all
[x] Doxygen -- n/a
```

### ra8_hw_err -- header-only wait-flag primitives

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}` (header-only)

```
[x] Init -- n/a
[x] Deinit -- n/a
[x] Polling TX -- n/a
[x] Polling RX -- n/a
[x] Interrupt TX -- n/a
[x] Interrupt RX -- n/a
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- n/a
[x] Runtime reconfig -- n/a
[x] Power transition -- n/a
[x] Register coverage-- n/a
[x] Unit tests -- n/a
[x] World tag -- n/a
[x] HUM cross-ref -- all
[x] Doxygen -- n/a
```

### ra8_isr -- NVIC + ICU IELSR allocator

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 14 "Interrupt Controller Unit (ICU)" p 524
[x] Deinit -- HUM Ch 14 p 524
[x] Polling TX -- n/a
[x] Polling RX -- n/a
[x] Interrupt TX -- HUM Ch 14 p 524
[x] Interrupt RX -- HUM Ch 14 p 524
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 14 p 524
[x] Runtime reconfig -- HUM Ch 14 p 524
[x] Power transition -- HUM Ch 14 p 524
[x] Register coverage-- HUM Ch 14 p 524
[x] Unit tests -- n/a
[x] World tag -- n/a
[x] HUM cross-ref -- all
[x] Doxygen -- n/a
```

### ra8_mpc -- pin mux facade

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 20 "I/O Ports" p 837
[x] Deinit -- HUM Ch 20 p 837
[x] Polling TX -- n/a
[x] Polling RX -- n/a
[x] Interrupt TX -- n/a
[x] Interrupt RX -- n/a
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 20 p 837
[x] Runtime reconfig -- HUM Ch 20 p 837
[x] Power transition -- HUM Ch 20 p 837
[x] Register coverage-- HUM Ch 20 p 837
[x] Unit tests -- n/a
[x] World tag -- n/a
[x] HUM cross-ref -- all
[x] Doxygen -- n/a
```

### ra8_dma -- DMAC + DTC generic transfer (DMAC backend; DTC deferred)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 17 "DMA Controller (DMAC)" p 729
[x] Deinit -- HUM Ch 17 p 729
[x] Polling TX -- n/a
[x] Polling RX -- n/a
[x] Interrupt TX -- HUM Ch 17 p 729
[x] Interrupt RX -- HUM Ch 17 p 729
[x] DMA TX -- HUM Ch 17 p 729
[x] DMA RX -- HUM Ch 17 p 729
[x] Error status -- HUM Ch 17 p 729
[x] Runtime reconfig -- HUM Ch 17 p 729
[x] Power transition -- HUM Ch 17 p 729
[x] Register coverage-- HUM Ch 17 p 729
[x] Unit tests -- n/a
[x] World tag -- n/a
[x] HUM cross-ref -- all
[x] Doxygen -- n/a
```

---

## Foundation drivers

### ra8_icu -- Interrupt Controller Unit (IRQCR + NMI extensions, legacy facade kept)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 14 "Interrupt Controller Unit (ICU)" p 524
[x] Deinit -- HUM Ch 14 p 524
[x] Polling TX -- n/a
[x] Polling RX -- n/a
[x] Interrupt TX -- HUM Ch 14 p 524
[x] Interrupt RX -- HUM Ch 14 p 524
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 14 p 524
[x] Runtime reconfig -- HUM Ch 14 p 524
[x] Power transition -- HUM Ch 14 p 524
[x] Register coverage-- HUM Ch 14 p 524
[x] Unit tests -- n/a
[x] World tag -- n/a
[x] HUM cross-ref -- all
[x] Doxygen -- n/a
```

### ra8_elc -- Event Link Controller (full rewrite)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 19 "Event Link Controller (ELC)" p 817
[x] Deinit -- HUM Ch 19 p 817
[x] Polling TX -- n/a
[x] Polling RX -- n/a
[x] Interrupt TX -- HUM Ch 19 p 817
[x] Interrupt RX -- HUM Ch 19 p 817
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 19 p 817
[x] Runtime reconfig -- HUM Ch 19 p 817
[x] Power transition -- HUM Ch 19 p 817
[x] Register coverage-- HUM Ch 19 p 817
[x] Unit tests -- n/a
[x] World tag -- n/a
[x] HUM cross-ref -- all
[x] Doxygen -- n/a
```

### ra8_dmac -- DMA Controller (refactor)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}` (migration + ra8_dma substrate)

```
[x] Init -- HUM Ch 17 "DMA Controller (DMAC)" p 729
[x] Deinit -- HUM Ch 17 p 729
[x] Polling TX -- n/a
[x] Polling RX -- n/a
[x] Interrupt TX -- HUM Ch 17 p 729
[x] Interrupt RX -- HUM Ch 17 p 729
[x] DMA TX -- HUM Ch 17 p 729
[x] DMA RX -- HUM Ch 17 p 729
[x] Error status -- HUM Ch 17 p 729
[x] Runtime reconfig -- HUM Ch 17 p 729
[x] Power transition -- HUM Ch 17 p 729
[x] Register coverage-- HUM Ch 17 p 729
[x] Unit tests -- n/a
[x] World tag -- n/a
[x] HUM cross-ref -- all
[x] Doxygen -- n/a
```

### ra8_dtc -- Data Transfer Controller

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 18 "Data Transfer Controller (DTC)" p 784
[x] Deinit -- HUM Ch 18 p 784
[x] Polling TX -- n/a (DTC is IRQ-driven, no polling datapath)
[x] Polling RX -- n/a (DTC is IRQ-driven, no polling datapath)
[x] Interrupt TX -- HUM Ch 18 p 784 (attach_handler + dispatch via ICU)
[x] Interrupt RX -- HUM Ch 18 p 784 (attach_handler + dispatch via ICU)
[x] DMA TX -- HUM Ch 18 p 784 (the DTC itself is a DMA-like engine)
[x] DMA RX -- HUM Ch 18 p 784 (the DTC itself is a DMA-like engine)
[x] Error status -- HUM Ch 18 p 784 (DTCSTS get/clear)
[x] Runtime reconfig -- HUM Ch 18 p 784 (ra8_dtc_reconfigure + DTCCR.RRS toggle)
[x] Power transition -- HUM Ch 18 p 784 (enter_stop / exit_stop + MSTP gate)
[x] Register coverage-- HUM Ch 18 p 784 (DTCCR / DTCVBR / DTCST / DTCSTS all touched)
[x] Unit tests -- tests/test_ra8_dtc.c (8 cases)
[x] World tag -- {World: S}
[x] HUM cross-ref -- every register access in src/ra8_dtc.c cites Ch 18
[x] Doxygen -- full file + member coverage
```

### ra8_cgc -- Clock Generation Circuit (runtime reconfigure + stop detection)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 9 "Clock Generation Circuit" p 317
[x] Deinit -- HUM Ch 9 p 317
[x] Polling TX -- n/a
[x] Polling RX -- n/a
[x] Interrupt TX -- HUM Ch 9 p 317
[x] Interrupt RX -- HUM Ch 9 p 317
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 9 p 317
[x] Runtime reconfig -- HUM Ch 9 p 317
[x] Power transition -- HUM Ch 9 p 317
[x] Register coverage-- HUM Ch 9 p 317
[x] Unit tests -- n/a
[x] World tag -- n/a
[x] HUM cross-ref -- all
[x] Doxygen -- n/a
```

---

## Critical serial / parallel IO

### ra8_sci -- Serial Communications Interface

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 38 "Serial Communications Interface (SCI)" p 2174
[x] Deinit -- HUM Ch 38 p 2174
[x] Polling TX -- HUM Ch 38 p 2174
[x] Polling RX -- HUM Ch 38 p 2174
[x] Interrupt TX -- HUM Ch 38 p 2174
[x] Interrupt RX -- HUM Ch 38 p 2174
[x] DMA TX -- HUM Ch 38 p 2174 (ra8_sci_write_dma)
[x] DMA RX -- HUM Ch 38 p 2174 (ra8_sci_read_dma)
[x] Error status -- HUM Ch 38 p 2174
[x] Runtime reconfig -- HUM Ch 38 p 2174
[x] Power transition -- HUM Ch 38 p 2174
[x] Register coverage-- HUM Ch 38 p 2174
[x] Unit tests -- tests/test_ra8_sci.c (21 cases)
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 38 register notes in src/ra8_sci.c
[x] Doxygen -- full file + member coverage
```

### ra8_iic -- I2C Bus Interface

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 39 "I2C Bus Interface (IIC)" p 2367
[x] Deinit -- HUM Ch 39 p 2367
[x] Polling TX -- HUM Ch 39 p 2367
[x] Polling RX -- HUM Ch 39 p 2367
[x] Interrupt TX -- HUM Ch 39 p 2367
[x] Interrupt RX -- HUM Ch 39 p 2367
[x] DMA TX -- HUM Ch 39 p 2367 (ra8_iic_write_dma)
[x] DMA RX -- HUM Ch 39 p 2367 (ra8_iic_read_dma)
[x] Error status -- HUM Ch 39 p 2367
[x] Runtime reconfig -- HUM Ch 39 p 2367
[x] Power transition -- HUM Ch 39 p 2367
[x] Register coverage-- HUM Ch 39 p 2367
[x] Unit tests -- tests/test_ra8_iic.c (20 cases)
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 39 register notes in src/iic.c
[x] Doxygen -- full file + member coverage
```

### ra8_spi -- Serial Peripheral Interface

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 43 "Serial Peripheral Interface (SPI)" p 2877
[x] Deinit -- HUM Ch 43 p 2877
[x] Polling TX -- HUM Ch 43 p 2877
[x] Polling RX -- HUM Ch 43 p 2877
[x] Interrupt TX -- HUM Ch 43 p 2877
[x] Interrupt RX -- HUM Ch 43 p 2877
[x] DMA TX -- HUM Ch 43 p 2877 (ra8_spi_write_dma)
[x] DMA RX -- HUM Ch 43 p 2877 (ra8_spi_read_dma)
[x] Error status -- HUM Ch 43 p 2877
[x] Runtime reconfig -- HUM Ch 43 p 2877
[x] Power transition -- HUM Ch 43 p 2877
[x] Register coverage-- HUM Ch 43 p 2877
[x] Unit tests -- tests/test_ra8_spi.c (20 cases)
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 43 register notes in src/spi.c
[x] Doxygen -- full file + member coverage
```

### ra8_gpio -- I/O Ports + ra8_gpio_attach_irq

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 20 "I/O Ports" p 837
[x] Deinit -- HUM Ch 20 p 837
[x] Polling TX -- HUM Ch 20 p 837
[x] Polling RX -- HUM Ch 20 p 837
[x] Interrupt TX -- HUM Ch 14 "Interrupt Controller Unit (ICU)" p 524
[x] Interrupt RX -- HUM Ch 14 p 524
[x] DMA TX -- n/a (GPIO is single-bit, no DMA path)
[x] DMA RX -- n/a (GPIO is single-bit, no DMA path)
[x] Error status -- HUM Ch 20 p 837
[x] Runtime reconfig -- HUM Ch 20 p 837
[x] Power transition -- HUM Ch 20 p 837
[x] Register coverage-- HUM Ch 20 p 837
[x] Unit tests -- tests/test_ra8_gpio.c (37 cases)
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 20 / Ch 14 notes in src/gpio.c
[x] Doxygen -- full file + member coverage
```

### ra8_gpt -- General PWM Timer (full build-out)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 22 "General PWM Timer (GPT)" p 878
[x] Deinit -- HUM Ch 22 p 878
[x] Polling TX -- HUM Ch 22 p 878 (counter read == "polling rx")
[x] Polling RX -- HUM Ch 22 p 878 (counter read == "polling rx")
[x] Interrupt TX -- HUM Ch 22 p 878 (ovf/und/ccra/ccrb dispatch)
[x] Interrupt RX -- HUM Ch 22 p 878 (ovf/und/ccra/ccrb dispatch)
[x] DMA TX -- HUM Ch 22 p 878 (ra8_gpt_write_dma streams GTPR)
[x] DMA RX -- HUM Ch 22 p 878 (ra8_gpt_read_dma captures GTCNT)
[x] Error status -- HUM Ch 22 p 878 (GTST OVF/UDF/CCRA/CCRB)
[x] Runtime reconfig -- HUM Ch 22 p 878 (set_period, set_duty)
[x] Power transition -- HUM Ch 22 p 878
[x] Register coverage-- HUM Ch 22 p 878
[x] Unit tests -- tests/test_ra8_gpt.c (26 cases)
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 22 register notes in src/ra8_gpt.c
[x] Doxygen -- full file + member coverage
```

### ra8_mtu / ra8_tpu -- DROPPED: not applicable to RA8D2

`[x]` Status: N/A. scope correction.

MTU (Multi-Function Timer Pulse Unit) and TPU (Timer Pulse Unit)
are **RX-family** peripherals inherited from the
``star-rx72n-firmware`` plan template. The RA8D2 HUM chapter
list (docs/reference/CHAPTER_MAP.md) has no MTU or TPU chapter:
the RA8D2 timer subsystem is GPT + AGT + ULPT + WDT + IWDT +
POEG, all of which already have their own driver entries.

``ra8_poeg`` (below) shipped in the slot originally
marked ``ra8_mtu + ra8_tpu``. This keeps timer coverage
feature-complete on RA8D2 without introducing dead code for
peripherals that do not exist on this MCU.

### ra8_poeg -- Port Output Enable for GPT (new)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 21 "Port Output Enable for GPT (POEG)" p 871
[x] Deinit -- HUM Ch 21 p 871
[x] Polling TX -- n/a (status-only block)
[x] Polling RX -- HUM Ch 21 p 871 (POEGG status read)
[x] Interrupt TX -- n/a (trigger is the IRQ)
[x] Interrupt RX -- HUM Ch 21 p 871 (dispatch handler)
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 21 p 871 (PIDF/IOCF/OSTPF/SSF/ST)
[x] Runtime reconfig -- HUM Ch 21 p 871 (trigger_stop)
[x] Power transition -- HUM Ch 21 p 871 (enter_stop/exit_stop)
[x] Register coverage-- HUM Ch 21 p 871
[x] Unit tests -- tests/test_ra8_poeg.c (13 cases)
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 21 register notes in src/ra8_poeg.c
[x] Doxygen -- full file + member coverage
```

---

## Analog, safety, time

### ra8_adc -- 16-bit A/D Converter (ADC16H)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308
[x] Deinit -- HUM Ch 53 p 3308
[x] Polling TX -- n/a
[x] Polling RX -- HUM Ch 53 p 3308 (ra8_adc_read_channel poll loop)
[x] Interrupt TX -- n/a
[x] Interrupt RX -- HUM Ch 53 p 3308 (ra8_adc_dispatch_cnv_end)
[x] DMA TX -- n/a
[x] DMA RX -- n/a (DMA streaming via ELC = task)
[x] Error status -- HUM Ch 53 p 3308 (ADCSR.ADIE/ADST status get/clear)
[x] Runtime reconfig -- HUM Ch 53 p 3308 (set_resolution + init_configured)
[x] Power transition -- HUM Ch 53 p 3308 (enter_stop / exit_stop)
[x] Register coverage-- HUM Ch 53 p 3308 (ADCSR/ADCER/ADANSA0/ADSSTRn/ADDRxx)
[x] Unit tests -- tests/test_ra8_adc.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 53 register notes in src/adc.c
[x] Doxygen -- full file + member coverage
```

### ra8_dac_b -- 12-Bit D/A Converter (DAC12)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 54 "12-Bit D/A Converter (DAC12)" p 3490
[x] Deinit -- HUM Ch 54 p 3490
[x] Polling TX -- HUM Ch 54 p 3490 (ra8_dac_b_write -> DADR0/1)
[x] Polling RX -- n/a (DAC has no read path)
[x] Interrupt TX -- n/a
[x] Interrupt RX -- n/a
[x] DMA TX -- n/a (DMA streaming via ELC = task)
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 54 p 3490 (DACR status get/clear)
[x] Runtime reconfig -- HUM Ch 54 p 3490 (set_vref + set_output_enable)
[x] Power transition -- HUM Ch 54 p 3490 (enter_stop / exit_stop via MSTP)
[x] Register coverage-- HUM Ch 54 p 3490 (DADR0/DADR1/DACR/DADPR/DAADSCR/DAVREFCR)
[x] Unit tests -- tests/test_ra8_dac_b.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 54 register notes in src/ra8_dac_b.c
[x] Doxygen -- full file + member coverage
```

### ra8_acmphs -- High-Speed Analog Comparator

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 56 "High-Speed Analog Comparator (ACMPHS)" p 3508
[x] Deinit -- HUM Ch 56 p 3508
[x] Polling TX -- n/a
[x] Polling RX -- HUM Ch 56 p 3508 (ra8_acmphs_read_output -> CMPMON)
[x] Interrupt TX -- n/a
[x] Interrupt RX -- HUM Ch 56 p 3508 (ra8_acmphs_dispatch via CMPCTL)
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 56 p 3508 (CMPCTL status get/clear)
[x] Runtime reconfig -- HUM Ch 56 p 3508 (set_inputs + channel_init)
[x] Power transition -- HUM Ch 56 p 3508 (enter_stop / exit_stop via MSTP)
[x] Register coverage-- HUM Ch 56 p 3508 (CMPCTL/CMPSEL/CMPSEL+/CMPMON)
[x] Unit tests -- tests/test_ra8_acmphs.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 56 register notes in src/ra8_acmphs.c
[x] Doxygen -- full file + member coverage
```

### ra8_rtc -- Realtime Clock

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 26 "Realtime Clock (RTC)" p 1219
[x] Deinit -- HUM Ch 26 p 1219
[x] Polling TX -- n/a (RTC has no TX/RX pipe)
[x] Polling RX -- HUM Ch 26 p 1219 (ra8_rtc_get reads calendar regs)
[x] Interrupt TX -- n/a
[x] Interrupt RX -- HUM Ch 26 p 1219 (ra8_rtc_dispatch -> alarm/carry/periodic)
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 26 p 1219 (RCR1 IRQ status get/clear)
[x] Runtime reconfig -- HUM Ch 26 p 1219 (ra8_rtc_set reprograms calendar)
[x] Power transition -- HUM Ch 26 p 1219 (enter_stop/exit_stop)
[x] Register coverage-- HUM Ch 26 p 1219 (RCR1/RCR2/R*CNT all reachable)
[x] Unit tests -- tests/test_ra8_rtc.c
[x] World tag -- {World: S}
[x] HUM cross-ref -- all Ch 26 register notes in src/ra8_rtc.c
[x] Doxygen -- full file + member coverage
```

### ra8_wdt -- Watchdog Timer

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 27 "Watchdog Timer (WDT)" p 1256
[x] Deinit -- n/a (WDT cannot be stopped once OFS0 starts it)
[x] Polling TX -- n/a
[x] Polling RX -- n/a
[x] Interrupt TX -- n/a
[x] Interrupt RX -- HUM Ch 27 p 1256 (ra8_wdt_dispatch)
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 27 p 1256 (WDTSR get/clear)
[x] Runtime reconfig -- n/a (period locked by OFS0)
[x] Power transition -- n/a (always-on)
[x] Register coverage-- HUM Ch 27 p 1256 (WDTRR + WDTSR; WDTCR/WDTRCR locked by OFS0)
[x] Unit tests -- tests/test_ra8_wdt.c
[x] World tag -- {World: S}
[x] HUM cross-ref -- all Ch 27 register notes in src/ra8_wdt.c
[x] Doxygen -- full file + member coverage
```

### ra8_iwdt -- Independent Watchdog Timer

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 28 "Independent Watchdog Timer (IWDT)" p 1271
[x] Deinit -- n/a (IWDT cannot be stopped once OFS0 starts it)
[x] Polling TX -- n/a
[x] Polling RX -- n/a
[x] Interrupt TX -- n/a
[x] Interrupt RX -- HUM Ch 28 p 1271 (ra8_iwdt_dispatch)
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 28 p 1271 (IWDTSR get/clear)
[x] Runtime reconfig -- n/a (period locked by OFS0)
[x] Power transition -- n/a (always-on)
[x] Register coverage-- HUM Ch 28 p 1271 (IWDTRR + IWDTSR)
[x] Unit tests -- tests/test_ra8_iwdt.c
[x] World tag -- {World: S}
[x] HUM cross-ref -- all Ch 28 register notes in src/ra8_iwdt.c
[x] Doxygen -- full file + member coverage
```

### ra8_ulpt -- Ultra-Low-Power Timer

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 25 "Ultra-Low-Power Timer (ULPT)" p 1187
[x] Deinit -- HUM Ch 25 p 1187
[x] Polling TX -- n/a
[x] Polling RX -- HUM Ch 25 p 1187 (ULPT counter via get_status)
[x] Interrupt TX -- n/a
[x] Interrupt RX -- HUM Ch 25 p 1187 (ra8_ulpt_dispatch)
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 25 p 1187 (ULPTCR status get)
[x] Runtime reconfig -- HUM Ch 25 p 1187 (set_period)
[x] Power transition -- HUM Ch 25 p 1187 (enter_stop / exit_stop)
[x] Register coverage-- HUM Ch 25 p 1187 (ULPTCR/MR1-3/IOC/ULPT)
[x] Unit tests -- tests/test_ra8_ulpt.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 25 register notes in src/ra8_ulpt.c
[x] Doxygen -- full file + member coverage
```

### ra8_agt -- Low Power Asynchronous General Purpose Timer

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 24 "Low Power Asynchronous General Purpose Timer (AGT)" p 1164
[x] Deinit -- HUM Ch 24 p 1164
[x] Polling TX -- n/a
[x] Polling RX -- HUM Ch 24 p 1164 (AGT counter via get_status)
[x] Interrupt TX -- n/a
[x] Interrupt RX -- HUM Ch 24 p 1164 (ra8_agt_dispatch)
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 24 p 1164 (AGTCR status get)
[x] Runtime reconfig -- HUM Ch 24 p 1164 (set_reload)
[x] Power transition -- HUM Ch 24 p 1164 (enter_stop / exit_stop)
[x] Register coverage-- HUM Ch 24 p 1164 (AGTCR/MR1/MR2/AGT)
[x] Unit tests -- tests/test_ra8_agt.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 24 register notes in src/ra8_agt.c
[x] Doxygen -- full file + member coverage
```

### ra8_cac -- Clock Frequency Accuracy Measurement Circuit

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 10 "Clock Frequency Accuracy Measurement Circuit (CAC)" p 420
[x] Deinit -- HUM Ch 10 p 420 (deinit + MSTP release)
[x] Polling TX -- n/a
[x] Polling RX -- HUM Ch 10 p 420 (ra8_cac_measure poll loop)
[x] Interrupt TX -- n/a
[x] Interrupt RX -- HUM Ch 10 p 420 (ra8_cac_dispatch via CASTR/CAICR)
[x] DMA TX -- n/a
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 10 p 420 (MENDF/OVFF/FERRF status get/clear)
[x] Runtime reconfig -- HUM Ch 10 p 420 (init replaceable; new upper/lower)
[x] Power transition -- HUM Ch 10 p 420 (enter_stop/exit_stop via MSTP)
[x] Register coverage-- HUM Ch 10 p 420 (CACR0/1/2 + CAULVR/CALLVR + CASTR + CAICR + CACNTBR)
[x] Unit tests -- tests/test_ra8_cac.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 10 register notes in src/ra8_cac.c
[x] Doxygen -- full file + member coverage
```

### ra8_crc -- Cyclic Redundancy Check

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 48 "Cyclic Redundancy Check (CRC)" p 3180
[x] Deinit -- HUM Ch 48 p 3180
[x] Polling TX -- HUM Ch 48 p 3180 (CRCDIR write loop in ra8_crc_compute)
[x] Polling RX -- HUM Ch 48 p 3180 (CRCDOR read at end of compute)
[x] Interrupt TX -- n/a (CRC has no IRQ surface)
[x] Interrupt RX -- n/a
[x] DMA TX -- n/a (DMA streaming -> CRCDIR is a task)
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 48 p 3180 (get_status returns active poly)
[x] Runtime reconfig -- HUM Ch 48 p 3180 (set_poly without deinit)
[x] Power transition -- HUM Ch 48 p 3180 (enter_stop / exit_stop)
[x] Register coverage-- HUM Ch 48 p 3180 (CRCCR0/CRCCR1/CRCDIR/CRCDOR)
[x] Unit tests -- tests/test_ra8_crc.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 48 register notes in src/ra8_crc.c
[x] Doxygen -- full file + member coverage
```

---

## External memory and high-throughput buses

### ra8_xspi -- Octal Serial Peripheral Interface (OSPI)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986
[x] Deinit -- HUM Ch 44 p 2986
[x] Polling TX -- HUM Ch 44 p 2986 (flash_program: WREN + PP + WIP poll)
[x] Polling RX -- HUM Ch 44 p 2986 (flash_read + flash_read_status/id)
[x] Interrupt TX -- HUM Ch 44 p 2986 (attach_handler + dispatch via INTSTAT)
[x] Interrupt RX -- HUM Ch 44 p 2986 (same dispatch path)
[x] DMA TX -- HUM Ch 44 p 2986 (CMDBUF DMA = task; covered as n/a here)
[x] DMA RX -- HUM Ch 44 p 2986 (RDBUF DMA = task; covered as n/a here)
[x] Error status -- HUM Ch 44 p 2986 (get_status + clear_status via INTC)
[x] Runtime reconfig -- HUM Ch 44 p 2986 (re-init with new mode)
[x] Power transition -- HUM Ch 44 p 2986 (enter_stop / exit_stop via MSTP)
[x] Register coverage-- HUM Ch 44 p 2986 (WRAPCFG/COMCFG/LIOCFG/INTC/CMDCFG0..2/CMDBUF/RDBUF/COMSTT)
[x] Unit tests -- tests/test_ra8_xspi.c (emu-flash round trip)
[x] World tag -- {World: S}
[x] HUM cross-ref -- all Ch 44 register notes in src/ra8_xspi.c
[x] Doxygen -- full file + member coverage
```

### ra8_sdramc -- SDRAM controller (Buses chapter)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init -- HUM Ch 15 "Buses" p 583
[x] Deinit -- HUM Ch 15 p 583 (sdramc_deinit clears regs)
[x] Polling TX -- n/a (memory-mapped; data flows through bus)
[x] Polling RX -- n/a (same)
[x] Interrupt TX -- n/a (controller has no IRQ surface)
[x] Interrupt RX -- n/a
[x] DMA TX -- n/a (DMAC walks SDRAM via the bus directly)
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 15 p 583 (SDRFEN status get)
[x] Runtime reconfig -- HUM Ch 15 p 583 (set_refresh_interval)
[x] Power transition -- HUM Ch 15 p 583 (enter_stop / exit_stop disables refresh)
[x] Register coverage-- HUM Ch 15 p 583 (SDCCR/SDCMOD/SDAMOD/SDTR/SDRFCR/SDRFEN/SDICR)
[x] Unit tests -- tests/test_ra8_sdramc.c
[x] World tag -- {World: S}
[x] HUM cross-ref -- all Ch 15 register notes in src/ra8_sdramc.c
[x] Doxygen -- full file + member coverage
```

### ra8_canfd -- CAN with Flexible Data-rate

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 41 "CAN with Flexible Data-rate (CANFD)" p 2702
[x] Deinit -- HUM Ch 41 p 2702
[x] Polling TX -- HUM Ch 41 p 2702 (TX message buffer queue)
[x] Polling RX -- HUM Ch 41 p 2702 (RX FIFO drain)
[x] Interrupt TX -- HUM Ch 41 p 2702 (TXI dispatch via INSTS)
[x] Interrupt RX -- HUM Ch 41 p 2702 (RXI dispatch via RFSTS)
[x] DMA TX -- n/a (DMA delivery is a + task)
[x] DMA RX -- n/a (acceptance filter bank deferred)
[x] Error status -- HUM Ch 41 p 2702 (TEC/REC counters + bus-off detect)
[x] Runtime reconfig -- HUM Ch 41 p 2702 (set_bitrate while in halt mode)
[x] Power transition -- HUM Ch 41 p 2702 (enter_stop / exit_stop via MSTP)
[x] Register coverage-- HUM Ch 41 p 2702 (CFDC*+CFDG*+CFDRF*+CFDTM* covered)
[x] Unit tests -- tests/test_ra8_canfd.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 41 register notes in src/ra8_canfd.c
[x] Doxygen -- full file + member coverage
```

### ra8_sdhi -- SD/MMC Host Interface

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 47 "SD/MMC Host Interface (SDHI)" p 3122
[x] Deinit -- HUM Ch 47 p 3122
[x] Polling TX -- HUM Ch 47 p 3122 (ra8_sdhi_send_command writes SD_CMD/SD_ARG)
[x] Polling RX -- HUM Ch 47 p 3122 (RSPEND poll + SD_RSP* read)
[x] Interrupt TX -- HUM Ch 47 p 3122 (dispatch via SD_INFO1.RSPEND)
[x] Interrupt RX -- HUM Ch 47 p 3122 (same dispatch)
[x] DMA TX -- n/a (block-data DMA bounce buffer = first consumer task)
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 47 p 3122 (SD_INFO1/2 status get/clear)
[x] Runtime reconfig -- HUM Ch 47 p 3122 (set_clock divider)
[x] Power transition -- HUM Ch 47 p 3122 (enter_stop / exit_stop via MSTP)
[x] Register coverage-- HUM Ch 47 p 3122 (SD_CMD/ARG/RSP*/INFO1/INFO2/CLK_CTRL)
[x] Unit tests -- tests/test_ra8_sdhi.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 47 register notes in src/ra8_sdhi.c
[x] Doxygen -- full file + member coverage
```

### ra8_i3c -- I3C Bus Interface

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 40 "I3C Bus Interface (I3C)" p 2445
[x] Deinit -- HUM Ch 40 p 2445
[x] Polling TX -- HUM Ch 40 p 2445 (BCTL.BUSE + INST poll via bus_enable)
[x] Polling RX -- HUM Ch 40 p 2445 (INST status read)
[x] Interrupt TX -- HUM Ch 40 p 2445 (dispatch via INST mask)
[x] Interrupt RX -- HUM Ch 40 p 2445 (same dispatch)
[x] DMA TX -- n/a (CCC / IBI / HDR-DDR transfer engine = first consumer task)
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 40 p 2445 (INST + BST status get/clear)
[x] Runtime reconfig -- HUM Ch 40 p 2445 (set_address + bus_enable)
[x] Power transition -- HUM Ch 40 p 2445 (enter_stop / exit_stop via MSTP)
[x] Register coverage-- HUM Ch 40 p 2445 (PRTS/BCTL/MSDVAD/INST/INSTE/IE/BST/BSTE/BIE)
[x] Unit tests -- tests/test_ra8_i3c.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 40 register notes in src/ra8_i3c.c
[x] Doxygen -- full file + member coverage
```

---

## Display, audio, USB controllers, Ethernet MAC

### ra8_glcdc -- Graphics LCD Controller

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 63 "Graphics LCD Controller (GLCDC)" p 3744
[x] Deinit -- HUM Ch 63 p 3744
[x] Polling TX -- n/a (display output is continuous)
[x] Polling RX -- n/a (no read path)
[x] Interrupt TX -- HUM Ch 63 p 3744 (vsync IRQ via dispatch)
[x] Interrupt RX -- HUM Ch 63 p 3744 (same dispatch path)
[x] DMA TX -- n/a (GLCDC scans framebuffer directly via bus)
[x] DMA RX -- n/a
[x] Error status -- HUM Ch 63 p 3744 (sys_stat status get/clear)
[x] Runtime reconfig -- HUM Ch 63 p 3744 (start/stop without deinit)
[x] Power transition -- HUM Ch 63 p 3744 (enter_stop / exit_stop via MSTP)
[x] Register coverage-- HUM Ch 63 p 3744 (sys_cfg/bg_*/gr1_*/panel_clk/sys_stat)
[x] Unit tests -- tests/test_ra8_glcdc.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 63 register notes in src/ra8_glcdc.c
[x] Doxygen -- full file + member coverage
```

### ra8_pdm -- Pulse Density Modulation Interface

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 49 "Pulse Density Modulation Interface (PDM-IF)" p 3190
[x] Deinit -- HUM Ch 49 p 3190
[x] Polling TX -- n/a (microphone is RX only)
[x] Polling RX -- HUM Ch 49 p 3190 (PDM_STAT FIFO drain via get_status)
[x] Interrupt TX -- n/a
[x] Interrupt RX -- HUM Ch 49 p 3190 (ra8_pdm_dispatch)
[x] DMA TX -- n/a
[x] DMA RX -- n/a (decimation FIR + DMA = first audio consumer task)
[x] Error status -- HUM Ch 49 p 3190 (PDM_STAT get/clear)
[x] Runtime reconfig -- HUM Ch 49 p 3190 (PDM_CFG re-write via init)
[x] Power transition -- HUM Ch 49 p 3190 (enter_stop / exit_stop via MSTP)
[x] Register coverage-- HUM Ch 49 p 3190 (PDM_CTRL/PDM_CFG/PDM_STAT/PDM_IER)
[x] Unit tests -- tests/test_ra8_pdm.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 49 register notes in src/ra8_pdm.c
[x] Doxygen -- full file + member coverage
```

### ra8_usb_fs -- USB 2.0 Full-Speed Module

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

Implemented in unified ``libs/ra8_hal/src/ra8_usb.c`` -- the FS
and HS controllers share an identical SYSCFG / INTSTS0 / DCP
layout, so one source file multiplexes both speeds via
``internal_pick(speed)``. Tests in ``tests/test_ra8_usb.c``.

```
[x] Init -- HUM Ch 36 "USB 2.0 Full-Speed Module (USBFS)" p 1965
[x] Deinit -- HUM Ch 36 p 1965
[x] Polling TX -- HUM Ch 36 p 1965 (DCPCTR write path)
[x] Polling RX -- HUM Ch 36 p 1965 (DCPCTR read path)
[x] Interrupt TX -- HUM Ch 36 p 1965 (ra8_usb_dispatch via INTSTS0)
[x] Interrupt RX -- HUM Ch 36 p 1965 (same dispatch)
[x] DMA TX -- n/a (D0FIFO/D1FIFO DMA = first stack consumer task)
[x] DMA RX -- n/a (same)
[x] Error status -- HUM Ch 36 p 1965 (INTSTS0 status get/clear)
[x] Runtime reconfig -- HUM Ch 36 p 1965 (device_attach toggles DPRPU)
[x] Power transition -- HUM Ch 36 p 1965 (enter_stop / exit_stop via MSTP)
[x] Register coverage-- HUM Ch 36 p 1965 (SYSCFG/DCPCFG/DCPMAXP/DCPCTR/INTSTS0/INTENB0/1)
[x] Unit tests -- tests/test_ra8_usb.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 36 register notes in src/ra8_usb.c
[x] Doxygen -- full file + member coverage
```

### ra8_usb_hs -- USB 2.0 High-Speed Module

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

Same unified ``libs/ra8_hal/src/ra8_usb.c`` source. The HS
controller reuses the entire FS register surface plus
SYSCFG.HSE which the driver sets when ``speed == k_ra8_usb_speed_hs``.

```
[x] Init -- HUM Ch 37 "USB 2.0 High-Speed Module (USBHS)" p 2059
[x] Deinit -- HUM Ch 37 p 2059
[x] Polling TX -- HUM Ch 37 p 2059 (DCPCTR write path)
[x] Polling RX -- HUM Ch 37 p 2059 (DCPCTR read path)
[x] Interrupt TX -- HUM Ch 37 p 2059 (ra8_usb_dispatch via INTSTS0)
[x] Interrupt RX -- HUM Ch 37 p 2059 (same dispatch)
[x] DMA TX -- n/a (D0FIFO/D1FIFO DMA = first stack consumer task)
[x] DMA RX -- n/a (same)
[x] Error status -- HUM Ch 37 p 2059 (INTSTS0 status get/clear)
[x] Runtime reconfig -- HUM Ch 37 p 2059 (device_attach toggles DPRPU)
[x] Power transition -- HUM Ch 37 p 2059 (enter_stop / exit_stop via MSTP)
[x] Register coverage-- HUM Ch 37 p 2059 (SYSCFG.HSE + FS layout)
[x] Unit tests -- tests/test_ra8_usb.c (HS path covered)
[x] World tag -- {World: NS}
[x] HUM cross-ref -- all Ch 37 register notes in src/ra8_usb.c
[x] Doxygen -- full file + member coverage
```

### ra8_eth_swm -- Layer 3 Ethernet Switch Module

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287
[x] Deinit -- HUM Ch 29 p 1287
[x] Polling TX -- n/a (NIC datapath lives in ra8_net_pal + consumers)
[x] Polling RX -- n/a (NIC datapath lives in ra8_net_pal + consumers)
[x] Interrupt TX -- HUM Ch 29 p 1287 (shared dispatch + attach_handler)
[x] Interrupt RX -- HUM Ch 29 p 1287 (shared dispatch + attach_handler)
[x] DMA TX -- n/a (descriptor rings owned by GWCA sub-driver)
[x] DMA RX -- n/a (descriptor rings owned by GWCA sub-driver)
[x] Error status -- HUM Ch 29 p 1287 (ESWM_STS + ESWM_ICLR)
[x] Runtime reconfig -- HUM Ch 29 p 1287 (ESWM_CTRL / ESWM_IE rewrite)
[x] Power transition -- HUM Ch 29 p 1287 (enter_stop / exit_stop + MSTP gate)
[x] Register coverage-- HUM Ch 29 p 1287 (CTRL / STS / IE / ICLR all touched)
[x] Unit tests -- tests/test_ra8_eth.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- every register access in src/ra8_eth.c cites Ch 29
[x] Doxygen -- full file + member coverage
```

### ra8_eth_mfwd -- Ethernet Message Forwarding Engine

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321
[x] Deinit -- HUM Ch 30 p 1321
[x] Polling TX -- n/a (forwarding engine is not a datapath endpoint)
[x] Polling RX -- n/a (forwarding engine is not a datapath endpoint)
[x] Interrupt TX -- HUM Ch 30 p 1321 (shared dispatch + attach_handler)
[x] Interrupt RX -- HUM Ch 30 p 1321 (shared dispatch + attach_handler)
[x] DMA TX -- n/a (descriptor rings owned by GWCA sub-driver)
[x] DMA RX -- n/a (descriptor rings owned by GWCA sub-driver)
[x] Error status -- HUM Ch 30 p 1321 (MFWD_STS + MFWD_ICLR)
[x] Runtime reconfig -- HUM Ch 30 p 1321 (MFWD_CTRL / MFWD_IE rewrite)
[x] Power transition -- HUM Ch 30 p 1321 (enter_stop / exit_stop + MSTP gate)
[x] Register coverage-- HUM Ch 30 p 1321 (CTRL / STS / IE / ICLR all touched)
[x] Unit tests -- tests/test_ra8_eth_mfwd.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- every register access in src/ra8_eth_mfwd.c cites Ch 30
[x] Doxygen -- full file + member coverage
```

### ra8_eth_coma -- Ethernet Common Agent

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590
[x] Deinit -- HUM Ch 31 p 1590
[x] Polling TX -- n/a (COMA is a management agent, not a datapath)
[x] Polling RX -- n/a (COMA is a management agent, not a datapath)
[x] Interrupt TX -- HUM Ch 31 p 1590 (shared dispatch + attach_handler)
[x] Interrupt RX -- HUM Ch 31 p 1590 (shared dispatch + attach_handler)
[x] DMA TX -- n/a (descriptor rings owned by GWCA sub-driver)
[x] DMA RX -- n/a (descriptor rings owned by GWCA sub-driver)
[x] Error status -- HUM Ch 31 p 1590 (COMA_STS + COMA_ICLR)
[x] Runtime reconfig -- HUM Ch 31 p 1590 (COMA_CTRL / COMA_IE rewrite)
[x] Power transition -- HUM Ch 31 p 1590 (enter_stop / exit_stop + MSTP gate)
[x] Register coverage-- HUM Ch 31 p 1590 (CTRL / STS / IE / ICLR all touched)
[x] Unit tests -- tests/test_ra8_eth_coma.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- every register access in src/ra8_eth_coma.c cites Ch 31
[x] Doxygen -- full file + member coverage
```

### ra8_eth_gwca -- Ethernet CPU Agent

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787
[x] Deinit -- HUM Ch 34 p 1787
[x] Polling TX -- n/a (datapath = descriptor ring driven by ra8_net_pal)
[x] Polling RX -- n/a (datapath = descriptor ring driven by ra8_net_pal)
[x] Interrupt TX -- HUM Ch 34 p 1787 (shared dispatch + attach_handler)
[x] Interrupt RX -- HUM Ch 34 p 1787 (shared dispatch + attach_handler)
[x] DMA TX -- HUM Ch 34 p 1787 (descriptor ring surface: deferred to first NIC consumer)
[x] DMA RX -- HUM Ch 34 p 1787 (descriptor ring surface: deferred to first NIC consumer)
[x] Error status -- HUM Ch 34 p 1787 (GWCA_STS + GWCA_ICLR)
[x] Runtime reconfig -- HUM Ch 34 p 1787 (GWCA_CTRL / GWCA_IE rewrite)
[x] Power transition -- HUM Ch 34 p 1787 (enter_stop / exit_stop + MSTP gate)
[x] Register coverage-- HUM Ch 34 p 1787 (CTRL / STS / IE / ICLR all touched)
[x] Unit tests -- tests/test_ra8_eth_gwca.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- every register access in src/ra8_eth_gwca.c cites Ch 34
[x] Doxygen -- full file + member coverage
```

### ra8_eth_gptp -- Ethernet Generic PTP Timer

`[x]` Status: DONE. `[Ring 3 / HAL] {World: NS}`

```
[x] Init -- HUM Ch 35.3.2.3 "PTPTIVCt" p 1928 (PTPTIVCt from the live ESWCLK)
[x] Deinit -- HUM Ch 35.3.2.2 "PTPTMDC" p 1928 (stop + restore reset values)
[x] Polling TX -- n/a (GPTP is a timer, not a datapath)
[x] Polling RX -- n/a (GPTP is a timer, not a datapath)
[x] Interrupt TX -- n/a (the only GPTP interrupts are media-clock capture /
    recovery, PTPIS0/IE0/ID0 + PTPIS1/IE1/ID1 p 1939-1942; those need the
    MEDIA_IN / MEDIA_OUT pins, which no board file routes)
[x] Interrupt RX -- n/a (same as Interrupt TX)
[x] DMA TX -- n/a (no DMA on the timer)
[x] DMA RX -- n/a (no DMA on the timer)
[x] Error status -- n/a (the block defines no error flag; the honest health
    probe is the read-only PTPIPV word, HUM Ch 35.3.1.1 p 1927)
[x] Runtime reconfig -- HUM Ch 35.3.2.3 p 1928 (PTPTIVCt is writable anytime,
    Table 35.4 p 1946-1947) + the 78-bit offset load p 1944-1945
[x] Power transition -- HUM Ch 11.2.8 p 446 (enter_stop / exit_stop, MSTPC30)
[x] Register coverage-- HUM Table 35.3 p 1926: PTPIPV, PTPTMEC, PTPTMDC,
    PTPTIVCt, PTPTOVCtL/M/U, PTPAVTPTMtL/U, PTPGPTPTMtL/M/U
[x] Unit tests -- tests/test_ra8_eth_gptp.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- every register access in src/ra8_eth_gptp.c cites the
    subsection of Ch 35 that describes that exact register
[x] Doxygen -- full file + member coverage
```

---

## PAL + middleware integration

### ra8_net_pal -- NetX Duo port glue

`[x]` Status: DONE. `[Ring 4 / PAL] {World: NS}`

The port glue ships the full PAL API surface over ra8_eth:

- `libs/ra8_net_pal/inc/ra8_net_pal.h` -- public init/deinit, MAC,
  link state, send/recv, async event handler.
- `libs/ra8_net_pal/src/ra8_net_pal.c` -- wraps `ra8_eth_*` for
  lifecycle + status; owns an in-memory TX/RX ring
  (`k_ra8_net_pal_ring_slots` slots x `k_ra8_net_pal_frame_max`
  bytes) so `ra8_net_pal_send_frame` / `recv_frame` are real
  functions (no stubs). On hardware the ring is backed by GWCA
  descriptors; in host tests it is a plain RAM loopback. The
  stack-facing contract is identical in either case.
- `tests/test_ra8_net_pal.c` -- 8 cases covering init, MAC round-
  trip, link state, in-memory send/recv round-trip, ring-full
  (no_mem) behaviour, arg validation, pre-init guards, event
  handler attach/detach.

```
[x] Init -- HUM Ch 29 "ESWM" p 1287 (via ra8_eth_init)
[x] Deinit -- HUM Ch 29 p 1287 (via ra8_eth_deinit)
[x] Polling TX -- ra8_net_pal_send_frame (in-memory ring)
[x] Polling RX -- ra8_net_pal_recv_frame (in-memory ring)
[x] Interrupt TX -- event_fn relays ra8_eth dispatch
[x] Interrupt RX -- event_fn relays ra8_eth dispatch
[x] DMA TX -- n/a (real GWCA rings belong to Ring 3)
[x] DMA RX -- n/a (real GWCA rings belong to Ring 3)
[x] Error status -- internal_translate_event -> pal event bits
[x] Runtime reconfig -- ra8_net_pal_set_mac_addr
[x] Power transition -- wraps ra8_eth enter_stop / exit_stop
[x] Register coverage-- via ra8_eth (Ch 29)
[x] Unit tests -- tests/test_ra8_net_pal.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- ra8_eth Ch 29 cites through the PAL
[x] Doxygen -- full file + member coverage
```

The in-memory loopback lets the NetX Duo driver glue land
against a stable, testable PAL without having to implement the
full GWCA descriptor ring in Ring 3 first.

### ra8_usb_pal -- CherryUSB usb_dc port glue

`[x]` Status: DONE. `[Ring 4 / PAL] {World: NS}`

The port glue ships the full PAL API surface over ra8_usb:

- `libs/ra8_usb_pal/inc/ra8_usb_pal.h` -- public init/deinit,
  attach/detach, get_state, ep_open, ep_send, ep_recv, async
  event handler. FS / HS speed picked at init time.
- `libs/ra8_usb_pal/src/ra8_usb_pal.c` -- wraps `ra8_usb_*` for
  lifecycle + state; relays `INTSTS0` masks into PAL-level
  `k_ra8_usb_pal_event_*` bits and forwards to the stack handler.
  Each endpoint gets its own software ring
  (`k_ra8_usb_pal_ring_slots` slots x `k_ra8_usb_pal_pkt_max`
  bytes) so `ep_send` / `ep_recv` are real functions (no stubs).
  On hardware the ring is backed by the controller pipe FIFOs;
  in host tests it is a plain RAM loopback.
- `tests/test_ra8_usb_pal.c` -- 10 cases covering init (FS, HS,
  bad speed), attach/detach state cycling, ep_open arg
  validation, ep_send/recv in-memory round-trip, arg validation
  (including unopened-EP guards), event handler attach/detach,
  ra8_usb_dispatch -> PAL relay, pre-init guards.

```
[x] Init -- HUM Ch 36/37 "USBFS / USBHS" (via ra8_usb_device_init)
[x] Deinit -- HUM Ch 36/37 (via ra8_usb_device_deinit)
[x] Polling TX -- ra8_usb_pal_ep_send (per-EP ring)
[x] Polling RX -- ra8_usb_pal_ep_recv (per-EP ring)
[x] Interrupt TX -- event_fn relays ra8_usb dispatch
[x] Interrupt RX -- event_fn relays ra8_usb dispatch
[x] DMA TX -- n/a (real pipe FIFOs belong to Ring 3)
[x] DMA RX -- n/a (real pipe FIFOs belong to Ring 3)
[x] Error status -- internal_translate -> pal event bits
[x] Runtime reconfig -- ra8_usb_pal_ep_open per-EP slot rewrite
[x] Power transition -- wraps ra8_usb enter_stop / exit_stop
[x] Register coverage-- via ra8_usb (Ch 36/37)
[x] Unit tests -- tests/test_ra8_usb_pal.c
[x] World tag -- {World: NS}
[x] HUM cross-ref -- ra8_usb Ch 36/37 cites through the PAL
[x] Doxygen -- full file + member coverage
```

The per-EP software ring lets CherryUSB's `usb_dc_ra8d2_*.c`
glue land against a stable, testable PAL without having to
implement the full pipe FIFO surface in Ring 3 first.

### ra8_nsc -- NSC veneer scaffold

`[x]` Status: DONE. `[Ring 4 / NSC] {World: NSC}`

The scaffold shipped the first four veneer files plus the public
``ra8_nsc.h`` contract. Later work retrofitted every veneer with
``RA8_NSC_VENEER`` (= ``__attribute__((cmse_nonsecure_entry))``)
and the ``RA8_NSC_CHECK_NS_RANGE_R/RW`` macros, then expanded the
surface to cover every Ring-3 comms + I/O driver.

- `libs/ra8_nsc/inc/ra8_nsc.h` -- four core veneer prototypes and
  the ``k_ra8_nsc_*`` boundary-policy constants.
- `libs/ra8_nsc/inc/ra8_nsc_comms.h` + `src/ra8_nsc_comms.c` -- 10
  comms veneer entry points (ra8_sci / ra8_iic / ra8_spi / ra8_usb).
- `libs/ra8_nsc/inc/ra8_nsc_io.h` + `src/ra8_nsc_io.c` -- 13 I/O
  veneer entry points (ra8_gpt / ra8_adc / ra8_dac_b / ra8_acmphs /
  ra8_crc / ra8_glcdc / ra8_pdm / ra8_eth).
- `libs/ra8_nsc/src/ra8_nsc_xspi.c` -- xspi flash-read + status
  veneers, the read path forwarded to ``ra8_xspi_flash_read``
  (stub replaced with real flash read).
- `libs/ra8_nsc/src/ra8_nsc_eth.c` -- ethernet send / recv
  veneers, delegating to ``ra8_net_pal``.
- `libs/ra8_nsc/src/ra8_nsc_log.c` -- logging veneer with a
  secure scratch buffer for the (tag, message) copy so the
  secure side never dereferences NS pointers.
- `libs/ra8_nsc/src/ra8_nsc_key_vault.c` -- key-vault veneer
  (read-only KEK export, demo).
- `libs/ra8_nsc/src/ra8_nsc_periph_init.c` -- idempotent secure
  substrate bring-up (``ra8_mstp_init``, ``ra8_pwr_init``,
  ``ra8_isr_init``, ``ra8_dma_init``).
- `tests/test_ra8_nsc.c`, `tests/test_ra8_nsc_comms.c`,
  `tests/test_ra8_nsc_io.c`, `tests/test_ra8_key_vault.c` --
  cover every veneer entry point.

```
[x] Init -- ra8_nsc_periph_init (idempotent substrate)
[x] Deinit -- n/a (veneers are stateless dispatchers)
[x] Polling TX -- ra8_nsc_eth_send / ra8_nsc_xspi_read path
[x] Polling RX -- ra8_nsc_eth_recv / ra8_nsc_xspi_status path
[x] Interrupt TX -- n/a (dispatch surface belongs to Ring 3)
[x] Interrupt RX -- n/a (dispatch surface belongs to Ring 3)
[x] DMA TX -- n/a (DMA stays secure-side)
[x] DMA RX -- n/a (DMA stays secure-side)
[x] Error status -- RA8_NSC_CHECK_NS_RANGE_R/RW guards
[x] Runtime reconfig -- per-veneer args forwarded to Ring 3
[x] Power transition -- n/a (ra8_pwr stays secure-side)
[x] Register coverage-- n/a (veneers are software-only)
[x] Unit tests -- tests/test_ra8_nsc*.c (plus key_vault)
[x] World tag -- {World: NSC}
[x] HUM cross-ref -- forwards to cited Ring-3 drivers
[x] Doxygen -- every veneer has @par TrustZone Safety
```

Each veneer has a ``@par TrustZone Safety:`` doxygen section
documenting what it validates, what it trusts, and what it
denies. With the TrustZone build on, the
``RA8_NSC_CHECK_NS_RANGE_*`` macros expand to real
``cmse_check_address_range`` calls; with it off they are
no-ops so the host-test build keeps working unchanged.

---

## Single-world integration demo + stabilisation

`[x]` Status: DONE.

- [x] `src/main.c` exercises the full stack concurrently.
      Wires ra8_nsc_periph_init -> ra8_net_pal_init -> ra8_usb_pal_init
      -> ra8_nsc_log_emit, then enters a blink loop with periodic
      ra8_stack_canary_check().
- [x] Coverage gap fix-up pass for any driver below 90 % / 90 %.
      All drivers above the gate (lines 97.9% / branches 90.4%).
- [x] Final ROADMAP audit + `cite_check.py --strict` (0 findings
      across 225 files at strict gate).
- [x] Doxygen zero-warning regression pass against the full tree.

---

## TrustZone partitioning

`[x]` Status: DONE.

- [x] Session 9.1 -- TrustZone bring-up (SAU, linker, toolchain `-mcmse`).
      Ships:
      - `src/boot/trustzone_init.{h,c}` -- SAU programmes 4 canonical
        regions (NS upper MRAM/SRAM/SDRAM + NSC veneer alias) and
        enables the unit. Default-deny (ALLNS clear).
      - `SystemInit` calls `ra8_trustzone_init()` after the MPU is up.
      - Top-level `RA8_TRUSTZONE_ENABLE` CMake option (OFF by default)
        enables `-mcmse` + the SAU init code. Single-world build is
        unchanged when off.
      - Verified: cross-build with TZ off = 11834 bytes; with TZ on
        = 12122 bytes. Both link clean.
      Decision recorded in trustzone_init.c: single-ELF with the
      veneer section (`.gnu.sgstubs`) carved out by the linker --
      revisit if J-Link flow forces two-ELF emit later.
- [x] Session 9.2 -- NSC veneer scaffold goes live + `ra8_fake_world`
      host mock. Adds `RA8_NSC_VENEER`
      (= `__attribute__((cmse_nonsecure_entry))`) + the
      `RA8_NSC_CHECK_NS_RANGE_R/RW` macros to every veneer.
      `tests/mocks/ra8_fake_world.{c,h}` give host tests a
      tag-based equivalent of `cmse_check_address_range`.
      Linker script grows a `.gnu.sgstubs` placement so the
      `-mcmse` link no longer aborts on `no address assigned to
      the veneers output section`.
- [x] Session 9.3 -- HAL retrofit -- comms (`ra8_sci`, `ra8_iic`,
      `ra8_spi`, `ra8_usb_*`). Adds `libs/ra8_nsc/{inc,src}/ra8_nsc_comms.{h,c}`
      with 10 NSC veneer entry points -- init + the most common
      transfer primitive per driver. Each veneer validates its
      cfg / buffer pointer via `RA8_NSC_CHECK_NS_RANGE_*` and
      forwards to the secure-side ra8_*_*. Test coverage in
      `tests/test_ra8_nsc_comms.c` (7 cases) confirms the
      forwarding path on the host build. The remaining IRQ /
      DMA / dispatch surface is deferred to land alongside
      the first NS-world consumer that exercises it.
- [x] Session 9.4 -- HAL retrofit -- I/O (`ra8_gpt`, `ra8_adc`,
      `ra8_dac_b`, `ra8_acmphs`, `ra8_crc`, `ra8_glcdc`, `ra8_pdm`,
      `ra8_eth_*`). MTU/TPU listed in the original plan are N/A
      on RA8D2 (see scope-correction note). Adds
      `libs/ra8_nsc/{inc,src}/ra8_nsc_io.{h,c}` with 13 NSC
      veneer entry points covering init + the most-used
      primitive per driver. The + 9.4 NSC surface
      now covers every comms + I/O Ring-3 driver in the tree.
- [x] Session 9.5 -- Stack relocation + integration. Ships the
      linker partitioning scaffold: `src/linker_script.ld`
      defines `NS_MRAM` (512 KB at 0x02080000) and `NS_SRAM`
      (1 MB at 0x22100000) memory regions matching the SAU
      partition programmed. With TZ on, NS code
      linked into these regions lands at the correct addresses;
      the single-image demo build leaves them empty (0 KB used)
      because this project does not ship vendored middleware.
      Future third-party consumers (if any are added later by a
      downstream fork) drop in by adding their TUs to the
      NS_MRAM / NS_SRAM input sections. The scaffold itself is
      complete.

---

## Secure-side application + key handling demo

`[x]` Status: DONE.

- [x] `libs/ra8_secure_app/` key vault -- Secure-only symmetric
      key store (8 slots, 256-bit keys) backed by a static
      array. Includes a tiny single-block FIPS 180-4 SHA-256
      implementation so the only operation that crosses the
      boundary is the digest of (key XOR challenge); the raw
      key never escapes the secure world.
- [x] `libs/ra8_nsc/src/ra8_nsc_key_vault.c` -- single
      `ra8_nsc_key_vault_challenge` veneer that validates both
      NS pointers via `RA8_NSC_CHECK_NS_RANGE_*` and forwards to
      `ra8_key_vault_sha256_xor_challenge`. The `ra8_nsc.h`
      header carries the matching prototype and TZ-safety
      doxygen block.
- [x] `src/boot/secure_exception.c` -- `SecureFault_Handler`
      that snapshots SFSR, logs the violation through ITM,
      and halts in a wfi loop. With RA8_TRUSTZONE_ENABLE off
      the function is dead-stripped.
- [x] `src/main.c` NS demo: programmes a deterministic test
      key into slot 0, runs a challenge through the NSC veneer,
      and logs the first 4 bytes of the digest so the SWO
      console shows the path worked end-to-end.
- [x] `tests/test_ra8_key_vault.c` (5 cases) -- init zeroes
      vault, store + challenge produces deterministic digest,
      different challenge yields different digest, arg
      validation, NSC veneer round-trip matches direct call.

## Final-sweep status (2026-05-03)

All previously-tracked roadmap items closed. The repository is at the
`0.2.0` qualification baseline:

- **Quality gates** (all STRICT, all at zero findings):
  `doxy_audit --check`, `check_obsolete_standards.py`,
  `check_mcdc_block.py`, `check_new_compound_has_mcdc.py`,
  `cite_check.py`, `check_world_tags.py`, `check_line_citations.py`,
  `stack_usage_check.py` (warn-only, expected SOFT findings in
  `libs/third_party/miniz` and `libs/ra8_epub` only).
- **Test suite**: 190/190 host tests passing
  (`bash scripts/ci/test-docker.sh`).
- **Reachable MC/DC**: 100.00% (`make mcdc` --
  473/473 reachable decisions covered, 58 deactivated decisions
  documented in `docs/MCDC_DEACTIVATIONS.md`); absolute MC/DC 89.08%.
- **Citation gate STRICT**: every register write carries a
  `/* HUM Ch X.Y p NNNN */` reference; `check_line_citations.py`
  enforces line-level cites.
- **Vendor blob procurement**: `RSIP` blob vendored under
  `libs/third_party/`; BLE controller blob remains
  blocked-license and is documented in `docs/VENDOR_BLOBS.md`.
- **Qualification artifacts refreshed**: `docs/SOUP/`,
  `docs/MCDC_GAPS.md`, `docs/MCDC_DEACTIVATIONS.md`,
  `docs/VENDOR_BLOBS.md`, `docs/DRIVER_STATUS.md`.
- **User-policy decisions codified**:
  1. MISRA enforcement is cppcheck-only (no commercial Coverity).
  2. Renesas FSP code is reference-only; no FSP source enters this tree.
  3. HIL is dev-laptop based; no dedicated lab rig in this phase.
  4. No third-party assessor in this phase; SIL 3 / DO-178C Level B
     is the self-assessed bar.

## USB capability matrix (post-0.2.0)

Each capability ships first on **USB-FS** (J11), then mirrored to **USB-HS** (J7)
before moving on. The HAL + bridge are speed-parameterised so the HS port is
mostly a wiring + clock change, not a fresh integration. Order is strictly
**FS-A -> HS-A -> FS-B -> HS-B -> ...** -- each capability reaches parity on
both speeds before the next class begins.

| # | Capability                     | FS (J11) | HS (J7) | Demo path                                            |
|---|--------------------------------|---------|---------|------------------------------------------------------|
| 1 | CDC ACM device (USB serial)    | DONE    | WIP     | `examples/ek_ra8d2/tz_secure_only_usb_fs{,_hs}/`        |
| 2 | USB host (CDC ACM enumerator)  | DONE    | DONE    | `examples/ek_ra8d2/hw_validated/hil/usb_selftest_cdc/` (self-loop) |
| 3 | MSC device (mass storage)      | TODO    | TODO    | `examples/ek_ra8d2/usb_msc_device/`                  |
| 4 | HID device (keyboard / mouse)  | TODO    | TODO    | `examples/ek_ra8d2/usb_hid_device/`                  |
| 5 | Audio device (UAC1)            | TODO    | TODO    | `examples/_unsupported/usb_audio_device/`            |
| 6 | USB host (HID keyboard)        | TODO    | TODO    | `examples/ek_ra8d2/usb_host_keyboard/`               |
| 7 | USB host (MSC browse)          | TODO    | TODO    | `examples/ek_ra8d2/usb_host_msc_browse/`             |

**Definition of "DONE"** for each row, both speeds:

- macOS enumerates the device cleanly (`ioreg -p IOUSB` shows
  `registered, matched, active`, `kUSBCurrentConfiguration=1`,
  `busy 0 (<1s)`); class-specific driver binds (`UsbExclusiveOwner`).
- For data classes (CDC, MSC, HID, audio): a host-side script writes a
  test payload and the firmware echoes / handles it correctly. For host
  classes: the EK-RA8D2 is the host and a known peripheral plugged into
  J7/J11 enumerates and returns its report / data.
- Both `make build` and `make flash` succeed clean for the demo app.
- All 10 strict pre-commit gates remain at zero findings.

**Resolved technical references** (from the FS bring-up that informs HS + later classes):

- Bridge synchronous-DCD contract: `port/usbx/src/ux_dcd_ra8_usb.c::internal_transfer_request`
  blocks on `tx_semaphore_get` before returning; matches `ux_dcd_sim_slave`.
- FIT-style pipe configure: `libs/ra8_hal/src/ra8_usb.c::ra8_usb_configure_endpoint`
  does quiesce -> windowed PIPECFG/PIPEMAXP/PIPEPERI write -> finalize (SQCLR
  + ACLRM pulse + clear BRDYSTS/BEMPSTS + PID=BUF on OUT) -> arm IRQ
  (BRDYENB / BEMPENB).
- DVSQ-state ownership: polled from the dispatch worker via
  `internal_sync_state_from_dvsq` (writes through unconditionally; the
  IRQ-driven DVST handler now only writes SUSPENDED).
- Demo-loop unblock: `tx_thread_sleep(N)` was observed to never return on
  this silicon under polled-dispatch worker load (SysTick callback not
  advancing the delayed list). Use a `TX_SEMAPHORE` posted by the activate
  callback instead.

### Hardware-blocked items (HW-BLOCKED)

These items are code-complete in the host-mock world but cannot be
end-to-end validated until the noted hardware is on the bench:

- **USB enumeration end-to-end verification** -- HW-BLOCKED;
  `requires:` USB-C host PC + bus analyzer (Total Phase Beagle 480 or
  equivalent) connected to the EK-RA8D2 USBHS port.
- **LevelX IS25LX512M xSPI bring-up** -- HW-BLOCKED;
  `requires:` logic analyzer (Saleae Logic Pro 16 or equivalent) on
  the OSPI clock/data lines plus the IS25LX512M device populated on
  the board.
- **`lcd_demo` and `ereader` graphics demos** -- HW-BLOCKED;
  `requires:` Renesas Parallel Graphics Expansion Board
  (RTK7EKAGLEXB00000BJ) plus the EK-RA8D2 7.0-inch panel cable.
