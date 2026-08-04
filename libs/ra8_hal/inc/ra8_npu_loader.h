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
 * (`tools/vela/vela_gen.py`) compiles a quantized `.tflite` model and distills
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

#ifdef __cplusplus
}
#endif
