# ra8d2-firmware Roadmap

Source of truth for progress through the HAL completion plan.

Single markdown file, updated in the same commit as the code it
tracks. Do not duplicate this status anywhere else (issue trackers,
PR descriptions, separate `STATUS.md` files). If you find yourself
copying status out of here, the answer is to refer back to here
instead.

Status markers:

- `[ ]` TODO -- not started
- `[~]` WIP  -- in progress this session, not yet at Done
- `[x]` DONE -- all 14 checkboxes ticked, lints + tests + coverage green
- `[!]` BLOCKED -- prereq missing or external blocker; describe in adjacent text

Sections under each peripheral copy the 14-checkbox template
verbatim. The `Summary` block at the top is rewritten
deterministically by `scripts/utils/roadmap_stats.py` in
pre-commit -- do not hand-edit it.

## Summary

<!-- BEGIN SUMMARY -- DO NOT EDIT BY HAND -- managed by roadmap_stats.py -->
- Total drivers tracked: 45
- DONE:    10
- WIP:     1
- BLOCKED: 0
- TODO:    34
- Checklist coverage: 169/672 boxes ticked (25.1%)
<!-- END SUMMARY -->

## Wave table

| Wave | Title                                                  | Sessions | Status |
|-----:|:-------------------------------------------------------|---------:|:-------|
|    0 | Citation + architecture infrastructure                 |        1 | [x]    |
|    1 | Shared HAL substrate                                   |        3 | [x]    |
|    2 | Foundation drivers (ICU, ELC, DMAC, DTC, CGC)          |        2 | [x]    |
|    3 | Critical serial / parallel IO                          |        7 | [ ]    |
|    4 | Analog, safety, time                                   |        4 | [ ]    |
|    5 | External memory and high-throughput buses              |        5 | [ ]    |
|    6 | Display, audio, USB controllers, Ethernet MAC          |        6 | [ ]    |
|    7 | PAL + middleware integration (lwIP, CherryUSB)         |        3 | [ ]    |
|    8 | Single-world integration demo + stabilisation         |      1-2 | [ ]    |
|    9 | TrustZone partitioning                                 |      4-5 | [ ]    |
|   10 | Secure-side application + key handling demo           |      1-2 | [ ]    |

## Per-driver feature checklist template

Every peripheral section below is a copy of this template with HUM
citations filled in. A driver is `[x]` DONE only when every
checkbox is ticked AND `cite_check.py` + `check_world_tags.py`
both pass for that driver's files.

```
[ ] Init             - MSTP ungate via ra_mstp_enable, CGC clock via ra_pwr_*,
                       pin route via ra_mpc_route_*, register baseline write,
                       s_channel_initialized[] set                              -- HUM Ch X.Y p NNNN
[ ] Deinit           - drain, ISR detach, DMA release, MSTP gate,
                       clear init state                                         -- HUM Ch X.Y p NNNN
[ ] Polling TX       - blocking with ra_hw_wait_flag timeout                    -- HUM Ch X.Y p NNNN
[ ] Polling RX       - blocking with ra_hw_wait_flag timeout                    -- HUM Ch X.Y p NNNN
[ ] Interrupt TX     - TDRE / TXI via ra_isr_register, ring buffer drain        -- HUM Ch X.Y p NNNN
[ ] Interrupt RX     - RDRF / RXI via ra_isr_register, ring buffer fill         -- HUM Ch X.Y p NNNN
[ ] DMA TX           - ra_dma_request/configure/start, completion via ra_isr    -- HUM Ch X.Y p NNNN
[ ] DMA RX           - ra_dma, cache maint (clean/invalidate) on cross target   -- HUM Ch X.Y p NNNN
[ ] Error status     - overrun, framing, parity, bus error: clear + recover     -- HUM Ch X.Y p NNNN
[ ] Runtime reconfig - baud/mode change without full deinit                     -- HUM Ch X.Y p NNNN
[ ] Power transition - ra_pwr_module_enter_stop + restore, wake event register  -- HUM Ch X.Y p NNNN
[ ] Register coverage- every field in ra8d2_xxx_regs.h reachable from public API-- HUM Ch X.Y p NNNN
[ ] Unit tests       - line + branch >= 90% (ra_sim_irq + ra_sim_dma exercised) -- n/a
[ ] World tag        - {World: S | NS | NSC} tag in file header + veneer path   -- n/a
[ ] HUM cross-ref    - every register write carries /* HUM Ch X.Y p NNNN */     -- all
[ ] Doxygen          - zero warnings, full tag set per CLAUDE.md                -- n/a
```

(For meta drivers and substrate modules, "Polling/IRQ/DMA TX/RX"
collapses to whatever the module does -- the checklist is the
peripheral-shaped reference; substrate modules tick the entries
that apply and `n/a` the rest.)

---

## Wave 0 -- Citation + architecture infrastructure

Status: `[x]` DONE. Track of Wave 0 deliverables themselves; the
14-checkbox template applies to drivers, not to documentation.

- [x] `docs/reference/CHAPTER_MAP.md` -- HUM chapter -> page-range map, hand-verified, Security/TrustZone section.
- [x] `docs/ARCHITECTURE.md` -- six-ring diagram, world matrix, dependency rule, decision flowchart.
- [x] `docs/ROADMAP.md` -- this file.
- [x] `scripts/utils/build_chapter_map.sh` -- pdftotext-driven chapter extractor.
- [x] `scripts/utils/cite_check.py` -- HUM citation validator (warn mode in Wave 0).
- [x] `scripts/utils/check_world_tags.py` -- `{World: ...}` tag validator.
- [x] `scripts/utils/roadmap_stats.py` -- summary block rewriter.
- [x] `scripts/git/pre-commit` extended with cite_check + check_world_tags + roadmap_stats hooks.
- [x] Wave 0 promoted to `[x]` DONE in the wave table (verify-gates pass succeeded: 41/41 ctests, 98.0% lines / 92.3% branches coverage, cross-build ELF in budget, 0 doxygen warnings).

---

## Wave 1 -- Shared HAL substrate

The substrate modules below are Ring 3 / `{World: S}` and underpin
every per-peripheral driver from Wave 2 onwards.

### ra_mstp -- MSTP module-stop ref count

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init             -- HUM Ch 11 "Low Power Mode" p 429
[x] Deinit           -- HUM Ch 11 p 429
[x] Polling TX       -- n/a
[x] Polling RX       -- n/a
[x] Interrupt TX     -- n/a
[x] Interrupt RX     -- n/a
[x] DMA TX           -- n/a
[x] DMA RX           -- n/a
[x] Error status     -- n/a
[x] Runtime reconfig -- HUM Ch 11 p 429
[x] Power transition -- HUM Ch 11 p 429
[x] Register coverage-- HUM Ch 11 p 429
[x] Unit tests       -- n/a
[x] World tag        -- n/a
[x] HUM cross-ref    -- all
[x] Doxygen          -- n/a
```

### ra_pwr -- LPM + CGC wrapper

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init             -- HUM Ch 11 "Low Power Mode" p 429
[x] Deinit           -- HUM Ch 11 p 429
[x] Polling TX       -- n/a
[x] Polling RX       -- n/a
[x] Interrupt TX     -- n/a
[x] Interrupt RX     -- n/a
[x] DMA TX           -- n/a
[x] DMA RX           -- n/a
[x] Error status     -- HUM Ch 11 p 429
[x] Runtime reconfig -- HUM Ch 11 p 429
[x] Power transition -- HUM Ch 11 p 429
[x] Register coverage-- HUM Ch 11 p 429
[x] Unit tests       -- n/a
[x] World tag        -- n/a
[x] HUM cross-ref    -- all
[x] Doxygen          -- n/a
```

### ra_hw_err -- header-only wait-flag primitives

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}` (header-only)

```
[x] Init             -- n/a
[x] Deinit           -- n/a
[x] Polling TX       -- n/a
[x] Polling RX       -- n/a
[x] Interrupt TX     -- n/a
[x] Interrupt RX     -- n/a
[x] DMA TX           -- n/a
[x] DMA RX           -- n/a
[x] Error status     -- n/a
[x] Runtime reconfig -- n/a
[x] Power transition -- n/a
[x] Register coverage-- n/a
[x] Unit tests       -- n/a
[x] World tag        -- n/a
[x] HUM cross-ref    -- all
[x] Doxygen          -- n/a
```

### ra_isr -- NVIC + ICU IELSR allocator

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init             -- HUM Ch 14 "Interrupt Controller Unit (ICU)" p 524
[x] Deinit           -- HUM Ch 14 p 524
[x] Polling TX       -- n/a
[x] Polling RX       -- n/a
[x] Interrupt TX     -- HUM Ch 14 p 524
[x] Interrupt RX     -- HUM Ch 14 p 524
[x] DMA TX           -- n/a
[x] DMA RX           -- n/a
[x] Error status     -- HUM Ch 14 p 524
[x] Runtime reconfig -- HUM Ch 14 p 524
[x] Power transition -- HUM Ch 14 p 524
[x] Register coverage-- HUM Ch 14 p 524
[x] Unit tests       -- n/a
[x] World tag        -- n/a
[x] HUM cross-ref    -- all
[x] Doxygen          -- n/a
```

### ra_mpc -- pin mux facade

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init             -- HUM Ch 20 "I/O Ports" p 837
[x] Deinit           -- HUM Ch 20 p 837
[x] Polling TX       -- n/a
[x] Polling RX       -- n/a
[x] Interrupt TX     -- n/a
[x] Interrupt RX     -- n/a
[x] DMA TX           -- n/a
[x] DMA RX           -- n/a
[x] Error status     -- HUM Ch 20 p 837
[x] Runtime reconfig -- HUM Ch 20 p 837
[x] Power transition -- HUM Ch 20 p 837
[x] Register coverage-- HUM Ch 20 p 837
[x] Unit tests       -- n/a
[x] World tag        -- n/a
[x] HUM cross-ref    -- all
[x] Doxygen          -- n/a
```

### ra_dma -- DMAC + DTC generic transfer (DMAC backend in Wave 1.3; DTC deferred to Wave 2.2)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init             -- HUM Ch 17 "DMA Controller (DMAC)" p 729
[x] Deinit           -- HUM Ch 17 p 729
[x] Polling TX       -- n/a
[x] Polling RX       -- n/a
[x] Interrupt TX     -- HUM Ch 17 p 729
[x] Interrupt RX     -- HUM Ch 17 p 729
[x] DMA TX           -- HUM Ch 17 p 729
[x] DMA RX           -- HUM Ch 17 p 729
[x] Error status     -- HUM Ch 17 p 729
[x] Runtime reconfig -- HUM Ch 17 p 729
[x] Power transition -- HUM Ch 17 p 729
[x] Register coverage-- HUM Ch 17 p 729
[x] Unit tests       -- n/a
[x] World tag        -- n/a
[x] HUM cross-ref    -- all
[x] Doxygen          -- n/a
```

---

## Wave 2 -- Foundation drivers

### ra_icu -- Interrupt Controller Unit (IRQCR + NMI extensions, legacy facade kept)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init             -- HUM Ch 14 "Interrupt Controller Unit (ICU)" p 524
[x] Deinit           -- HUM Ch 14 p 524
[x] Polling TX       -- n/a
[x] Polling RX       -- n/a
[x] Interrupt TX     -- HUM Ch 14 p 524
[x] Interrupt RX     -- HUM Ch 14 p 524
[x] DMA TX           -- n/a
[x] DMA RX           -- n/a
[x] Error status     -- HUM Ch 14 p 524
[x] Runtime reconfig -- HUM Ch 14 p 524
[x] Power transition -- HUM Ch 14 p 524
[x] Register coverage-- HUM Ch 14 p 524
[x] Unit tests       -- n/a
[x] World tag        -- n/a
[x] HUM cross-ref    -- all
[x] Doxygen          -- n/a
```

### ra_elc -- Event Link Controller (full rewrite)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init             -- HUM Ch 19 "Event Link Controller (ELC)" p 817
[x] Deinit           -- HUM Ch 19 p 817
[x] Polling TX       -- n/a
[x] Polling RX       -- n/a
[x] Interrupt TX     -- HUM Ch 19 p 817
[x] Interrupt RX     -- HUM Ch 19 p 817
[x] DMA TX           -- n/a
[x] DMA RX           -- n/a
[x] Error status     -- HUM Ch 19 p 817
[x] Runtime reconfig -- HUM Ch 19 p 817
[x] Power transition -- HUM Ch 19 p 817
[x] Register coverage-- HUM Ch 19 p 817
[x] Unit tests       -- n/a
[x] World tag        -- n/a
[x] HUM cross-ref    -- all
[x] Doxygen          -- n/a
```

### ra_dmac -- DMA Controller (refactor)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}` (Wave 1.1 migration + Wave 1.3 ra_dma substrate)

```
[x] Init             -- HUM Ch 17 "DMA Controller (DMAC)" p 729
[x] Deinit           -- HUM Ch 17 p 729
[x] Polling TX       -- n/a
[x] Polling RX       -- n/a
[x] Interrupt TX     -- HUM Ch 17 p 729
[x] Interrupt RX     -- HUM Ch 17 p 729
[x] DMA TX           -- HUM Ch 17 p 729
[x] DMA RX           -- HUM Ch 17 p 729
[x] Error status     -- HUM Ch 17 p 729
[x] Runtime reconfig -- HUM Ch 17 p 729
[x] Power transition -- HUM Ch 17 p 729
[x] Register coverage-- HUM Ch 17 p 729
[x] Unit tests       -- n/a
[x] World tag        -- n/a
[x] HUM cross-ref    -- all
[x] Doxygen          -- n/a
```

### ra_dtc -- Data Transfer Controller (MSTP + init done; advanced modes deferred to Wave 2.2b)

`[~]` Status: WIP. `[Ring 3 / HAL] {World: S}`

```
[x] Init             -- HUM Ch 18 "Data Transfer Controller (DTC)" p 784
[x] Deinit           -- HUM Ch 18 p 784
[x] Polling TX       -- n/a
[x] Polling RX       -- n/a
[ ] Interrupt TX     -- HUM Ch 18 p 784
[ ] Interrupt RX     -- HUM Ch 18 p 784
[ ] DMA TX           -- HUM Ch 18 p 784
[ ] DMA RX           -- HUM Ch 18 p 784
[x] Error status     -- HUM Ch 18 p 784
[ ] Runtime reconfig -- HUM Ch 18 p 784
[x] Power transition -- HUM Ch 18 p 784
[ ] Register coverage-- HUM Ch 18 p 784
[ ] Unit tests       -- n/a
[x] World tag        -- n/a
[x] HUM cross-ref    -- all
[x] Doxygen          -- n/a
```

### ra_cgc -- Clock Generation Circuit (runtime reconfigure + stop detection)

`[x]` Status: DONE. `[Ring 3 / HAL] {World: S}`

```
[x] Init             -- HUM Ch 9 "Clock Generation Circuit" p 317
[x] Deinit           -- HUM Ch 9 p 317
[x] Polling TX       -- n/a
[x] Polling RX       -- n/a
[x] Interrupt TX     -- HUM Ch 9 p 317
[x] Interrupt RX     -- HUM Ch 9 p 317
[x] DMA TX           -- n/a
[x] DMA RX           -- n/a
[x] Error status     -- HUM Ch 9 p 317
[x] Runtime reconfig -- HUM Ch 9 p 317
[x] Power transition -- HUM Ch 9 p 317
[x] Register coverage-- HUM Ch 9 p 317
[x] Unit tests       -- n/a
[x] World tag        -- n/a
[x] HUM cross-ref    -- all
[x] Doxygen          -- n/a
```

---

## Wave 3 -- Critical serial / parallel IO

### ra_sci -- Serial Communications Interface

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 38 "Serial Communications Interface (SCI)" p 2174
[ ] Deinit           -- HUM Ch 38 p 2174
[ ] Polling TX       -- HUM Ch 38 p 2174
[ ] Polling RX       -- HUM Ch 38 p 2174
[ ] Interrupt TX     -- HUM Ch 38 p 2174
[ ] Interrupt RX     -- HUM Ch 38 p 2174
[ ] DMA TX           -- HUM Ch 38 p 2174
[ ] DMA RX           -- HUM Ch 38 p 2174
[ ] Error status     -- HUM Ch 38 p 2174
[ ] Runtime reconfig -- HUM Ch 38 p 2174
[ ] Power transition -- HUM Ch 38 p 2174
[ ] Register coverage-- HUM Ch 38 p 2174
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_iic -- I2C Bus Interface

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 39 "I2C Bus Interface (IIC)" p 2367
[ ] Deinit           -- HUM Ch 39 p 2367
[ ] Polling TX       -- HUM Ch 39 p 2367
[ ] Polling RX       -- HUM Ch 39 p 2367
[ ] Interrupt TX     -- HUM Ch 39 p 2367
[ ] Interrupt RX     -- HUM Ch 39 p 2367
[ ] DMA TX           -- HUM Ch 39 p 2367
[ ] DMA RX           -- HUM Ch 39 p 2367
[ ] Error status     -- HUM Ch 39 p 2367
[ ] Runtime reconfig -- HUM Ch 39 p 2367
[ ] Power transition -- HUM Ch 39 p 2367
[ ] Register coverage-- HUM Ch 39 p 2367
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_spi -- Serial Peripheral Interface

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 43 "Serial Peripheral Interface (SPI)" p 2877
[ ] Deinit           -- HUM Ch 43 p 2877
[ ] Polling TX       -- HUM Ch 43 p 2877
[ ] Polling RX       -- HUM Ch 43 p 2877
[ ] Interrupt TX     -- HUM Ch 43 p 2877
[ ] Interrupt RX     -- HUM Ch 43 p 2877
[ ] DMA TX           -- HUM Ch 43 p 2877
[ ] DMA RX           -- HUM Ch 43 p 2877
[ ] Error status     -- HUM Ch 43 p 2877
[ ] Runtime reconfig -- HUM Ch 43 p 2877
[ ] Power transition -- HUM Ch 43 p 2877
[ ] Register coverage-- HUM Ch 43 p 2877
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_gpio -- I/O Ports + ra_gpio_attach_irq

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 20 "I/O Ports" p 837
[ ] Deinit           -- HUM Ch 20 p 837
[ ] Polling TX       -- HUM Ch 20 p 837
[ ] Polling RX       -- HUM Ch 20 p 837
[ ] Interrupt TX     -- HUM Ch 14 "Interrupt Controller Unit (ICU)" p 524
[ ] Interrupt RX     -- HUM Ch 14 p 524
[ ] DMA TX           -- n/a
[ ] DMA RX           -- n/a
[ ] Error status     -- HUM Ch 20 p 837
[ ] Runtime reconfig -- HUM Ch 20 p 837
[ ] Power transition -- HUM Ch 20 p 837
[ ] Register coverage-- HUM Ch 20 p 837
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_gpt -- General PWM Timer (full build-out)

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 22 "General PWM Timer (GPT)" p 878
[ ] Deinit           -- HUM Ch 22 p 878
[ ] Polling TX       -- HUM Ch 22 p 878
[ ] Polling RX       -- HUM Ch 22 p 878
[ ] Interrupt TX     -- HUM Ch 22 p 878
[ ] Interrupt RX     -- HUM Ch 22 p 878
[ ] DMA TX           -- HUM Ch 22 p 878
[ ] DMA RX           -- HUM Ch 22 p 878
[ ] Error status     -- HUM Ch 22 p 878
[ ] Runtime reconfig -- HUM Ch 22 p 878
[ ] Power transition -- HUM Ch 22 p 878
[ ] Register coverage-- HUM Ch 22 p 878
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_mtu -- Multi-Function Timer Pulse Unit (new)

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

(MTU lives in the GPT family in the RA8D2 register space; cite
chapter is identified at the start of the session via Read-tool
verification of HUM Ch 22 region.)

```
[ ] Init             -- HUM Ch 22 "General PWM Timer (GPT)" p 878
[ ] Deinit           -- HUM Ch 22 p 878
[ ] Polling TX       -- HUM Ch 22 p 878
[ ] Polling RX       -- HUM Ch 22 p 878
[ ] Interrupt TX     -- HUM Ch 22 p 878
[ ] Interrupt RX     -- HUM Ch 22 p 878
[ ] DMA TX           -- HUM Ch 22 p 878
[ ] DMA RX           -- HUM Ch 22 p 878
[ ] Error status     -- HUM Ch 22 p 878
[ ] Runtime reconfig -- HUM Ch 22 p 878
[ ] Power transition -- HUM Ch 22 p 878
[ ] Register coverage-- HUM Ch 22 p 878
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_tpu -- Timer Pulse Unit (new)

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 22 "General PWM Timer (GPT)" p 878
[ ] Deinit           -- HUM Ch 22 p 878
[ ] Polling TX       -- HUM Ch 22 p 878
[ ] Polling RX       -- HUM Ch 22 p 878
[ ] Interrupt TX     -- HUM Ch 22 p 878
[ ] Interrupt RX     -- HUM Ch 22 p 878
[ ] DMA TX           -- HUM Ch 22 p 878
[ ] DMA RX           -- HUM Ch 22 p 878
[ ] Error status     -- HUM Ch 22 p 878
[ ] Runtime reconfig -- HUM Ch 22 p 878
[ ] Power transition -- HUM Ch 22 p 878
[ ] Register coverage-- HUM Ch 22 p 878
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

---

## Wave 4 -- Analog, safety, time

### ra_adc -- 16-bit A/D Converter (ADC16H)

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308
[ ] Deinit           -- HUM Ch 53 p 3308
[ ] Polling TX       -- n/a
[ ] Polling RX       -- HUM Ch 53 p 3308
[ ] Interrupt TX     -- n/a
[ ] Interrupt RX     -- HUM Ch 53 p 3308
[ ] DMA TX           -- n/a
[ ] DMA RX           -- HUM Ch 53 p 3308
[ ] Error status     -- HUM Ch 53 p 3308
[ ] Runtime reconfig -- HUM Ch 53 p 3308
[ ] Power transition -- HUM Ch 53 p 3308
[ ] Register coverage-- HUM Ch 53 p 3308
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_dac_b -- 12-Bit D/A Converter (DAC12)

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 54 "12-Bit D/A Converter (DAC12)" p 3490
[ ] Deinit           -- HUM Ch 54 p 3490
[ ] Polling TX       -- HUM Ch 54 p 3490
[ ] Polling RX       -- n/a
[ ] Interrupt TX     -- n/a
[ ] Interrupt RX     -- n/a
[ ] DMA TX           -- HUM Ch 54 p 3490
[ ] DMA RX           -- n/a
[ ] Error status     -- HUM Ch 54 p 3490
[ ] Runtime reconfig -- HUM Ch 54 p 3490
[ ] Power transition -- HUM Ch 54 p 3490
[ ] Register coverage-- HUM Ch 54 p 3490
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_acmphs -- High-Speed Analog Comparator

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 56 "High-Speed Analog Comparator (ACMPHS)" p 3508
[ ] Deinit           -- HUM Ch 56 p 3508
[ ] Polling TX       -- n/a
[ ] Polling RX       -- HUM Ch 56 p 3508
[ ] Interrupt TX     -- n/a
[ ] Interrupt RX     -- HUM Ch 56 p 3508
[ ] DMA TX           -- n/a
[ ] DMA RX           -- n/a
[ ] Error status     -- HUM Ch 56 p 3508
[ ] Runtime reconfig -- HUM Ch 56 p 3508
[ ] Power transition -- HUM Ch 56 p 3508
[ ] Register coverage-- HUM Ch 56 p 3508
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_rtc -- Realtime Clock

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: S}`

```
[ ] Init             -- HUM Ch 26 "Realtime Clock (RTC)" p 1219
[ ] Deinit           -- HUM Ch 26 p 1219
[ ] Polling TX       -- n/a
[ ] Polling RX       -- HUM Ch 26 p 1219
[ ] Interrupt TX     -- n/a
[ ] Interrupt RX     -- HUM Ch 26 p 1219
[ ] DMA TX           -- n/a
[ ] DMA RX           -- n/a
[ ] Error status     -- HUM Ch 26 p 1219
[ ] Runtime reconfig -- HUM Ch 26 p 1219
[ ] Power transition -- HUM Ch 26 p 1219
[ ] Register coverage-- HUM Ch 26 p 1219
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_wdt -- Watchdog Timer

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: S}`

```
[ ] Init             -- HUM Ch 27 "Watchdog Timer (WDT)" p 1256
[ ] Deinit           -- HUM Ch 27 p 1256
[ ] Polling TX       -- n/a
[ ] Polling RX       -- n/a
[ ] Interrupt TX     -- n/a
[ ] Interrupt RX     -- HUM Ch 27 p 1256
[ ] DMA TX           -- n/a
[ ] DMA RX           -- n/a
[ ] Error status     -- HUM Ch 27 p 1256
[ ] Runtime reconfig -- HUM Ch 27 p 1256
[ ] Power transition -- HUM Ch 27 p 1256
[ ] Register coverage-- HUM Ch 27 p 1256
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_iwdt -- Independent Watchdog Timer

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: S}`

```
[ ] Init             -- HUM Ch 28 "Independent Watchdog Timer (IWDT)" p 1271
[ ] Deinit           -- HUM Ch 28 p 1271
[ ] Polling TX       -- n/a
[ ] Polling RX       -- n/a
[ ] Interrupt TX     -- n/a
[ ] Interrupt RX     -- HUM Ch 28 p 1271
[ ] DMA TX           -- n/a
[ ] DMA RX           -- n/a
[ ] Error status     -- HUM Ch 28 p 1271
[ ] Runtime reconfig -- HUM Ch 28 p 1271
[ ] Power transition -- HUM Ch 28 p 1271
[ ] Register coverage-- HUM Ch 28 p 1271
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_ulpt -- Ultra-Low-Power Timer

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 25 "Ultra-Low-Power Timer (ULPT)" p 1187
[ ] Deinit           -- HUM Ch 25 p 1187
[ ] Polling TX       -- n/a
[ ] Polling RX       -- n/a
[ ] Interrupt TX     -- n/a
[ ] Interrupt RX     -- HUM Ch 25 p 1187
[ ] DMA TX           -- n/a
[ ] DMA RX           -- n/a
[ ] Error status     -- HUM Ch 25 p 1187
[ ] Runtime reconfig -- HUM Ch 25 p 1187
[ ] Power transition -- HUM Ch 25 p 1187
[ ] Register coverage-- HUM Ch 25 p 1187
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_agt -- Low Power Asynchronous General Purpose Timer

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 24 "Low Power Asynchronous General Purpose Timer (AGT)" p 1164
[ ] Deinit           -- HUM Ch 24 p 1164
[ ] Polling TX       -- n/a
[ ] Polling RX       -- n/a
[ ] Interrupt TX     -- n/a
[ ] Interrupt RX     -- HUM Ch 24 p 1164
[ ] DMA TX           -- n/a
[ ] DMA RX           -- n/a
[ ] Error status     -- HUM Ch 24 p 1164
[ ] Runtime reconfig -- HUM Ch 24 p 1164
[ ] Power transition -- HUM Ch 24 p 1164
[ ] Register coverage-- HUM Ch 24 p 1164
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_cac -- Clock Frequency Accuracy Measurement Circuit

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 10 "Clock Frequency Accuracy Measurement Circuit (CAC)" p 420
[ ] Deinit           -- HUM Ch 10 p 420
[ ] Polling TX       -- n/a
[ ] Polling RX       -- HUM Ch 10 p 420
[ ] Interrupt TX     -- n/a
[ ] Interrupt RX     -- HUM Ch 10 p 420
[ ] DMA TX           -- n/a
[ ] DMA RX           -- n/a
[ ] Error status     -- HUM Ch 10 p 420
[ ] Runtime reconfig -- HUM Ch 10 p 420
[ ] Power transition -- HUM Ch 10 p 420
[ ] Register coverage-- HUM Ch 10 p 420
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_crc -- Cyclic Redundancy Check

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 48 "Cyclic Redundancy Check (CRC)" p 3180
[ ] Deinit           -- HUM Ch 48 p 3180
[ ] Polling TX       -- HUM Ch 48 p 3180
[ ] Polling RX       -- HUM Ch 48 p 3180
[ ] Interrupt TX     -- n/a
[ ] Interrupt RX     -- n/a
[ ] DMA TX           -- HUM Ch 48 p 3180
[ ] DMA RX           -- HUM Ch 48 p 3180
[ ] Error status     -- HUM Ch 48 p 3180
[ ] Runtime reconfig -- HUM Ch 48 p 3180
[ ] Power transition -- HUM Ch 48 p 3180
[ ] Register coverage-- HUM Ch 48 p 3180
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

---

## Wave 5 -- External memory and high-throughput buses

### ra_xspi -- Octal Serial Peripheral Interface (OSPI)

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: S}`

```
[ ] Init             -- HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986
[ ] Deinit           -- HUM Ch 44 p 2986
[ ] Polling TX       -- HUM Ch 44 p 2986
[ ] Polling RX       -- HUM Ch 44 p 2986
[ ] Interrupt TX     -- HUM Ch 44 p 2986
[ ] Interrupt RX     -- HUM Ch 44 p 2986
[ ] DMA TX           -- HUM Ch 44 p 2986
[ ] DMA RX           -- HUM Ch 44 p 2986
[ ] Error status     -- HUM Ch 44 p 2986
[ ] Runtime reconfig -- HUM Ch 44 p 2986
[ ] Power transition -- HUM Ch 44 p 2986
[ ] Register coverage-- HUM Ch 44 p 2986
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_sdramc -- SDRAM controller (Buses chapter)

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: S}`

```
[ ] Init             -- HUM Ch 15 "Buses" p 583
[ ] Deinit           -- HUM Ch 15 p 583
[ ] Polling TX       -- n/a
[ ] Polling RX       -- n/a
[ ] Interrupt TX     -- n/a
[ ] Interrupt RX     -- HUM Ch 15 p 583
[ ] DMA TX           -- n/a
[ ] DMA RX           -- n/a
[ ] Error status     -- HUM Ch 15 p 583
[ ] Runtime reconfig -- HUM Ch 15 p 583
[ ] Power transition -- HUM Ch 15 p 583
[ ] Register coverage-- HUM Ch 15 p 583
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_canfd -- CAN with Flexible Data-rate

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 41 "CAN with Flexible Data-rate (CANFD)" p 2702
[ ] Deinit           -- HUM Ch 41 p 2702
[ ] Polling TX       -- HUM Ch 41 p 2702
[ ] Polling RX       -- HUM Ch 41 p 2702
[ ] Interrupt TX     -- HUM Ch 41 p 2702
[ ] Interrupt RX     -- HUM Ch 41 p 2702
[ ] DMA TX           -- HUM Ch 41 p 2702
[ ] DMA RX           -- HUM Ch 41 p 2702
[ ] Error status     -- HUM Ch 41 p 2702
[ ] Runtime reconfig -- HUM Ch 41 p 2702
[ ] Power transition -- HUM Ch 41 p 2702
[ ] Register coverage-- HUM Ch 41 p 2702
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_sdhi -- SD/MMC Host Interface

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 47 "SD/MMC Host Interface (SDHI)" p 3122
[ ] Deinit           -- HUM Ch 47 p 3122
[ ] Polling TX       -- HUM Ch 47 p 3122
[ ] Polling RX       -- HUM Ch 47 p 3122
[ ] Interrupt TX     -- HUM Ch 47 p 3122
[ ] Interrupt RX     -- HUM Ch 47 p 3122
[ ] DMA TX           -- HUM Ch 47 p 3122
[ ] DMA RX           -- HUM Ch 47 p 3122
[ ] Error status     -- HUM Ch 47 p 3122
[ ] Runtime reconfig -- HUM Ch 47 p 3122
[ ] Power transition -- HUM Ch 47 p 3122
[ ] Register coverage-- HUM Ch 47 p 3122
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_i3c -- I3C Bus Interface

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 40 "I3C Bus Interface (I3C)" p 2445
[ ] Deinit           -- HUM Ch 40 p 2445
[ ] Polling TX       -- HUM Ch 40 p 2445
[ ] Polling RX       -- HUM Ch 40 p 2445
[ ] Interrupt TX     -- HUM Ch 40 p 2445
[ ] Interrupt RX     -- HUM Ch 40 p 2445
[ ] DMA TX           -- HUM Ch 40 p 2445
[ ] DMA RX           -- HUM Ch 40 p 2445
[ ] Error status     -- HUM Ch 40 p 2445
[ ] Runtime reconfig -- HUM Ch 40 p 2445
[ ] Power transition -- HUM Ch 40 p 2445
[ ] Register coverage-- HUM Ch 40 p 2445
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

---

## Wave 6 -- Display, audio, USB controllers, Ethernet MAC

### ra_glcdc -- Graphics LCD Controller

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 63 "Graphics LCD Controller (GLCDC)" p 3744
[ ] Deinit           -- HUM Ch 63 p 3744
[ ] Polling TX       -- n/a
[ ] Polling RX       -- n/a
[ ] Interrupt TX     -- HUM Ch 63 p 3744
[ ] Interrupt RX     -- HUM Ch 63 p 3744
[ ] DMA TX           -- HUM Ch 63 p 3744
[ ] DMA RX           -- n/a
[ ] Error status     -- HUM Ch 63 p 3744
[ ] Runtime reconfig -- HUM Ch 63 p 3744
[ ] Power transition -- HUM Ch 63 p 3744
[ ] Register coverage-- HUM Ch 63 p 3744
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_pdm -- Pulse Density Modulation Interface

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 49 "Pulse Density Modulation Interface (PDM-IF)" p 3190
[ ] Deinit           -- HUM Ch 49 p 3190
[ ] Polling TX       -- n/a
[ ] Polling RX       -- HUM Ch 49 p 3190
[ ] Interrupt TX     -- n/a
[ ] Interrupt RX     -- HUM Ch 49 p 3190
[ ] DMA TX           -- n/a
[ ] DMA RX           -- HUM Ch 49 p 3190
[ ] Error status     -- HUM Ch 49 p 3190
[ ] Runtime reconfig -- HUM Ch 49 p 3190
[ ] Power transition -- HUM Ch 49 p 3190
[ ] Register coverage-- HUM Ch 49 p 3190
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_usb_fs -- USB 2.0 Full-Speed Module

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 36 "USB 2.0 Full-Speed Module (USBFS)" p 1965
[ ] Deinit           -- HUM Ch 36 p 1965
[ ] Polling TX       -- HUM Ch 36 p 1965
[ ] Polling RX       -- HUM Ch 36 p 1965
[ ] Interrupt TX     -- HUM Ch 36 p 1965
[ ] Interrupt RX     -- HUM Ch 36 p 1965
[ ] DMA TX           -- HUM Ch 36 p 1965
[ ] DMA RX           -- HUM Ch 36 p 1965
[ ] Error status     -- HUM Ch 36 p 1965
[ ] Runtime reconfig -- HUM Ch 36 p 1965
[ ] Power transition -- HUM Ch 36 p 1965
[ ] Register coverage-- HUM Ch 36 p 1965
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_usb_hs -- USB 2.0 High-Speed Module

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 37 "USB 2.0 High-Speed Module (USBHS)" p 2059
[ ] Deinit           -- HUM Ch 37 p 2059
[ ] Polling TX       -- HUM Ch 37 p 2059
[ ] Polling RX       -- HUM Ch 37 p 2059
[ ] Interrupt TX     -- HUM Ch 37 p 2059
[ ] Interrupt RX     -- HUM Ch 37 p 2059
[ ] DMA TX           -- HUM Ch 37 p 2059
[ ] DMA RX           -- HUM Ch 37 p 2059
[ ] Error status     -- HUM Ch 37 p 2059
[ ] Runtime reconfig -- HUM Ch 37 p 2059
[ ] Power transition -- HUM Ch 37 p 2059
[ ] Register coverage-- HUM Ch 37 p 2059
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_eth_swm -- Layer 3 Ethernet Switch Module

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287
[ ] Deinit           -- HUM Ch 29 p 1287
[ ] Polling TX       -- n/a
[ ] Polling RX       -- n/a
[ ] Interrupt TX     -- HUM Ch 29 p 1287
[ ] Interrupt RX     -- HUM Ch 29 p 1287
[ ] DMA TX           -- HUM Ch 29 p 1287
[ ] DMA RX           -- HUM Ch 29 p 1287
[ ] Error status     -- HUM Ch 29 p 1287
[ ] Runtime reconfig -- HUM Ch 29 p 1287
[ ] Power transition -- HUM Ch 29 p 1287
[ ] Register coverage-- HUM Ch 29 p 1287
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_eth_mfwd -- Ethernet Message Forwarding Engine

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321
[ ] Deinit           -- HUM Ch 30 p 1321
[ ] Polling TX       -- n/a
[ ] Polling RX       -- n/a
[ ] Interrupt TX     -- HUM Ch 30 p 1321
[ ] Interrupt RX     -- HUM Ch 30 p 1321
[ ] DMA TX           -- HUM Ch 30 p 1321
[ ] DMA RX           -- HUM Ch 30 p 1321
[ ] Error status     -- HUM Ch 30 p 1321
[ ] Runtime reconfig -- HUM Ch 30 p 1321
[ ] Power transition -- HUM Ch 30 p 1321
[ ] Register coverage-- HUM Ch 30 p 1321
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_eth_coma -- Ethernet Common Agent

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590
[ ] Deinit           -- HUM Ch 31 p 1590
[ ] Polling TX       -- n/a
[ ] Polling RX       -- n/a
[ ] Interrupt TX     -- HUM Ch 31 p 1590
[ ] Interrupt RX     -- HUM Ch 31 p 1590
[ ] DMA TX           -- HUM Ch 31 p 1590
[ ] DMA RX           -- HUM Ch 31 p 1590
[ ] Error status     -- HUM Ch 31 p 1590
[ ] Runtime reconfig -- HUM Ch 31 p 1590
[ ] Power transition -- HUM Ch 31 p 1590
[ ] Register coverage-- HUM Ch 31 p 1590
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_eth_gwca -- Ethernet CPU Agent

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787
[ ] Deinit           -- HUM Ch 34 p 1787
[ ] Polling TX       -- n/a
[ ] Polling RX       -- n/a
[ ] Interrupt TX     -- HUM Ch 34 p 1787
[ ] Interrupt RX     -- HUM Ch 34 p 1787
[ ] DMA TX           -- HUM Ch 34 p 1787
[ ] DMA RX           -- HUM Ch 34 p 1787
[ ] Error status     -- HUM Ch 34 p 1787
[ ] Runtime reconfig -- HUM Ch 34 p 1787
[ ] Power transition -- HUM Ch 34 p 1787
[ ] Register coverage-- HUM Ch 34 p 1787
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

### ra_eth_gptp -- Ethernet Generic PTP Timer

`[ ]` Status: TODO. `[Ring 3 / HAL] {World: NS}`

```
[ ] Init             -- HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925
[ ] Deinit           -- HUM Ch 35 p 1925
[ ] Polling TX       -- n/a
[ ] Polling RX       -- n/a
[ ] Interrupt TX     -- HUM Ch 35 p 1925
[ ] Interrupt RX     -- HUM Ch 35 p 1925
[ ] DMA TX           -- n/a
[ ] DMA RX           -- n/a
[ ] Error status     -- HUM Ch 35 p 1925
[ ] Runtime reconfig -- HUM Ch 35 p 1925
[ ] Power transition -- HUM Ch 35 p 1925
[ ] Register coverage-- HUM Ch 35 p 1925
[ ] Unit tests       -- n/a
[ ] World tag        -- n/a
[ ] HUM cross-ref    -- all
[ ] Doxygen          -- n/a
```

---

## Wave 7 -- PAL + middleware integration

### ra_net_pal -- lwIP port glue

`[ ]` Status: TODO. `[Ring 4 / PAL] {World: NS}`

PAL bringup follows the lwIP `contrib/ports/unix/` shape: a
single-threaded `sys_arch.c`, an `ethernetif.c` that wraps the
Ring-3 `ra_eth_*` functions, and a project-facing
`ra_net_pal_init` API in `libs/ra_net_pal/inc/ra_net_pal.h`. No
14-checkbox items apply because this is not a peripheral driver;
the gates are: lwIP DHCP succeeds, ICMP echo reply observable,
no leaks under sustained TX.

### ra_usb_pal -- CherryUSB usb_dc port glue

`[ ]` Status: TODO. `[Ring 4 / PAL] {World: NS}`

Two `usb_dc_ra8d2_*.c` files implementing CherryUSB's port API
on top of `ra_usb_fs` and `ra_usb_hs`. Configuration is centralised
in `libs/ra_usb_pal/inc/usb_config.h`. Gates: CDC-ACM enumerates,
HID mouse moves cursor, MSC mounts, no controller stalls under
sustained transfer.

### ra_nsc -- NSC veneer scaffold

`[ ]` Status: TODO. `[Ring 4 / NSC] {World: NSC}`

Wave-9 deliverable, listed here so the location and naming are
discoverable. Veneer files: `ra_nsc_xspi.c`, `ra_nsc_eth.c`,
`ra_nsc_log.c`, `ra_nsc_periph_init.c`. Each veneer carries HUM
citations and a `@par TrustZone Safety:` Doxygen section
explaining what is validated and why the boundary is safe.

---

## Wave 8 -- Single-world integration demo + stabilisation

`[ ]` Status: TODO.

- [ ] `src/main.c` exercises the full Wave 7 stack concurrently.
- [ ] Coverage gap fix-up pass for any driver below 90 % / 90 %.
- [ ] Final ROADMAP audit + `cite_check.py --strict`.
- [ ] Doxygen zero-warning regression pass.

---

## Wave 9 -- TrustZone partitioning

`[ ]` Status: TODO.

- [ ] Session 9.1 -- TrustZone bring-up (SAU, linker, toolchain `-mcmse`, two-ELF emit decision).
- [ ] Session 9.2 -- NSC veneer scaffold + `ra_sim_world` host mock.
- [ ] Session 9.3 -- HAL retrofit -- comms (`ra_sci`, `ra_iic`, `ra_spi`, `ra_usb_*`).
- [ ] Session 9.4 -- HAL retrofit -- I/O (`ra_gpt`, `ra_mtu`, `ra_tpu`, `ra_adc`, `ra_dac_b`, `ra_acmphs`, `ra_crc`, `ra_glcdc`, `ra_pdm`, `ra_eth_*`).
- [ ] Session 9.5 -- Stack relocation + integration (`lwip` + CherryUSB + `src/main.c` move to NS).

---

## Wave 10 -- Secure-side application + key handling demo

`[ ]` Status: TODO.

- [ ] `src/secure_app/key_vault.c` -- Secure-only symmetric key store.
- [ ] `libs/ra_nsc/src/ra_nsc_key_vault.c` -- single SHA256-of-(key XOR challenge) veneer.
- [ ] `src/boot/secure_exception.c` -- Secure fault handler logging NS -> S violations.
- [ ] `src/main.c` NS demo: legitimate veneer call + deliberate out-of-bounds Secure read.
- [ ] `ra_sim_world` tests cover both happy path and violation path.
