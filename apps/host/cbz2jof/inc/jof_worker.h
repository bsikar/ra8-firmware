/**
 * @file jof_worker.h
 * @brief Single-image to JOF worker: the per-page transcode the cbz2jof Go
 *        driver shells out to.
 * @details The Go layer owns archive policy (CBZ entry selection, limits,
 *          atomic page publication); this worker owns pixels. It reads one
 *          encoded JPEG, PNG, or WebP source file through the firmware's own
 *          `jof_produce()` pipeline and writes one JOF band-tile atlas, so a
 *          page converted here is byte-identical to one the RA8 would produce
 *          from the same source bytes.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum jof_worker_result_t
 * @brief Process exit code contract between the worker and its Go driver.
 * @details Explicitly sized so the `ok=0 .. memory=6` mapping below is the
 *          ABI, not a compiler choice. `ok` is zero so the shell idiom
 *          `if (rc)` treats any failure as true.
 * @since 0.1.0
 */
typedef enum : int {
  k_jof_worker_ok       = 0, /**< Atlas fully written to the output path.      */
  k_jof_worker_usage    = 1, /**< Wrong argument count or null path.           */
  k_jof_worker_input    = 2, /**< Input open/stat/read failed or was refused.  */
  k_jof_worker_output   = 3, /**< Output open/write/close failed.              */
  k_jof_worker_geometry = 4, /**< Source dimensions unusable or unproduceable. */
  k_jof_worker_decode   = 5, /**< Source is hostile or an unsupported variant. */
  k_jof_worker_memory   = 6, /**< A bounded work arena could not be allocated. */
} jof_worker_result_t;

/**
 * @brief Transcode one encoded image file into one JOF atlas file.
 * @details Opens and bounds-checks the input, probes its dimensions, sizes
 *          the exact `jof_work_bytes()` / `jof_webp_work_bytes()` arenas for
 *          them, runs `jof_produce()` with a full-width band tile
 *          (`tile_w == width`, `tile_h == min(height, 256)`), and closes the
 *          output. On any failure the output path is closed and removed, so
 *          no torn atlas is left behind; cleanup failure never masks the
 *          primary error.
 * @param[in] in_path  Encoded source image (JPEG, PNG, or WebP).
 * @param[in] out_path Destination JOF atlas (created/truncated).
 * @return One ::jof_worker_result_t member.
 * @retval k_jof_worker_ok       Atlas fully written to the output path.
 * @retval k_jof_worker_usage    Wrong argument count or null path.
 * @retval k_jof_worker_input    Input open/stat/read failed or was refused.
 * @retval k_jof_worker_output   Output open/write/close failed.
 * @retval k_jof_worker_geometry Source dimensions unusable or unproduceable.
 * @retval k_jof_worker_decode   Source is hostile or an unsupported variant.
 * @retval k_jof_worker_memory   A bounded work arena could not be allocated.
 * @pre @p in_path and @p out_path are non-NULL NUL-terminated paths.
 * @pre @p in_path points to an existing readable file.
 * @post On `k_jof_worker_ok` @p out_path holds one complete JOF atlas.
 * @post On any other result @p out_path does not exist.
 * @note Not thread-safe (module-static decoder contexts in the producer).
 * @since 0.1.0
 */
[[nodiscard]] jof_worker_result_t jof_worker_convert(const char* in_path, const char* out_path);

#ifdef __cplusplus
}
#endif
