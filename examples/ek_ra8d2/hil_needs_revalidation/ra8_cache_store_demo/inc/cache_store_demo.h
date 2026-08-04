/**
 * @file cache_store_demo.h
 * @brief Hardware-free driver for the ra8_cache_store on-media cache demo (#257).
 *
 * @details
 * Factors the whole ra8_cache_store demonstration -- mount, put/get several keyed
 * blobs, pin one, force an eviction, re-put into the reclaimed space, close, then
 * remount and confirm on-media persistence -- into a single pure function that
 * touches NO hardware. It takes the injected LevelX NOR driver-initialise
 * callback (`ra8_cache_store_nor_init_fn`) plus all caller-owned buffers, so the
 * identical logic runs in two places: the ARM example's `main.c` binds a
 * RAM-backed NOR driver over SRAM, and the host unit test binds the same driver
 * on the x86_64 host. That shared core is what makes the ra8_emulator gate and the
 * host test exercise byte-identical behaviour.
 *
 * @note Not thread-safe: ra8_cache_store serialises access (one reader core).
 * @see cache_store_demo_run  The single entry point.
 * @since 0.1.0
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_cache_store.h"
#include "ra8_err.h"

/**
 * @enum cache_store_demo_stage_t
 * @brief Furthest sequence stage the demo reached (for diagnostics on failure).
 * @details Recorded in ::cache_store_demo_result_t::last_stage so a failing run
 *          reports exactly which step broke.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cache_store_demo_stage_none      = 0U, /**< Nothing ran yet.                    */
  k_cache_store_demo_stage_mounted   = 1U, /**< Store formatted + mounted.          */
  k_cache_store_demo_stage_seeded    = 2U, /**< Entries put + read-verified.        */
  k_cache_store_demo_stage_evicted   = 3U, /**< Pinned one, forced an eviction.     */
  k_cache_store_demo_stage_reused    = 4U, /**< Re-put into the reclaimed sectors.  */
  k_cache_store_demo_stage_closed    = 5U, /**< Checkpointed + clean-closed.        */
  k_cache_store_demo_stage_remounted = 6U, /**< Re-opened over the same media.      */
  k_cache_store_demo_stage_persisted = 7U, /**< Survivors + pin confirmed on media. */
} cache_store_demo_stage_t;

/**
 * @enum cache_store_demo_status_t
 * @brief Granular verdict for one ::cache_store_demo_run.
 * @details ::k_cache_store_demo_ok means every stage passed; any other value
 *          names the stage that failed.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cache_store_demo_ok          = 0U, /**< All stages passed.                      */
  k_cache_store_demo_err_arg     = 1U, /**< NULL config / result / buffer.          */
  k_cache_store_demo_err_mount   = 2U, /**< Format + mount failed.                  */
  k_cache_store_demo_err_seed    = 3U, /**< A put or its read-back verify failed.   */
  k_cache_store_demo_err_evict   = 4U, /**< Eviction / survivor / pin-guard failed. */
  k_cache_store_demo_err_reuse   = 5U, /**< Re-put into reclaimed sectors failed.   */
  k_cache_store_demo_err_close   = 6U, /**< Checkpoint + close failed.              */
  k_cache_store_demo_err_remount = 7U, /**< Re-mount over the same media failed.    */
  k_cache_store_demo_err_persist = 8U, /**< A survivor / eviction / pin did not     */
                                       /**< survive the remount. */
} cache_store_demo_status_t;

/**
 * @struct cache_store_demo_result_t
 * @brief Outcome + observable facts of one demo run (for the banner / asserts).
 * @details Populated by ::cache_store_demo_run. The example prints these fields;
 *          the host test asserts them.
 * @invariant When `status == k_cache_store_demo_ok`, `survivors` is the live
 *            entry count after the remount and both survival flags are true.
 * @since 0.1.0
 */
typedef struct {
  cache_store_demo_status_t status;              /**< Overall verdict.                       */
  cache_store_demo_stage_t  last_stage;          /**< Furthest stage reached.                */
  uint32_t                  survivors;           /**< Live entries after the remount.        */
  uint32_t                  evicted_key;         /**< Key dropped by the forced eviction.    */
  bool                      evicted_stayed_gone; /**< Evicted key still absent post-remount. */
  bool                      pin_survived;        /**< Pinned entry still refuses eviction.   */
} cache_store_demo_result_t;

/**
 * @struct cache_store_demo_cfg_t
 * @brief Injected dependencies + caller-owned buffers for ::cache_store_demo_run.
 * @details Open/Closed seam: the demo owns no globals. The LevelX control block,
 *          the physical-flash driver bind, the store index array, the one-sector
 *          staging buffer, and a read-back scratch buffer are all passed in, so
 *          the ARM app (RAM driver over SRAM) and the host test (RAM driver on
 *          the host) drive the same core through different buffers.
 * @invariant `nor_flash`, `nor_driver_init`, `index`, `staging`, and `scratch`
 *            are non-NULL; `index_cap > 0`; `scratch_bytes` covers the largest
 *            demo blob.
 * @since 0.1.0
 */
typedef struct {
  struct LX_NOR_FLASH_STRUCT* nor_flash;       /**< Caller-owned LevelX control block.   */
  ra8_cache_store_nor_init_fn nor_driver_init; /**< Injected physical-flash bind.        */
  ra8_cache_store_entry_t*    index;           /**< Caller-owned index array.            */
  uint8_t*                    staging;         /**< Caller-owned >=1-sector scratch.     */
  uint8_t*                    scratch;         /**< Read-back scratch (>= largest blob). */
  uint32_t                    staging_bytes;   /**< Bytes at @c staging (>= sector).     */
  uint32_t                    scratch_bytes;   /**< Bytes at @c scratch.                 */
  uint32_t                    logical_sectors; /**< Usable LevelX logical-sector span.   */
  uint16_t                    index_cap;       /**< Index slots (max live entries).      */
} cache_store_demo_cfg_t;

/**
 * @brief Run the full ra8_cache_store on-media cache demonstration, end to end.
 *
 * @details
 * Executes, in order, over the injected NOR driver:
 *   1. Format + mount a fresh cache_store.
 *   2. Put four keyed "render/glyph cache" blobs and read each back
 *      byte-identical (whole + sliced reads).
 *   3. Pin one entry (the open-book cover atlas -- never evict), then force an
 *      eviction of an un-pinned entry; confirm it is now a miss, the survivors
 *      still read, and evicting the pinned entry is refused (busy).
 *   4. Re-put a new blob, proving the evicted entry's sectors were reclaimed.
 *   5. Checkpoint + clean-close the store.
 *   6. Re-mount over the same media with a fresh (zeroed) control block -- a
 *      simulated reboot -- and confirm the survivors (including the pinned one)
 *      come back byte-identical, the evicted key stays gone, and the pin
 *      persisted across the checkpoint.
 *
 * @param[in]  cfg Injected dependencies + buffers (see the struct).
 * @param[out] out Receives the verdict + observable facts.
 *
 * @return Error code.
 * @retval k_ra8_ok               Every stage passed (`out->status` == ok).
 * @retval k_ra8_err_null_ptr     `cfg`, `out`, or a required buffer was NULL.
 * @retval k_ra8_err_invalid_state A stage failed; `out->status` / `out->last_stage`
 *                                name it.
 *
 * @pre `cfg` and `out` are non-NULL.
 * @pre Every non-optional `cfg` member is non-NULL and correctly sized.
 * @post `out->status` and `out->last_stage` reflect how far the run got.
 * @post On `k_ra8_ok`, `out->survivors` counts the entries that survived the
 *       remount and both survival flags are true.
 *
 * @note Not thread-safe.
 * @see cache_store_demo_result_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t cache_store_demo_run(const cache_store_demo_cfg_t* cfg,
                                             cache_store_demo_result_t*    out);

#ifdef __cplusplus
}
#endif
