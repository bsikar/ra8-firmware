# RA8P1 E-Reader Concurrent Workload Profile and Memory Placement Budget

Technical reference and memory placement architecture for the Renesas RA8P1
(`R7KA8P1KFLCAC`) dual-core e-reader platform (`RA8FW-259`). This document establishes
the durable memory placement plan and concurrent workload profile across all
on-chip tightly coupled memories, SRAM banks, caches, external SDRAM, and
external Octo-SPI NOR Flash.

The implementation is codified and tested by:
- Constants and validation functions: [`libs/ra8_board_ra8p1/inc/ra8_board_ra8p1_workloads.h`](../../libs/ra8_board_ra8p1/inc/ra8_board_ra8p1_workloads.h)
- Board memmap constants: [`libs/ra8_board_ra8p1/inc/ra8_board_memmap.h`](../../libs/ra8_board_ra8p1/inc/ra8_board_memmap.h)
- Hardware capacities and limits: [`libs/ra8_board_ra8p1/inc/ra8_board_ra8p1.h`](../../libs/ra8_board_ra8p1/inc/ra8_board_ra8p1.h)
- Automated unit test suite: [`tests/core/src/test_ra8p1_ereader_budget.c`](../../tests/core/src/test_ra8p1_ereader_budget.c)
- Host memmap regression suite: [`libs/ra8_board_ra8p1/tests/memmap_test.zig`](../../libs/ra8_board_ra8p1/tests/memmap_test.zig)

---

## 1. Hardware Memory Hierarchy & Reconciliation

The RA8P1 memory subsystem couples a primary Cortex-M85 (1 GHz), a secondary
Cortex-M33 (250 MHz), an Arm Ethos-U55 micro-NPU (up to 500 MHz, 256 8x8 MACs),
an external 32-bit SDRAM controller (`SDRAMC`), and a dual-channel Octo-SPI
controller (`OSPI_B`).

Capacities are cited from primary sources:
- **Datasheet**: Renesas RA8P1 Datasheet `R01DS0439EJ0130` Rev 1.30 (Table 1.15 p 11, Table 2.65 p 172)
- **HUM**: Renesas RA8P1 Hardware User's Manual `R01UH1064EJ0130` Rev 1.30 (Sec 2.1.1 pp 111-112, Sec 2.16.1.1 Table 2.34 p 160)
- **ADR-0020**: On-device offline translation as a background chapter job
- **ADR-0021**: Emulator-driven external OSPI NOR and SDRAM sizing for RA8P1

### Complete Memory Tier Capacity vs. Allocation Summary

| Memory Tier | Physical Base | Silicon Capacity | Supported Floor / Working Limit | Primary Subsystem Ownership |
|---|---|---|---|---|
| **M85 ITCM** | `0x0000_0000` | 128 KiB ECC (16x8 KiB) | **64 KiB** (`0x0001_0000`) | Vector table, fast blitters, font rasterizer, e-paper inner loop |
| **M85 DTCM** | `0x2000_0000` | 128 KiB ECC (16x8 KiB) | **64 KiB** (`0x0001_0000`) | Fast ISR stacks, ThreadX scheduler state, touch filter, waveform LUT |
| **M85 L1 Caches** | Core-integrated | 16 KiB I + 16 KiB D | 32 KiB total | Automatic acceleration of MRAM/SRAM/SDRAM execution & data |
| **M33 CTCM** | `0x2000_0000` (CPU1) | 64 KiB ECC | **64 KiB** | Cortex-M33 low-power supervisor firmware image (`cpu1_main.c`) |
| **M33 STCM** | `0x3000_0000` (CPU1) | 64 KiB ECC | **64 KiB** | Cortex-M33 private stacks, scheduler, sensor sampling buffers |
| **M33 Bus Caches** | Bus-integrated | 16 KiB C + 16 KiB S | 32 KiB total | Bus caches filtering MRAM and system bus fetches for CPU1 |
| **SRAM0** | `0x2200_0000` | 1024 KiB | **1024 KiB - 256 B** | M85 Secure OS, thread stacks, C runtime `.data`/`.bss`, VFS state |
| **NOINIT** | `0x220F_FF00` | 256 B | **256 B** | Warm-reset persistent crash-log record (`ra8_crashlog_t`) |
| **SRAM1** | `0x2210_0000` | 640 KiB | **640 KiB** | Non-Secure app, M33 run aperture, IPC mailbox, Ethos-U55 scratch |
| **External SDRAM** | `0x6800_0000` | 128 MiB (controller) | **64 MiB** (EK fitted) | Framebuffers, image tile cache, EPUB DOM reflow, NMT activations |
| **External OSPI Flash**| `0x8000_0000` | 256 MiB (per CS) | **128 MiB** (target density)| Marian NMT int8 weights, Ethos-U55 `.npub`, CJK/Latin typography |

---

## 2. On-Chip Memory Placement Plan

### 2.1 Tightly Coupled Memories (TCM Floor: 64 KiB each)

Per `RA8FW-336` and `libs/ra8_core/inc/ra8_device.h`, although the Cortex-M85 silicon
provides 128 KiB each of ITCM and DTCM, the software build explicitly enforces a
conservative **64 KiB floor** (`k_ra8_board_itcm_size` / `k_ra8_board_dtcm_size`).
Allocations stay strictly within this 64 KiB boundary:

- **ITCM Partitioning (64 KiB floor)**:
  - Vector Table & Core Relocations: `1 KiB`
  - Fast Glypher & Alpha-Blitter Inner Loop: `15 KiB`
  - E-Paper Refresh Waveform Driver Inner Loop: `8 KiB`
  - Helium Vector DSP Fixed-Point Decoders: `8 KiB`
  - **Allocated Total**: `32 KiB`
  - **Headroom**: `32 KiB` (50.0% uncommitted)

- **DTCM Partitioning (64 KiB floor)**:
  - Fast ISR & Fault Exception Stacks: `8 KiB`
  - ThreadX OS Timer & Dispatch Control Block: `4 KiB`
  - Touch Coordinate Ring Buffer & IIR Filter: `4 KiB`
  - Low-Latency M85 <-> M33 Mailbox Ring: `8 KiB`
  - Active E-Paper Temperature-Compensated Waveform LUT: `8 KiB`
  - **Allocated Total**: `32 KiB`
  - **Headroom**: `32 KiB` (50.0% uncommitted)

### 2.2 On-Chip System SRAM (1664 KiB Total)

On-chip SRAM is segmented into three hardware-protected zones:

1. **SRAM0 (`0x2200_0000..0x220F_FEFF`, 1024 KiB less 256 B)**:
   - Primary ThreadX kernel stacks & byte pools: `64 KiB`
   - Secure C-runtime `.data` and `.bss`: `48 KiB`
   - VFS mount table, file descriptors, directory walk cursors: `32 KiB`
   - Font catalogue & kerning table directory: `32 KiB`
   - Dynamic packet/packet-buffer heap: `128 KiB`
   - **Active Working Set**: `304 KiB`
   - **Headroom**: `720 KiB less 256 B` (70.3% free margin)

2. **NOINIT (`0x220F_FF00..0x220F_FFFF`, 256 B)**:
   - Dedicated uninitialized memory holding the warm-reset crash-log payload.

3. **SRAM1 (`0x2210_0000..0x2219_FFFF`, 640 KiB)**:
   - Cortex-M33 Secondary Core Private SRAM Aperture (`0x2219_0000`): `64 KiB`
   - Non-Secure Application Runtime `.data` and `.bss`: `64 KiB`
   - Cross-Core IPC Mailbox & Descriptors: `16 KiB`
   - **Ethos-U55 NPU Activation Scratch Arena**: `384 KiB`
     *(Keeps high-frequency intermediate convolution tensors in fast on-chip SRAM, avoiding SDRAM bus contention during bubble detection and OCR)*
   - Coherent DMA Bounce Buffers (SDHI1, USB-FS/HS, SPI): `112 KiB`
   - **Total SRAM1 Partitioned**: `640 KiB` (100.0% mapped)

---

## 3. External SDRAM Working Memory Placement (64 MiB Baseline)

External 32-bit SDRAM (`0x6800_0000`, 133 MHz, 532 MB/s theoretical bandwidth)
serves as the shared working store for the four concurrent e-reader pipelines:

```
0x6800_0000 +-------------------------------------------------------+ 0 MiB
            | Region 1: Display & Framebuffers                      |
            | Front: 2.06M | Back: 2.06M | Overlay: 2.06M | Wave: 1.8M| 8 MiB
0x6880_0000 +-------------------------------------------------------+ 8 MiB
            | Region 2: Image Decompression & Manga Streaming       |
            | Page Cache: 256K | Tile: 192K | Strip: 4M | Canvas: 3.56M| 8 MiB
0x6900_0000 +-------------------------------------------------------+ 16 MiB
            | Region 3: Book Engine & Reflow Arena                  |
            | EPUB Zip: 4M | DOM Nodes: 4M | Glyph: 4M | VFS: 4M    | 16 MiB
0x6A00_0000 +-------------------------------------------------------+ 32 MiB
            | Region 4: On-Device ML & Translation Working Space    |
            | Marian NMT Int8 Activations & KV Cache: 24 MiB        |
            | Ethos-U55 Full-Page Detection Feature Maps: 8 MiB     | 32 MiB
0x6C00_0000 +=======================================================+ 64 MiB
            | Optional Expansion Area (up to 128 MiB controller cap)|
            | High-res colour manga, multi-turn translation cache   | +64 MiB
0x7000_0000 +-------------------------------------------------------+ 128 MiB
```

### Detailed Breakdown

1. **Region 1: Display & Framebuffers (8 MiB @ `0x6800_0000`)**:
   - `k_ra8p1_sdram_disp_front_bytes` (`2.06 MiB`): 1264 x 1680 @ 8 bpp grayscale front display buffer.
   - `k_ra8p1_sdram_disp_back_bytes` (`2.06 MiB`): Back display buffer for differential update computation.
   - `k_ra8p1_sdram_disp_overlay_bytes` (`2.06 MiB`): Compositing plane for translated text bubble overlays.
   - `k_ra8p1_sdram_disp_staging_bytes` (`1.81 MiB`): E-paper controller waveform staging and ping-pong buffers.

2. **Region 2: Image Decompression & Manga Streaming (8 MiB @ `0x6880_0000`)**:
   - `k_ra8p1_sdram_manga_page_cache_b` (`256 KiB`): 64-frame two-tier streaming page cache (`RA8FW-257`).
   - `k_ra8p1_sdram_manga_tile_cache_b` (`192 KiB`): 4-cell SLRU tile cache for high-speed panning (`RA8FW-257`).
   - `k_ra8p1_sdram_img_strip_decomp_b` (`4.00 MiB`): Streaming JPEG/WebP strip decompression buffer.
   - `k_ra8p1_sdram_canvas_double_buf_b` (`3.56 MiB`): Double-buffered uncompressed full-page canvas.

3. **Region 3: Book Engine & Reflow Arena (16 MiB @ `0x6900_0000`)**:
   - `k_ra8p1_sdram_epub_container_b` (`4.00 MiB`): Streamed EPUB zip container and XML parsing scratchpad.
   - `k_ra8p1_sdram_reflow_nodes_b` (`4.00 MiB`): Dynamic layout DOM node tree and line break arena.
   - `k_ra8p1_sdram_glyph_cache_b` (`4.00 MiB`): Pre-rendered anti-aliased font glyph cache pool.
   - `k_ra8p1_sdram_vfs_cluster_buf_b` (`4.00 MiB`): MicroSD card / SDHI blockdev cluster cache.

4. **Region 4: On-Device ML & Translation Workspace (32 MiB @ `0x6A00_0000`)**:
   - `k_ra8p1_sdram_marian_act_arena_b` (`24.00 MiB`): Marian NMT int8 encoder-decoder activation tensors, beam search state, and key-value attention workspace (`ADR-0020`).
   - `k_ra8p1_sdram_npu_feature_maps_b` (`8.00 MiB`): Ethos-U55 full-page text detection feature maps and bounding box candidate arrays.

---

## 4. External OSPI NOR Flash Asset Placement (CS0 @ `0x8000_0000`)

Model weights and immutable typography assets reside in Octo-SPI NOR Flash
operating at 166.67 MHz DDR (`8D-8D-8D`, 333 MB/s raw read throughput):

- **Marian NMT Int8 Weights (`.tpak` / `.trn1`)**: `55.00 MiB` (maximum single active language pair)
- **Ethos-U55 Text/Bubble Detector (`.npub`)**: `2.50 MiB`
- **Ethos-U55 Script-Specific CTC OCR (`.npub`)**: `4.50 MiB`
- **Noto Sans CJK Typography Package**: `12.00 MiB`
- **Literata Latin-1 Typography Package**: `1.25 MiB`
- **UI Icons, Chrome Themes, Localizations**: `2.00 MiB`
- **Total CS0 Allocation**: `78.31 MiB`
- **Headroom on standard 128 MiB NOR Flash**: `49.69 MiB` (38.8% uncommitted)

---

## 5. Concurrent Workload Profiles & Bandwidth Sizing

### 5.1 Concurrency Operational States

The system transitions between five explicit operational states:

1. **`k_ra8p1_state_screen_off_hold`**: M85 core asleep (WFI), SDRAM in low-power self-refresh (< 2 mW). Cortex-M33 active at 50/250 MHz monitoring touch and battery status.
2. **`k_ra8p1_state_reading_interactive`**: Foreground page flip, EPUB reflow, font blitting, and e-paper display refresh. Active bandwidth: ~45 MB/s.
3. **`k_ra8p1_state_manga_streaming`**: Rapid 60 FPS pan and zoom, SLRU tile cache eviction, strip decompressors active. Active bandwidth: ~110 MB/s.
4. **`k_ra8p1_state_background_translate`**: Background chapter job executing Ethos-U55 bubble detection, CTC OCR, and Marian NMT int8 inference. Active bandwidth: ~125 MB/s.
5. **`k_ra8p1_state_concurrent_peak`**: Simultaneous interactive reading while background chapter translation processes queued downloads. Active bandwidth: ~180 MB/s.

### 5.2 Contention Management & Bandwidth Headroom (ADR-0021)

Under worst-case peak concurrent operation (`k_ra8p1_state_concurrent_peak`), simultaneous
demands on the SDRAM bus are arbitrated to preserve the interactive page-turn latency budget:

- **Bus Capacity**: 133 MHz x 32-bit SDRAM provides `532 MB/s` raw bandwidth.
- **Throttled Peak Traffic**: `180 MB/s` total:
  - Display refresh / framebuffer blitting: `45 MB/s` (highest priority)
  - Tile / strip decompression: `35 MB/s`
  - Marian NMT activation read/write: `100 MB/s` (rate-limited via task yield)
- **Bus Utilization**: `180 / 532 = 33.8%`.
- **Bandwidth Headroom**: **66.2% margin**, comfortably exceeding the mandatory **>= 25% headroom** requirement codified in `ADR-0021 Section 4`.

### 5.3 Quality-of-Service (QoS) Guarantees

1. **Interactive Priority**: Any user touch event (`er_input_active`) immediately signals the background Marian NMT thread to pause weight streaming and tensor updates for the duration of the touch gesture.
2. **NPU Isolation**: Ethos-U55 scratch buffers are restricted to `SRAM1` (`0x2210_0000`), ensuring NPU activation traffic never contends with the CPU display refresh on the external SDRAM bus.
3. **Coherency**: Framebuffers and DMA bounce buffers are mapped with MPU Shareable/Non-Cacheable attributes, eliminating D-cache flush/invalidate overhead on display and SDHI transfers.
