/**
 * @file ra8_ftl.h
 * @brief Flash Translation Layer -- free overwrite over erase-before-write media.
 * @ingroup grp_storage
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The RA8D2 on-chip extra MRAM (data flash) is **erase-before-write**: a
 * 512-byte logical block can only be programmed after its backing erase block
 * has been cleared to the erase value (`0xFF`). FAT, by contrast, freely
 * overwrites individual sectors in place (directory entries, FAT links) with no
 * erase step, so layering FAT directly over the raw MRAM block device corrupts
 * data. See issue #165.
 *
 * This module is a Flash Translation Layer (FTL). It **wraps** an
 * erase-before-write ::ra8_io_blockdev_t (the underlying device) and **presents**
 * a clean free-overwrite ::ra8_io_blockdev_t to the FAT/VFS layer above. The FAT
 * layer sees a device it can overwrite at will; the FTL handles erase ordering,
 * copy-on-write relocation, stale-block reclamation, and wear-levelling
 * underneath.
 *
 * ## Model
 *
 * The FTL works one logical block to one physical erase block. The underlying
 * device must report `erase_unit_blocks == 1` (each 512-byte logical block is
 * exactly one erase unit) -- this is true of the MRAM backend, whose 512-byte
 * logical block already maps onto a whole set of native MRAM erase units. The
 * underlying device has `P` physical blocks; the FTL presents `L` logical
 * blocks where `L < P`. The surplus `P - L` blocks are spare capacity used as
 * copy-on-write relocation targets and as wear-levelling headroom.
 *
 * - **Mapping table** (caller storage, one `uint16_t` per logical block):
 *   `map[lbn]` is the physical block currently holding that logical block's
 *   data, or ::k_ra8_ftl_unmapped if the logical block has never been written
 *   (a read of an unmapped block returns the erase value).
 * - **Per-physical-block metadata** (caller storage, one
 *   ::ra8_ftl_pblock_t per physical block): the block's state (free / live /
 *   stale) and its cumulative erase count for wear-levelling.
 *
 * ## Write path (copy-on-write + wear-levelling)
 *
 * On a logical-block write the FTL:
 * 1. Picks the **least-erased FREE** physical block (wear-levelling); if no
 *    free block exists it first reclaims STALE blocks by erasing them.
 * 2. Erases that physical block (advancing its erase count), then programs the
 *    new data into it.
 * 3. Re-points `map[lbn]` at the new physical block and marks the previous
 *    physical block (if any) STALE for later reclamation.
 *
 * This never overwrites a non-blank physical block, so the underlying
 * erase-before-write contract is always honoured.
 *
 * ## Invariants
 *
 * - `map[lbn]` is either ::k_ra8_ftl_unmapped or a valid physical index whose
 *   metadata state is LIVE.
 * - Every LIVE physical block is referenced by exactly one `map[]` entry.
 * - A FREE physical block reads back entirely as the erase value.
 * - `L < P` always (at least one spare block) so a write can always relocate.
 *
 * ## Storage (zero dynamic allocation, NASA P10 Rule 3)
 *
 * All state is caller-provided: the ::ra8_ftl_t handle, the `map` array
 * (`logical_blocks` entries of `uint16_t`), the `pblocks` array
 * (`physical_blocks` entries of ::ra8_ftl_pblock_t), and one 512-byte scratch
 * buffer for copy operations. Nothing is allocated.
 *
 * @code
 * extern ra8_io_blockdev_t raw;          // bound MRAM (erase-before-write) device
 * static uint16_t        s_map[24];     // logical_blocks entries
 * static ra8_ftl_pblock_t s_pb[24];      // physical_blocks entries
 * static uint8_t         s_scratch[512];
 * ra8_ftl_t        ftl = {};
 * ra8_io_blockdev_t bd = {};             // clean free-overwrite device for FAT
 * (void)ra8_ftl_init(&ftl, &raw, s_map, 16U, s_pb, 24U, s_scratch);
 * (void)ra8_ftl_as_blockdev(&ftl, &bd);  // FAT now mounts on bd
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_io_blockdev.h"

/* =============================================================================
 * Constants
 * =============================================================================
 */

/**
 * @enum ra8_ftl_const_t
 * @brief FTL sizing and sentinel constants.
 *
 * @details
 * The mapping table stores physical block indices as `uint16_t`, so the FTL
 * addresses at most ::k_ra8_ftl_max_pblocks physical blocks. ::k_ra8_ftl_unmapped
 * is the reserved sentinel meaning "this logical block has never been written".
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_ftl_unmapped    = 0xFFFF, /**< `map[]` sentinel: logical block unwritten. */
  k_ra8_ftl_max_pblocks = 0xFFFE, /**< Max physical blocks an FTL may wrap.       */
  k_ra8_ftl_min_spare   = 1,      /**< Minimum spare blocks (`P - L >= 1`).       */
} ra8_ftl_const_t;

/**
 * @enum ra8_ftl_pstate_t
 * @brief Lifecycle state of one physical erase block.
 *
 * @details
 * A physical block cycles FREE -> LIVE (programmed, referenced by a `map[]`
 * entry) -> STALE (superseded by a copy-on-write relocation) -> FREE (after the
 * FTL reclaims it with an erase).
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_ftl_pstate_free  = 0, /**< Blank (reads as erase value); allocatable. */
  k_ra8_ftl_pstate_live  = 1, /**< Holds current data for one logical block.  */
  k_ra8_ftl_pstate_stale = 2, /**< Superseded; reclaimable by erase to FREE.  */
} ra8_ftl_pstate_t;

/* =============================================================================
 * Per-physical-block metadata
 * =============================================================================
 */

/**
 * @struct ra8_ftl_pblock_t
 * @brief Caller-owned metadata for one physical erase block.
 *
 * @details
 * One entry per physical block of the underlying device. Zero-initialise the
 * whole array (`= {}`): all-zero means every block is FREE with a zero erase
 * count, which is the correct cold-start state. Treat the fields as private;
 * the FTL maintains them.
 *
 * @invariant `state` is one of ::ra8_ftl_pstate_t.
 * @invariant `erase_count` is monotonically non-decreasing across the device
 *            lifetime.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t erase_count; /**< Cumulative erases of this block (wear metric). */
  uint8_t  state;       /**< ::ra8_ftl_pstate_t lifecycle state (private).  */
} ra8_ftl_pblock_t;
/* cppcheck-suppress-end [unusedStructMember] */

/* =============================================================================
 * Handle
 * =============================================================================
 */

/**
 * @struct ra8_ftl_t
 * @brief Caller-allocated FTL handle binding the wrapper to the raw device.
 *
 * @details
 * Zero-initialise (`= {}`) and pass to ::ra8_ftl_init, which validates the
 * underlying device, records the caller-provided storage, and prepares the
 * cold-start mapping. Treat the fields as private; drive the FTL through the
 * `ra8_ftl_*` functions and the block device produced by ::ra8_ftl_as_blockdev.
 * The handle, the underlying device, and all caller storage must out-live every
 * call made through them.
 *
 * @invariant `logical_blocks < physical_blocks` (at least one spare block).
 * @invariant `map` has `logical_blocks` entries; `pblocks` has
 *            `physical_blocks` entries.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  const ra8_io_blockdev_t* raw;             /**< Underlying erase-before-write dev. */
  uint16_t*                map;             /**< logical->physical map (private).   */
  ra8_ftl_pblock_t*        pblocks;         /**< Per-physical metadata (private).   */
  uint8_t*                 scratch;         /**< 512-byte copy scratch (private).   */
  uint32_t                 logical_blocks;  /**< Blocks presented to FAT (private). */
  uint32_t                 physical_blocks; /**< Blocks in the raw dev (private).   */
  uint8_t                  erase_value;     /**< Raw medium erase byte (private).   */
} ra8_ftl_t;
/* cppcheck-suppress-end [unusedStructMember] */

/* =============================================================================
 * API
 * =============================================================================
 */

/**
 * @brief Initialise an FTL over an erase-before-write block device.
 *
 * @details
 * Validates the underlying device's capabilities (it must report
 * `erase_unit_blocks == 1`, must not be read-only, and must have at least
 * `logical_blocks + k_ra8_ftl_min_spare` physical blocks), records the
 * caller-provided storage, snapshots the medium erase value, and sets every
 * logical block to ::k_ra8_ftl_unmapped and every physical block to FREE. No
 * erase or program is issued here; physical blocks are erased lazily on first
 * write. No allocation occurs.
 *
 * @param[out] bd              FTL handle to initialise (zero-initialised first).
 * @param[in]  raw             Bound underlying erase-before-write device.
 * @param[out] map             Caller array of `logical_blocks` `uint16_t`.
 * @param[in]  logical_blocks  Blocks to present to FAT (>= 1).
 * @param[out] pblocks         Caller array of `physical_blocks` metadata entries
 *                             (zero-initialised by the caller).
 * @param[in]  physical_blocks Physical blocks in `raw`
 *                             (>= `logical_blocks + k_ra8_ftl_min_spare`).
 * @param[out] scratch         Caller 512-byte copy scratch buffer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  FTL initialised and ready.
 * @retval k_ra8_err_null_ptr        Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_size    `logical_blocks` was zero or `physical_blocks`
 *                                  exceeds ::k_ra8_ftl_max_pblocks.
 * @retval k_ra8_err_invalid_arg     Underlying caps query failed, the device is
 *                                  read-only, `erase_unit_blocks != 1`, or there
 *                                  is no spare block.
 * @retval k_ra8_err_not_initialized No backend is bound to `raw`.
 *
 * @pre All pointer arguments out-live every call through the FTL.
 * @pre `pblocks[0 .. physical_blocks)` were zero-initialised by the caller.
 * @post On success every logical block is unmapped and every physical block FREE.
 * @post On any non-ok return `bd` is left unbound.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @see ra8_ftl_as_blockdev
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ftl_init(ra8_ftl_t*               bd,
                                     const ra8_io_blockdev_t* raw,
                                     uint16_t*                map,
                                     uint32_t                 logical_blocks,
                                     ra8_ftl_pblock_t*        pblocks,
                                     uint32_t                 physical_blocks,
                                     uint8_t*                 scratch);

/**
 * @brief Expose an initialised FTL as a free-overwrite ::ra8_io_blockdev_t.
 *
 * @details
 * Binds `out` to the FTL vtable with the ::ra8_ftl_t as its context, so the
 * layers above (FAT, VFS, the write-back cache) see a normal block device that
 * supports in-place overwrite, never needs an explicit erase, and reads
 * unwritten blocks back as the underlying erase value. The handle passed as
 * `ftl` must out-live the resulting block device.
 *
 * @param[in]  ftl FTL handle previously initialised by ::ra8_ftl_init.
 * @param[out] out Block-device handle to bind (zero-initialised by the caller).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `out` bound to the FTL.
 * @retval k_ra8_err_null_ptr        `ftl` or `out` was NULL.
 * @retval k_ra8_err_not_initialized `ftl` was not initialised (no raw device).
 *
 * @pre `ftl` was initialised by ::ra8_ftl_init.
 * @pre `out` is writable and out-lives every block-device call.
 * @post On success `out` dispatches every operation through the FTL.
 * @post On any non-ok return `out` is left unbound.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @see ra8_ftl_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ftl_as_blockdev(ra8_ftl_t* ftl, ra8_io_blockdev_t* out);

/**
 * @brief Report the highest per-physical-block erase count seen so far.
 *
 * @details
 * Wear-levelling diagnostic: returns the maximum and minimum `erase_count`
 * across all physical blocks. A healthy FTL keeps these close together; a large
 * spread indicates a hot block. Intended for tests and telemetry, not the data
 * path.
 *
 * @param[in]  ftl     Initialised FTL handle.
 * @param[out] max_out Receives the maximum erase count.
 * @param[out] min_out Receives the minimum erase count.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `*max_out` and `*min_out` populated.
 * @retval k_ra8_err_null_ptr        Any pointer argument was NULL.
 * @retval k_ra8_err_not_initialized `ftl` was not initialised.
 *
 * @pre `ftl` was initialised by ::ra8_ftl_init.
 * @pre `max_out` and `min_out` are writable.
 * @post On success `*max_out >= *min_out`.
 * @post No FTL or device state is mutated.
 *
 * @note Thread-safe with respect to the data path is NOT guaranteed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ftl_wear_stats(const ra8_ftl_t* ftl, uint32_t* max_out, uint32_t* min_out);

#ifdef __cplusplus
}
#endif
