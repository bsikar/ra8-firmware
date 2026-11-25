# ADR-0021: Emulator-driven external OSPI NOR and SDRAM sizing for RA8P1 (provisional development ceiling)

## Status

Accepted -- 2026-10-04 (recorded 2026-11-24). Records the RA8P1 external-memory
sizing policy under `RA8FW-660` and `RA8FW-666`. Final production part
densities and bus speeds will be appended as a follow-up revision to this ADR
once real translation workloads complete the emulator sweep (`RA8EMU-755`).

## Context

`ADR-0020` establishes on-device offline manga and webtoon translation on the
RA8P1 (`R7KA8P1KFLCAC`) using an Ethos-U55 NPU for text/bubble detection and
script-specific OCR (`.npub` blobs) alongside a freestanding Zig int8 Marian
NMT runtime (17-55 MiB per language pair plus per-script OCR models and TVOC
vocabularies).

While the existing EK-RA8D2 reference board populates 64 MiB of Octo-SPI NOR
(`0x80000000`) and 64 MiB of 32-bit SDRAM (`0x68000000`), RA8P1 production
external flash and RAM are unconstrained up front: the hardware BOM can scale
to whatever densities and speeds the measured workloads demand, subject to the
MCU's physical controller limits and battery/latency targets.

Picking fixed board part densities or bus speeds prematurely risks either
under-sizing multi-language model storage/activations or over-paying in BOM
cost, pin loading, and power draw before real inference traffic is measured.

## Decision

1. **Emulator-driven sizing methodology (`ra8_emulator sweep`):**
   External OSPI NOR and SDRAM densities and operating speeds for the RA8P1
   board are selected from `ra8_emulator sweep` external-memory fabric
   measurements (`RA8EMU-645` synthetic weight-streaming sweep before workloads
   exist; `RA8EMU-755` `--elf` sweep on real detector, OCR, NMT, and chapter-job
   images once integrated), not guessed up front. Final BOM densities will be
   recorded in a follow-up edit to this ADR from that report.

2. **Provisional development ceiling:**
   During firmware bring-up and workload measurement, targets assume generous
   controller-ceiling defaults so no pipeline stage runs out of space while
   collecting sizing data:
   - **External Octo-SPI NOR (models and assets):** up to **256 MiB** octal NOR
     on `OSPI0 CS0` (`0x80000000`, plus optional `CS1` `0x90000000` window up
     to 256 MiB per CS).
   - **External SDRAM (working memory):** up to the RA8P1 SDRAMC controller
     maximum (**128 MiB** at `0x68000000..0x6FFFFFFF`, vs. the 64 MiB currently
     fitted on EK-RA8D2).

3. **Respect RA8P1 hardware controller limits:**
   Every candidate configuration in the sizing matrix must stay within the
   documented electrical, bus-width, and address-window bounds of the RA8P1
   OSPI (`OSPI_B`) and SDRAM (`R_BUS.SDRAM`) controllers.

4. **Targets that define "enough":**
   The selected memory configuration must satisfy:
   - **Capacity headroom:** at least **25% headroom** above measured peak high-water
     usage across flash and SDRAM (rounded up to the smallest controller-supported
     power-of-two JEDEC device density).
   - **Interactive page-turn latency:** page decode plus precomputed TRN1 overlay
     rendering remains inside the reader's interactive page-turn latency budget.
   - **Background chapter translation time:** a full batched chapter translation
     finishes within its background time budget while on charge / idle schedule
     so pre-translated chapters stay ahead of reading speed.

## Hardware Controller Constraints (RA8P1 / R01DS0439EJ0130 & HUM Ch 5 / 15 / 44)

| Controller | Address Window | Supported Bus Widths & Modes | Clock & Throughput Limits |
|---|---|---|---|
| **OSPI (`OSPI0` / `OSPI1`, `OSPI_B`)** | `OSPI0`: up to **256 MiB** per CS (`CS0` `0x80000000`, `CS1` `0x90000000`). `OSPI1`: up to **128 MiB** per CS (`CS1` reserved for SiP Flash on SiP parts). Standard BGA289/303 exposes 2 units; BGA224 exposes 1 unit. | JESD251 Profiles 1.0 (xSPI / Octal SPI) & 2.0 (HyperBus / HyperRAM), QSPI; 1S-1S-1S, 1S-2S-2S, 2S-2S-2S, 1S-4S-4S, 4S-4S-4S, 4S-4D-4D, 8D-8D-8D; XiP with prefetch & write buffer + DOTF. | DS Table 2.65: `tPERIOD >= 6.00 ns` (**166.67 MHz** max) in SDR-with-`OM_DQS` or DDR (`VSCR_1`, `VCC/VCC2 >= 2.70 V`), `7.50 ns` (133.33 MHz) in `VSCR_2`; `16.67 ns` (60 MHz) / `20.00 ns` (50 MHz) SDR without `OM_DQS`. Up to **333 MB/s** raw transfer rate (`8D-8D-8D`, 166.67 MHz). |
| **SDRAMC (`R_BUS.SDRAM`)** | **128 MiB** contiguous (`0x68000000..0x6FFFFFFF`, single `SDCS#`; up to two 512 Mbit devices in 32-bit mode, 4 banks x up to 13 rows x up to 11 cols via `SDADR.MXC[1:0]`). | `DS Table 1.15`: **32-bit** (289/303-pin) and **16-bit** (224-pin) SDRAM external buses; `SDCCR.BSIZE[1:0]` supports 8/16/32-bit controller framing; CAS latency `CL2` / `CL3`. | DS Sec 2.3: `SDCLK = 8 to 133 MHz` (`BCLKA = SDCLK`; `BCLK = SDCLK` 8 to 125 MHz) when SDRAMC alone is active; `8 to 66 MHz` when CSC and SDRAMC run concurrently. Requires `VCC/VCC2 = 3.00 to 3.63 V` when 32-bit SDRAM is used (`ADR-0010`). |

*Note on emulator sweep grid:* `ra8_emulator sweep` evaluates 144 timing pairs
crossing OSPI `x1/x2/x4/x8` @ `41.67 / 83.33 / 166.67 MHz` against SDRAM
`x8/x16/x32` @ `66.5 / 133 MHz` with `CL2 / CL3`. Hardware part-availability
and pinout (DS Table 1.15) prune non-16/32-bit SDRAM rows when mapping sweep
results onto physical BGA289/BGA224 BOM candidates.

## Baseline Emulator Measurements

### 1. Synthetic Weight-Streaming Sweep (`RA8EMU-645`, `ra8-emulator` `20a64614`)

Default synthetic workload modeling Ethos-U55 burst weight reads (`16 MiB` from
OSPI) interleaved with activation reads/writes (`2 MiB` through SDRAM), 1 pass,
across 144 configurations (`ReleaseFast`):

- **High water:** Flash `16,777,216 B` (minimum with >= 25% headroom: **32 MiB**);
  SDRAM `2,097,152 B` (minimum with >= 25% headroom: **4 MiB**).
- **Fastest (`#71`: OSPI x8 @ 166.66 MHz, SDRAM x32 @ 133 MHz CL2):** `165.053 ms` per pass.
- **EK-RA8D2 profile (`#143`: OSPI x8 @ 166.66 MHz, SDRAM x32 @ 133 MHz CL3):** `166.888 ms` total (`8.388 ms` NPU compute, `2.000 ms` CPU compute, `156.499 ms` memory stall -- ~94% OSPI stall).
- **Half-clock / narrow SDRAM sensitivity (OSPI fixed @ x8 166.66 MHz):**
  SDRAM x32 @ 66.5 MHz CL2 (`#35`): `176.850 ms`; SDRAM x8 @ 66.5 MHz CL2 (`#11`): `224.035 ms`.
- **Slowest (`#72`: OSPI x1 @ 41.66 MHz, SDRAM x8 @ 66.5 MHz CL3):** `3499.525 ms`.

### 2. Guest ELF Baseline Sweep (`RA8EMU-755`, `ra8-emulator` `09d39165`, PR `#1679`)

Command: `ra8_emulator sweep --elf tests/fixtures/sizing/ext_stream.elf --stop-sym ext_stream_done 1`
(reads `64 KiB` from OSPI, writes to SDRAM, and reads back from SDRAM;
144 configurations completed in `58 s`, `ReleaseFast`):

- **High water:** Flash `65,536 B` (minimum with >= 25% headroom: **1 MiB**);
  SDRAM `65,536 B` (minimum with >= 25% headroom: **1 MiB**).
- **Fastest (`#71`: OSPI x8 @ 166.67 MHz, SDRAM x32 @ 133 MHz CL2):** `3.933 ms` (`1.933 ms` memory stall).
- **EK-RA8D2 profile (`#143`: OSPI x8 @ 166.67 MHz, SDRAM x32 @ 133 MHz CL3):** `4.195 ms` (`2.195 ms` memory stall).
- **Slowest (`#72`: OSPI x1 @ 41.67 MHz, SDRAM x8 @ 66.5 MHz CL3):** `21.202 ms` (`19.202 ms` memory stall); `#0` (same corner @ CL2): `20.710 ms`. Across OSPI `x1 @ 41.67 MHz`, all SDRAM configurations span `18.48 ms` to `21.20 ms`.

**Initial reading from both sweeps:** Weight-streaming and OSPI-to-SDRAM traffic
are heavily dominated by OSPI bus width and clock rate (~94% stall in the
synthetic NPU pass), whereas SDRAM CAS latency (`CL2` vs `CL3`) changes total
pass time by ~1% at x8/166.67 MHz and SDRAM bus width primarily matters when
OSPI bandwidth does not bottleneck or when activations/framebuffers scale.

*Caveat:* Per `RA8EMU-645` and `RA8EMU-755`, byte counts and high-water marks
are exact, while nanosecond/millisecond timings reflect candidate-part
datasheet models until physical RA8P1 hardware validation (`RA8FW-261`).
`ext_stream.elf` is a pre-model fixture and will be superseded by real detector,
OCR, Marian NMT, and chapter-job ELFs.

## Buffer Placement Policy

| Buffer / Artifact | Primary Residence | Access Pattern & Rationale |
|---|---|---|
| **Page framebuffers, decoded images, and tile caches** | External **SDRAM** (`0x68000000`) | Large decoded comic/manga/book rasters (`docs/formats/JOF.md`, `RA8FW-308`) and display scanout/overlay scratch exceed on-chip SRAM; reside in SDRAM behind 32-bit CPU/DRW access. |
| **Ethos-U55 NPU `RUNTIME` activations & scratch** | On-chip **System SRAM** (`0x22000000`, `1664 KiB` user SRAM) | 16-byte-aligned shared scratch arena (`libs/ra8_hal/inc/ra8_npu_loader.h`, `RA8FW-669`) alternates detector and OCR recognizers over AXI without allocating; overflow/batched tensors spill to SDRAM only if SRAM arena limits require it. |
| **Active chapter text, OCR spans, series glossary, and TRN1 sidecars** | Persistent on **SD / `ra8_fs`**; working buffers in **SDRAM** / **SRAM** | Loaded per background chapter job into caller-bounded SDRAM/SRAM arenas (`ADR-0020`); precomputed TRN1 sidecars are read per page turn without invoking the translator. |
| **`.npub` `BAKED` weights, int8 Marian NMT weights, and `TVOC` vocabularies** | **XIP in external Octo-SPI NOR (`OSPI0`)** by default; selective **SDRAM** staging only if sweep dictates | `BAKED` weights execute directly in place from OSPI XIP (`docs/formats/NPU1.md`, zero SRAM copy) up to the 256 MiB ceiling; int8 Marian weights stream from OSPI by default, or are copied into SDRAM during a chapter job only if real-ELF `RA8EMU-755` sweeps prove OSPI stall prevents meeting the chapter budget and SDRAM headroom permits. |

## Consequences

* **Positive:**
  * Development work (`RA8FW-669` multi-model NPU loader, NMT runtime, and chapter
    jobs) proceeds with clear controller boundaries (`256 MiB` OSPI, `128 MiB`
    SDRAM) and fixed buffer placement without premature BOM lock-in.
  * Every sizing choice for the final RA8P1 board schematic becomes reproducible
    via `ra8_emulator sweep --elf` against concrete `>= 25%` headroom and latency
    pass/fail gates.
* **Negative / Trade-offs:**
  * Final OSPI NOR and SDRAM part numbers remain provisional until detector, OCR,
    and NMT ELFs run through `ra8_emulator sweep --elf` and this ADR is updated.
  * OSPI dominates streaming latency (~94% stall at 16 MiB), so lower-cost QSPI
    or slow OSPI clocks are unlikely to meet NPU/NMT budgets unless weights fit
    SDRAM shadowing without starving page/display buffers.

## References

* `RA8FW-660` -- parent epic for offline manga and webtoon translation on RA8P1.
* `RA8FW-666` -- ADR tracking issue for RA8P1 external memory sizing.
* `RA8EMU-645` / `RA8EMU-755` -- emulator external-memory sizing sweep framework
  and baseline measurements (`ra8_emulator sweep`).
* `ADR-0010` -- SDRAM interface supply domain (`VCC`/`VCC2` 3.00-3.63 V envelope).
* `ADR-0020` -- On-device offline translation architecture (`docs/adr/0020-offline-on-device-translation.md`).
* `docs/reference/ra8p1-datasheet.pdf` (`R01DS0439EJ0130`), `docs/reference/ra8d2-hardware-user-manual.pdf`
  (`R01UH1065EJ0130`, Ch 5/15/44), and `docs/reference/ra8p1_vs_ra8d2.md`.
* `libs/ra8_hal/inc/ra8_npu_loader.h` and `docs/formats/NPU1.md`.
