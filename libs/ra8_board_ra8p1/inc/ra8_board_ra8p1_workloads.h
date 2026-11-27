/**
 * @file ra8_board_ra8p1_workloads.h
 * @brief RA8P1 e-reader concurrent workload profile and memory placement budget
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * This header defines the authoritative memory placement plan and concurrent
 * workload budget for the RA8P1 (R7KA8P1KFLCAC) e-reader target (RA8FW-259).
 * It models the memory requirements across all hardware tiers (M85 ITCM/DTCM,
 * L1 caches, M33 CTCM/STCM, bus caches, on-chip SRAM0/SRAM1, external SDRAM,
 * and external OSPI NOR) for four concurrent/interleaved workload subsystems:
 *
 * 1. **Interactive Reader**: EPUB reflow, font rasterization, UI, e-paper refresh.
 * 2. **Manga/Comic Streamer**: Two-tier SLRU page/tile cache, strip decompressors.
 * 3. **Background Chapter Translation**: Ethos-U55 NPU detection/OCR, Marian NMT int8.
 * 4. **Dual-Core Low-Power Supervisor**: Cortex-M33 background hold, touch/battery monitor.
 *
 * Hardware limits reconciled against:
 * - RA8P1 Datasheet R01DS0439EJ0130 Rev 1.30 (Table 1.15 p 11, Table 2.65 p 172)
 * - RA8P1 Hardware User's Manual R01UH1064EJ0130 Rev 1.30 (Ch 2.1.1, Ch 2.16.1.1, Ch 5)
 * - ADR-0020 ("On-device offline translation as a background chapter job")
 * - ADR-0021 ("Emulator-driven external OSPI NOR and SDRAM sizing for RA8P1")
 * - `libs/ra8_core/inc/ra8_device.h` (RA8FW-336 reconciliation)
 * - `libs/ra8_board_ra8p1/inc/ra8_board_memmap.h` and `inc/ra8_board_ra8p1.h`
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_board_memmap.h"
#include "ra8_board_ra8p1.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Hardware Memory Capacities & Supported Floors
 * =============================================================================
 */

/**
 * @enum ra8p1_mem_tier_cap_t
 * @brief Byte capacities and supported allocation floors for RA8P1 memory tiers.
 */
typedef enum : uint32_t {
  /* On-chip tightly coupled memories */
  k_ra8p1_mem_m85_itcm_cap_bytes   = 0x00020000UL, /**< 128 KiB M85 ITCM silicon cap.        */
  k_ra8p1_mem_m85_itcm_floor_bytes = 0x00010000UL, /**< 64 KiB ITCM supported linker floor.  */
  k_ra8p1_mem_m85_dtcm_cap_bytes   = 0x00020000UL, /**< 128 KiB M85 DTCM silicon cap.        */
  k_ra8p1_mem_m85_dtcm_floor_bytes = 0x00010000UL, /**< 64 KiB DTCM supported linker floor.  */
  k_ra8p1_mem_m33_ctcm_cap_bytes   = 0x00010000UL, /**< 64 KiB M33 CTCM silicon cap.         */
  k_ra8p1_mem_m33_stcm_cap_bytes   = 0x00010000UL, /**< 64 KiB M33 STCM silicon cap.         */

  /* L1 and bus caches */
  k_ra8p1_mem_m85_icache_bytes     = 0x00004000UL, /**< 16 KiB M85 L1 instruction cache.     */
  k_ra8p1_mem_m85_dcache_bytes     = 0x00004000UL, /**< 16 KiB M85 L1 data cache.            */
  k_ra8p1_mem_m33_ccache_bytes     = 0x00004000UL, /**< 16 KiB M33 code bus cache.          */
  k_ra8p1_mem_m33_scache_bytes     = 0x00004000UL, /**< 16 KiB M33 system bus cache.        */

  /* On-chip system SRAM island */
  k_ra8p1_mem_sram_island_bytes    = 0x00200000UL, /**< 2048 KiB total SRAM island.          */
  k_ra8p1_mem_user_sram_cap_bytes  = 0x001A0000UL, /**< 1664 KiB user SRAM (island - TCMs).  */
  k_ra8p1_mem_sram0_limit_bytes    = 0x000FFF00UL, /**< 1024 KiB less 256 B NOINIT.          */
  k_ra8p1_mem_noinit_bytes         = 0x00000100UL, /**< 256 B warm-reset crash-log buffer.  */
  k_ra8p1_mem_sram1_limit_bytes    = 0x000A0000UL, /**< 640 KiB SRAM1 / shared non-secure.   */

  /* External fabrics */
  k_ra8p1_mem_sdram_ek_base_bytes  = 0x04000000UL, /**< 64 MiB SDRAM baseline (EK fitted).   */
  k_ra8p1_mem_sdram_max_cap_bytes  = 0x08000000UL, /**< 128 MiB SDRAMC controller ceiling.   */
  k_ra8p1_mem_ospi_cs_max_bytes    = 0x10000000UL, /**< 256 MiB per-CS OSPI addressing max.  */
} ra8p1_mem_tier_cap_t;

/* =============================================================================
 * On-Chip Memory Placement Budgets
 * =============================================================================
 */

/**
 * @enum ra8p1_tcm_budget_t
 * @brief Memory allocation across M85 ITCM and DTCM (64 KiB floor).
 */
typedef enum : uint32_t {
  /* ITCM (64 KiB floor) */
  k_ra8p1_itcm_vector_table_bytes   = 0x00000400UL, /**< 1 KiB M85 primary vector table.      */
  k_ra8p1_itcm_fast_raster_bytes    = 0x00003C00UL, /**< 15 KiB fast blit/font raster code.   */
  k_ra8p1_itcm_waveform_loop_bytes  = 0x00002000UL, /**< 8 KiB e-paper driver inner loops.    */
  k_ra8p1_itcm_dsp_kernels_bytes    = 0x00002000UL, /**< 8 KiB Helium vector DSP math kernel. */
  k_ra8p1_itcm_allocated_bytes      = 0x00008000UL, /**< 32 KiB total ITCM allocated.         */
  k_ra8p1_itcm_headroom_bytes       = 0x00008000UL, /**< 32 KiB ITCM headroom (50.0% free).   */

  /* DTCM (64 KiB floor) */
  k_ra8p1_dtcm_fast_stack_bytes     = 0x00002000UL, /**< 8 KiB fast ISR & exception stack.    */
  k_ra8p1_dtcm_os_tick_state_bytes  = 0x00001000UL, /**< 4 KiB ThreadX timer/scheduler state. */
  k_ra8p1_dtcm_touch_filter_bytes   = 0x00001000UL, /**< 4 KiB touch coordinate filter/ring.  */
  k_ra8p1_dtcm_ipc_ring_bytes       = 0x00002000UL, /**< 8 KiB low-latency M85<->M33 mailbox. */
  k_ra8p1_dtcm_waveform_lut_bytes   = 0x00002000UL, /**< 8 KiB active e-paper waveform LUT.   */
  k_ra8p1_dtcm_allocated_bytes      = 0x00008000UL, /**< 32 KiB total DTCM allocated.         */
  k_ra8p1_dtcm_headroom_bytes       = 0x00008000UL, /**< 32 KiB DTCM headroom (50.0% free).   */
} ra8p1_tcm_budget_t;

/**
 * @enum ra8p1_sram0_budget_t
 * @brief Memory allocation across SRAM0 (1024 KiB less 256 B NOINIT).
 */
typedef enum : uint32_t {
  k_ra8p1_sram0_threadx_pools_bytes = 0x00010000UL, /**< 64 KiB OS pools & thread stacks.     */
  k_ra8p1_sram0_secure_data_bytes   = 0x0000C000UL, /**< 48 KiB C-runtime .data and .bss.     */
  k_ra8p1_sram0_vfs_handles_bytes   = 0x00008000UL, /**< 32 KiB VFS mount/dir walk state.     */
  k_ra8p1_sram0_font_meta_bytes     = 0x00008000UL, /**< 32 KiB font catalogue & metadata.   */
  k_ra8p1_sram0_dynamic_heap_bytes  = 0x00020000UL, /**< 128 KiB heap for transient packets.  */
  k_ra8p1_sram0_allocated_bytes     = 0x0004C000UL, /**< 304 KiB total SRAM0 working set.     */
  k_ra8p1_sram0_headroom_bytes      = 0x000B3F00UL, /**< 720 KiB less 256 B free (70.3%).     */
} ra8p1_sram0_budget_t;

/**
 * @enum ra8p1_sram1_budget_t
 * @brief Memory allocation across SRAM1 (640 KiB Non-Secure / Shared Aperture).
 */
typedef enum : uint32_t {
  k_ra8p1_sram1_cpu1_m33_run_bytes  = 0x00010000UL, /**< 64 KiB CPU1 (M33) private run window. */
  k_ra8p1_sram1_ns_app_data_bytes   = 0x00010000UL, /**< 64 KiB NS application .data and .bss. */
  k_ra8p1_sram1_ipc_mailbox_bytes   = 0x00004000UL, /**< 16 KiB cross-core shared descriptors. */
  k_ra8p1_sram1_npu_scratch_bytes   = 0x00060000UL, /**< 384 KiB Ethos-U55 activation arena.  */
  k_ra8p1_sram1_dma_bounce_bytes    = 0x0001C000UL, /**< 112 KiB SDHI/USB/SPI coherent bounce. */
  k_ra8p1_sram1_allocated_bytes     = 0x000A0000UL, /**< 640 KiB fully partitioned (100.0%).  */
} ra8p1_sram1_budget_t;

/* =============================================================================
 * External SDRAM Memory Placement Budgets (64 MiB Baseline)
 * =============================================================================
 */

/**
 * @enum ra8p1_sdram_budget_t
 * @brief External SDRAM working memory partitioning (Base 0x68000000).
 */
typedef enum : uint32_t {
  /* Region 1: Display & Compositing Framebuffers (8 MiB @ 0x68000000) */
  k_ra8p1_sdram_disp_front_bytes    = 0x00210000UL, /**< 2.06 MiB front 1264x1680 8bpp frame. */
  k_ra8p1_sdram_disp_back_bytes     = 0x00210000UL, /**< 2.06 MiB back diff accumulation buf. */
  k_ra8p1_sdram_disp_overlay_bytes  = 0x00210000UL, /**< 2.06 MiB translation overlay buffer. */
  k_ra8p1_sdram_disp_staging_bytes  = 0x001D0000UL, /**< 1.81 MiB e-paper waveform staging.   */
  k_ra8p1_sdram_display_total_bytes = 0x00800000UL, /**< 8 MiB total display subsystem.       */

  /* Region 2: Image Decoding & Manga Stream Cache (8 MiB @ 0x68800000) */
  k_ra8p1_sdram_manga_page_cache_b  = 0x00040000UL, /**< 256 KiB 64-frame SLRU page cache.    */
  k_ra8p1_sdram_manga_tile_cache_b  = 0x00030000UL, /**< 192 KiB 4-cell SLRU tile cache.      */
  k_ra8p1_sdram_img_strip_decomp_b  = 0x00400000UL, /**< 4 MiB JPEG/WebP strip decompressor.  */
  k_ra8p1_sdram_canvas_double_buf_b = 0x00390000UL, /**< 3.56 MiB uncompressed canvas buffer. */
  k_ra8p1_sdram_image_total_bytes   = 0x00800000UL, /**< 8 MiB total image/manga subsystem.   */

  /* Region 3: Book Engine Working Memory (16 MiB @ 0x69000000) */
  k_ra8p1_sdram_epub_container_b    = 0x00400000UL, /**< 4 MiB streamed EPUB zip/xml buffer.  */
  k_ra8p1_sdram_reflow_nodes_b      = 0x00400000UL, /**< 4 MiB layout DOM node tree arena.    */
  k_ra8p1_sdram_glyph_cache_b       = 0x00400000UL, /**< 4 MiB rendered glyph cache pool.     */
  k_ra8p1_sdram_vfs_cluster_buf_b   = 0x00400000UL, /**< 4 MiB SDHI blockdev cluster cache.   */
  k_ra8p1_sdram_book_total_bytes    = 0x01000000UL, /**< 16 MiB total book/reflow subsystem.  */

  /* Region 4: ML & Translation Working Space (32 MiB @ 0x6A000000) */
  k_ra8p1_sdram_marian_act_arena_b  = 0x01800000UL, /**< 24 MiB Marian NMT activations / KV.  */
  k_ra8p1_sdram_npu_feature_maps_b  = 0x00800000UL, /**< 8 MiB NPU full-page detection maps.  */
  k_ra8p1_sdram_ml_total_bytes      = 0x02000000UL, /**< 32 MiB total on-device ML subsystem. */

  /* Total SDRAM allocation */
  k_ra8p1_sdram_allocated_bytes     = 0x04000000UL, /**< 64 MiB total allocated on baseline.  */
  k_ra8p1_sdram_ceiling_expansion_b = 0x04000000UL, /**< 64 MiB expansion to 128 MiB ceiling. */
} ra8p1_sdram_budget_t;

/* =============================================================================
 * External OSPI NOR Flash Asset Placement Budgets
 * =============================================================================
 */

/**
 * @enum ra8p1_ospi_budget_t
 * @brief External OSPI NOR Flash model and asset placement (Base 0x80000000).
 */
typedef enum : uint32_t {
  k_ra8p1_ospi_marian_weights_bytes = 0x03700000UL, /**< 55 MiB max per active NMT pair.     */
  k_ra8p1_ospi_npu_detect_blob_b    = 0x00280000UL, /**< 2.5 MiB text/bubble detector .npub.  */
  k_ra8p1_ospi_npu_ocr_blob_bytes   = 0x00480000UL, /**< 4.5 MiB script-specific CTC OCR blob.*/
  k_ra8p1_ospi_cjk_font_pack_bytes  = 0x00C00000UL, /**< 12 MiB Noto Sans CJK int8 font.     */
  k_ra8p1_ospi_latin_font_bytes     = 0x00140000UL, /**< 1.25 MiB Literata Latin-1 subset.   */
  k_ra8p1_ospi_ui_assets_bytes      = 0x00200000UL, /**< 2 MiB icons, themes, localizations.  */
  k_ra8p1_ospi_cs0_allocated_bytes  = 0x04E50000UL, /**< 78.31 MiB total CS0 allocation.      */
  k_ra8p1_ospi_cs0_128m_headroom_b  = 0x031B0000UL, /**< 49.69 MiB free on 128 MiB flash (38%).*/
} ra8p1_ospi_budget_t;

/* =============================================================================
 * Concurrency States & Bandwidth Budget Limits
 * =============================================================================
 */

/**
 * @enum ra8p1_concurrency_state_t
 * @brief High-level operational concurrency states for the RA8P1 e-reader.
 */
typedef enum : uint8_t {
  k_ra8p1_state_screen_off_hold      = 0U, /**< M85 sleep, M33 low-power monitor.     */
  k_ra8p1_state_reading_interactive  = 1U, /**< Active page-turn, reflow, e-paper.    */
  k_ra8p1_state_manga_streaming      = 2U, /**< Fast pan/zoom, two-tier tile cache.   */
  k_ra8p1_state_background_translate = 3U, /**< Batch chapter translation in progress.*/
  k_ra8p1_state_concurrent_peak      = 4U, /**< Reading + Background Translation concurrent. */
} ra8p1_concurrency_state_t;

/**
 * @enum ra8p1_bandwidth_limits_t
 * @brief Peak theoretical and measured bandwidth limits (MB/s).
 */
typedef enum : uint32_t {
  k_ra8p1_sdram_raw_bw_mbps       = 532U, /**< 133 MHz x 32-bit SDRAM raw max (MB/s). */
  k_ra8p1_ospi_raw_bw_mbps        = 333U, /**< 166.67 MHz DDR 8-bit raw max (MB/s).    */
  k_ra8p1_bw_disp_refresh_mbps    = 45U,  /**< 45 MB/s during 60 ms page turn refresh. */
  k_ra8p1_bw_manga_pan_mbps       = 110U, /**< 110 MB/s during 60 FPS continuous pan.  */
  k_ra8p1_bw_nmt_inference_mbps   = 125U, /**< 125 MB/s Marian NMT weight streaming.   */
  k_ra8p1_bw_concurrent_peak_mbps = 180U, /**< 180 MB/s throttled mixed peak traffic. */
} ra8p1_bandwidth_limits_t;

/* =============================================================================
 * Programmatic Budget Validation Helpers
 * =============================================================================
 */

/**
 * @brief Compute total allocated SRAM0 working memory in bytes.
 */
static inline uint32_t ra8p1_workload_get_sram0_bytes(void)
{
  return (uint32_t)k_ra8p1_sram0_allocated_bytes;
}

/**
 * @brief Compute total allocated SRAM1 shared aperture memory in bytes.
 */
static inline uint32_t ra8p1_workload_get_sram1_bytes(void)
{
  return (uint32_t)k_ra8p1_sram1_allocated_bytes;
}

/**
 * @brief Compute total allocated on-chip user SRAM across SRAM0 and SRAM1.
 */
static inline uint32_t ra8p1_workload_get_total_user_sram_bytes(void)
{
  return (uint32_t)k_ra8p1_sram0_allocated_bytes +
         (uint32_t)k_ra8p1_mem_noinit_bytes +
         (uint32_t)k_ra8p1_sram1_allocated_bytes;
}

/**
 * @brief Compute total allocated external SDRAM in bytes.
 */
static inline uint32_t ra8p1_workload_get_sdram_bytes(void)
{
  return (uint32_t)k_ra8p1_sdram_display_total_bytes +
         (uint32_t)k_ra8p1_sdram_image_total_bytes +
         (uint32_t)k_ra8p1_sdram_book_total_bytes +
         (uint32_t)k_ra8p1_sdram_ml_total_bytes;
}

/**
 * @brief Compute total allocated external OSPI NOR flash assets on CS0.
 */
static inline uint32_t ra8p1_workload_get_ospi_cs0_bytes(void)
{
  return (uint32_t)k_ra8p1_ospi_cs0_allocated_bytes;
}

/**
 * @brief Validate that all memory placement allocations stay strictly within
 *        hardware controller and board limits.
 *
 * @return k_ra8_ok on success.
 * @retval k_ra8_err_invalid_state if any allocation exceeds bank capacity.
 */
static inline ra8_err_t ra8p1_workload_validate_budget(void)
{
  /* Validate ITCM and DTCM stay within supported 64 KiB floor */
  if ((uint32_t)k_ra8p1_itcm_allocated_bytes > (uint32_t)k_ra8p1_mem_m85_itcm_floor_bytes) {
    return k_ra8_err_invalid_state;
  }
  if ((uint32_t)k_ra8p1_dtcm_allocated_bytes > (uint32_t)k_ra8p1_mem_m85_dtcm_floor_bytes) {
    return k_ra8_err_invalid_state;
  }

  /* Validate SRAM0 stays within 1024 KiB less NOINIT */
  if (ra8p1_workload_get_sram0_bytes() > (uint32_t)k_ra8p1_mem_sram0_limit_bytes) {
    return k_ra8_err_invalid_state;
  }

  /* Validate SRAM1 stays within 640 KiB shared boundary */
  if (ra8p1_workload_get_sram1_bytes() > (uint32_t)k_ra8p1_mem_sram1_limit_bytes) {
    return k_ra8_err_invalid_state;
  }

  /* Validate combined user SRAM stays within 1664 KiB silicon capacity */
  if (ra8p1_workload_get_total_user_sram_bytes() > (uint32_t)k_ra8p1_mem_user_sram_cap_bytes) {
    return k_ra8_err_invalid_state;
  }

  /* Validate SDRAM stays within 64 MiB baseline and 128 MiB ceiling */
  if (ra8p1_workload_get_sdram_bytes() > (uint32_t)k_ra8p1_mem_sdram_ek_base_bytes) {
    return k_ra8_err_invalid_state;
  }

  /* Validate OSPI CS0 stays within 128 MiB target density and 256 MiB CS window */
  if (ra8p1_workload_get_ospi_cs0_bytes() > (uint32_t)k_ra8p1_mem_ospi_cs_max_bytes) {
    return k_ra8_err_invalid_state;
  }

  /* Validate memory bandwidth headroom is >= 25% (ADR-0021 Section 4) */
  const uint32_t raw_bw = (uint32_t)k_ra8p1_sdram_raw_bw_mbps;
  const uint32_t peak_bw = (uint32_t)k_ra8p1_bw_concurrent_peak_mbps;
  if (peak_bw * 100U / raw_bw > 75U) {
    return k_ra8_err_invalid_state; /* Less than 25% bandwidth headroom */
  }

  return k_ra8_ok;
}

#ifdef __cplusplus
}
#endif
