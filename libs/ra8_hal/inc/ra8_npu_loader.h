/**
 * @file ra8_npu_loader.h
 * @brief On-target loader: `.npub` Vela blob -> `ra8_npu_job_t` (RA8P1-only)
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The FRONT HALF of the Ethos-U55 model loader. The offline Vela build step
 * (`tools/vela/src/vela_gen.py`) compiles a quantized `.tflite` model and distills
 * the `ethos-u` custom operator's command stream plus its tensor region layout
 * into a lean, linkable `.npub` container (`ra8_npu_blob.h`). This loader validates
 * that container and maps it into an ::ra8_npu_job_t so `ra8_npu_submit()` /
 * `ra8_npu_run()` can execute it -- WITHOUT parsing a TFLite flatbuffer on the MCU.
 *
 * For every region the blob declares, the loader produces a `BASEPn` base address:
 *  - a BAKED region (weights / const inputs) points at the region's bytes inside
 *    the blob itself (which lives in flash / MRAM, readable by the NPU over AXI);
 *  - a RUNTIME region (scratch / output activations) is carved out of a
 *    caller-provided arena in NPU-visible SRAM, 16-byte aligned.
 *
 * ## Scope
 *
 * This is the LOADER only. It does not run inference (that is `ra8_npu_run()` plus
 * the follow-up runtime) and does not itself compile a model (that is Vela, run
 * offline). It has no board dependency and is fully host-tested.
 *
 * ## Threading
 *
 * Re-entrant: the loader keeps no state and only reads the blob and writes the
 * caller's ::ra8_npu_job_t. The NPU serialisation contract is owned by `ra8_npu`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_device.h"
#include "ra8_err.h"

#ifndef RA8_HAS_NPU
#error "ra8_npu_loader.h included on a device without an NPU (RA8P1-only)."
#endif

#include "ra8_npu.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct ra8_npu_arena_t
 * @brief Caller-provided runtime arena for a model's RUNTIME regions.
 *
 * @details `base` is a writable block in NPU-visible system SRAM into which the
 *          loader lays out every region the blob marks RUNTIME (scratch / output
 *          activations), each 16-byte aligned and in descriptor order. `bytes` is
 *          its capacity; the loader fails with `k_ra8_err_no_mem` if the runtime
 *          regions do not fit. A model with only BAKED regions may pass a zero-size
 *          arena (`base` may then be nullptr).
 *
 * @invariant `base` addresses at least `bytes` writable bytes reachable by the NPU.
 *
 * @par Example:
 * @code
 * static uint8_t s_arena[4096];
 * ra8_npu_arena_t arena = { .base = s_arena, .bytes = (uint32_t)sizeof(s_arena) };
 * @endcode
 *
 * @see ra8_npu_load
 * @since 0.1.0
 */
typedef struct {
  uint8_t* base;  /**< Runtime arena base in NPU-visible SRAM (writable).     */
  uint32_t bytes; /**< Runtime arena capacity in bytes (0 if no runtime rgn). */
} ra8_npu_arena_t;

/**
 * @brief Validate a `.npub` blob and map it into an ::ra8_npu_job_t.
 *
 * @details
 * Checks the container magic, version, declared sizes, region count, and payload
 * checksum, then fills @p out_job: `cmd_stream` / `cmd_stream_bytes` point at the
 * blob's command stream, and each `region_base[i]` is resolved to the blob (BAKED
 * regions) or to @p arena (RUNTIME regions, 16-byte aligned in descriptor order).
 * The job is programmed but NOT submitted; the caller runs it via
 * `ra8_npu_submit()` + `ra8_npu_run()`.
 *
 * @param[in]  blob       Base of the `.npub` container in NPU-visible memory.
 * @param[in]  blob_bytes Length of @p blob in bytes (>= the header size).
 * @param[in]  arena      Runtime arena for RUNTIME regions (non-NULL; may be 0 sz).
 * @param[out] out_job    Filled job descriptor on success; untouched on failure.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok The blob is valid and @p out_job is fully populated.
 * @retval k_ra8_err_null_ptr @p blob, @p arena, or @p out_job was nullptr.
 * @retval k_ra8_err_invalid_size @p blob_bytes is below the header size, or the
 *         command stream length is zero.
 * @retval k_ra8_err_invalid_arg Bad magic, unsupported version, or a region count
 *         above `k_ra8_npu_region_count`.
 * @retval k_ra8_err_out_of_range A declared offset/size falls outside @p blob.
 * @retval k_ra8_err_checksum_mismatch The payload checksum did not match.
 * @retval k_ra8_err_no_mem The RUNTIME regions do not fit in @p arena.
 *
 * @pre @p blob, @p arena, and @p out_job are non-NULL.
 * @pre @p blob is resident in memory the NPU can read over AXI.
 * @post On k_ra8_ok @p out_job describes a runnable job (regions resolved).
 * @post On any error @p out_job is left unmodified.
 *
 * @note Re-entrant; keeps no state. Not an NPU-hardware call (touches no register).
 * @see ra8_npu_submit
 * @see ra8_npu_arena_t
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: 2 preconditions, 2 postconditions
 * - Rule 7: returns ra8_err_t, marked [[nodiscard]]
 */
[[nodiscard]] ra8_err_t ra8_npu_load(const void*            blob,
                                     uint32_t               blob_bytes,
                                     const ra8_npu_arena_t* arena,
                                     ra8_npu_job_t*         out_job);

/**
 * @brief Report the runtime-arena bytes a `.npub` blob requires.
 *
 * @details
 * Validates the container exactly as ra8_npu_load() does -- magic, version,
 * declared sizes, region-table bounds, payload checksum -- then walks the region
 * table and returns the arena the loader will claim: every RUNTIME region rounded
 * up to `k_ra8_npu_blob_arena_align` in descriptor order, BAKED regions
 * contributing nothing because their bytes stay in the blob. A model whose
 * regions are all baked reports 0.
 *
 * The figure is EXACT, not an upper bound: ra8_npu_load() succeeds with an arena
 * of precisely this many bytes and reports `k_ra8_err_no_mem` one byte below it,
 * because the loader measures the caller's arena against this same walk. Ask the
 * blob rather than hard-coding an arena constant beside it and discovering the
 * shortfall as a run-time `k_ra8_err_no_mem`.
 *
 * @param[in]  blob       Base of the `.npub` container.
 * @param[in]  blob_bytes Length of @p blob in bytes (>= the header size).
 * @param[out] out_bytes  Required runtime-arena length on success.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok The blob is valid and @p out_bytes holds the requirement.
 * @retval k_ra8_err_null_ptr @p blob or @p out_bytes was nullptr.
 * @retval k_ra8_err_invalid_size @p blob_bytes is below the header size, the
 *         command stream length is zero, or the requirement would exceed 32 bits.
 * @retval k_ra8_err_invalid_arg Bad magic, unsupported version, or a region count
 *         above `k_ra8_npu_region_count`.
 * @retval k_ra8_err_out_of_range A declared offset/size falls outside @p blob.
 * @retval k_ra8_err_checksum_mismatch The payload checksum did not match.
 *
 * @pre @p blob and @p out_bytes are non-NULL.
 * @pre @p blob addresses at least @p blob_bytes readable bytes.
 * @post On k_ra8_ok @p out_bytes is the arena size ra8_npu_load() will claim.
 * @post On any error @p out_bytes is left unmodified.
 *
 * @note Re-entrant; keeps no state, touches no NPU register, needs no arena.
 * @see ra8_npu_load
 * @see ra8_npu_arena_t
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: 2 preconditions, 2 postconditions
 * - Rule 7: returns ra8_err_t, marked [[nodiscard]]
 */
[[nodiscard]] ra8_err_t
ra8_npu_arena_bytes(const void* blob, uint32_t blob_bytes, uint32_t* out_bytes);

/**
 * @enum ra8_npu_loader_limit_t
 * @brief Fixed bounds and OSPI XIP controller limits for multi-model loading.
 *
 * @details
 * Bounds caller-provided multi-model tables (NASA Rule 2) and mirrors the
 * hardware external OSPI0 CS0 / CS1 XIP decode windows (`0x80000000` and
 * `0x90000000`, up to 256 MiB each per ADR-0021 and `ra8_device.h`).
 *
 * @invariant Every enumerator fits a `uint32_t`.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_max_models       = 16U,         /**< Max models in one multi-model table.            */
  k_ra8_npu_xip_ospi_cs0_min = 0x80000000U, /**< OSPI0 CS0 XIP base (`k_ra8_mem_ospi_cs0_base`). */
  k_ra8_npu_xip_ospi_cs1_min = 0x90000000U, /**< OSPI0 CS1 XIP base (`k_ra8_mem_ospi_cs1_base`). */
  k_ra8_npu_xip_ospi_cs_size = 0x10000000U, /**< Max 256 MiB per OSPI CS window (ADR-0021).      */
} ra8_npu_loader_limit_t;

/**
 * @struct ra8_npu_flash_window_t
 * @brief CPU-readable external OSPI flash XIP window for in-place `.npub` models.
 *
 * @details
 * On RA8P1 silicon (or the emulator), `mapped_base` equals
 * `(const void*)(uintptr_t)xip_base` once XIP is active so `BAKED` weights stay
 * in external OSPI NOR with zero SRAM copy. On host unit tests, `mapped_base`
 * points into a caller-supplied fake flash buffer while `xip_base` and
 * `window_bytes` still enforce hardware CS0/CS1 window boundaries.
 *
 * @invariant `xip_base` is ::k_ra8_npu_xip_ospi_cs0_min or ::k_ra8_npu_xip_ospi_cs1_min.
 * @since 0.1.0
 */
typedef struct ra8_npu_flash_window_s {
  const void* mapped_base;  /**< CPU-readable base address of the XIP window.        */
  uint32_t    xip_base;     /**< Target XIP window base (`0x80000000`/`0x90000000`). */
  uint32_t    window_bytes; /**< Window byte capacity (1 .. 256 MiB).                */
} ra8_npu_flash_window_t;

/**
 * @struct ra8_npu_model_entry_t
 * @brief One `.npub` model registered in a caller-owned multi-model table.
 *
 * @details
 * Holds the CPU-readable blob pointer (in external OSPI XIP flash, MRAM, or
 * a host buffer), byte length, and recorded XIP address (or `0` when bound
 * outside an OSPI XIP window).
 *
 * @since 0.1.0
 */
typedef struct ra8_npu_model_entry_s {
  const void* blob;       /**< In-place `.npub` image pointer (read-only). */
  uint32_t    blob_bytes; /**< Validated length of the `.npub` image.      */
  uint32_t    xip_addr;   /**< Recorded target XIP address (0 if direct).  */
} ra8_npu_model_entry_t;

/**
 * @struct ra8_npu_model_table_t
 * @brief Caller-owned table of `count` `.npub` models sharing one scratch arena.
 *
 * @invariant `models` points to at least `count` initialized entries (`count <= ::k_ra8_npu_max_models`).
 * @since 0.1.0
 */
typedef struct ra8_npu_model_table_s {
  const ra8_npu_model_entry_t* models; /**< Array of registered models.       */
  uint32_t                     count;  /**< Active entries (1 .. max_models). */
} ra8_npu_model_table_t;

/**
 * @brief Bind and validate one external OSPI XIP `.npub` model into an entry.
 *
 * @details
 * Verifies that @p window names OSPI0 CS0 (`0x80000000`) or CS1 (`0x90000000`)
 * with a non-zero size <= ::k_ra8_npu_xip_ospi_cs_size, checks that
 * `[xip_offset, xip_offset + blob_bytes)` fits inside @p window without
 * overflow, validates the `.npub` container in place at `mapped_base +
 * xip_offset` (so corrupt or incomplete flash writes are refused upfront), and
 * writes @p out_entry with `BAKED` weights left in flash.
 *
 * @param[in]  window     External OSPI flash XIP window descriptor.
 * @param[in]  xip_offset Byte offset within @p window where the `.npub` starts.
 * @param[in]  blob_bytes Byte length of the `.npub` image.
 * @param[out] out_entry  Populated model entry on k_ra8_ok.
 *
 * @return `ra8_err_t` status code.
 * @retval k_ra8_ok Valid OSPI XIP `.npub` image bound to @p out_entry.
 * @retval k_ra8_err_null_ptr Null @p window, @p window->mapped_base, or @p out_entry.
 * @retval k_ra8_err_invalid_arg @p window->xip_base is not an OSPI XIP base, or bad blob header.
 * @retval k_ra8_err_invalid_size Zero window size, tiny blob, or 0 command bytes.
 * @retval k_ra8_err_out_of_range Window exceeds 256 MiB or blob span leaves window/buffer.
 * @retval k_ra8_err_checksum_mismatch Payload checksum failed.
 *
 * @pre @p window and @p out_entry are non-NULL and @p window->mapped_base is readable.
 * @post On k_ra8_ok @p out_entry points in place into @p window with `xip_addr = window->xip_base + xip_offset`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_npu_model_from_flash(const ra8_npu_flash_window_t* window,
                                                 uint32_t                      xip_offset,
                                                 uint32_t                      blob_bytes,
                                                 ra8_npu_model_entry_t*        out_entry);

/**
 * @brief Plan the shared scratch-arena byte budget across `N` `.npub` models.
 *
 * @details
 * Validates every model in @p table (headers, descriptors, checksums, and
 * 16-byte-aligned runtime region sizing) and returns the maximum arena size
 * across all entries in @p out_bytes. A single caller-supplied 16-byte-aligned
 * arena of `*out_bytes` bytes can then alternate any model in @p table without
 * reallocation while `BAKED` weights stay in flash. Does not touch `*out_bytes`
 * on error.
 *
 * @param[in]  table     Multi-model table (1 .. ::k_ra8_npu_max_models entries).
 * @param[out] out_bytes Maximum 16-byte-aligned scratch bytes required across @p table.
 *
 * @return `ra8_err_t` status code.
 * @retval k_ra8_ok All models validated; `*out_bytes` holds shared arena size.
 * @retval k_ra8_err_null_ptr Null @p table, @p table->models, @p out_bytes, or entry blob.
 * @retval k_ra8_err_invalid_arg Model count 0 / exceeds max, or invalid model header.
 * @retval k_ra8_err_invalid_size Overflowing arena requirement or invalid blob length.
 * @retval k_ra8_err_out_of_range Span outside blob buffer.
 * @retval k_ra8_err_checksum_mismatch Corrupt model checksum.
 *
 * @pre @p table, @p table->models, and @p out_bytes are non-NULL.
 * @post On k_ra8_ok `*out_bytes` holds the exact shared arena size for @p table.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_npu_shared_arena_bytes(const ra8_npu_model_table_t* table,
                                                   uint32_t*                    out_bytes);

/**
 * @brief Load one model from @p table against the shared 16-byte-aligned arena.
 *
 * @details
 * Resolves entry @p model_index from @p table into @p out_job against
 * @p shared_arena so back-to-back models (e.g. text detector followed by OCR
 * recognizer) reuse the same scratch memory without reallocation. `BAKED`
 * weights and command streams stay in place inside the entry's blob (e.g. XIP
 * OSPI flash); only `RUNTIME` regions bind into @p shared_arena. When
 * `shared_arena->bytes > 0`, `shared_arena->base` must be 16-byte aligned.
 *
 * @param[in]  table        Multi-model table.
 * @param[in]  model_index  Zero-based index `< table->count`.
 * @param[in]  shared_arena Caller-owned shared scratch arena.
 * @param[out] out_job      Populated NPU job ready for `ra8_npu_submit()`.
 *
 * @return `ra8_err_t` status code.
 * @retval k_ra8_ok Job populated; ready to submit.
 * @retval k_ra8_err_null_ptr Null pointer argument or NULL entry blob / non-empty arena base.
 * @retval k_ra8_err_invalid_arg Bad table count, unaligned arena base, or malformed blob.
 * @retval k_ra8_err_out_of_range @p model_index >= @p table->count or out-of-bounds span.
 * @retval k_ra8_err_no_mem @p shared_arena is smaller than the model requires.
 * @retval k_ra8_err_checksum_mismatch Corrupt model checksum.
 *
 * @pre @p table, @p table->models, @p shared_arena, and @p out_job are non-NULL.
 * @post On k_ra8_ok @p out_job has all `RUNTIME` regions mapped into @p shared_arena
 *       and all `BAKED` regions pointing in place into the model blob.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_npu_table_load(const ra8_npu_model_table_t* table,
                                           uint32_t                     model_index,
                                           const ra8_npu_arena_t*       shared_arena,
                                           ra8_npu_job_t*               out_job);

#ifdef __cplusplus
}
#endif
