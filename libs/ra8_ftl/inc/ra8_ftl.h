/**
 * @file ra8_ftl.h
 * @brief Flash Translation Layer -- free overwrite over erase-before-write
 * media.
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
 * erase-before-write ::ra8_io_blockdev_t (the underlying device) and
 * **presents** a clean free-overwrite ::ra8_io_blockdev_t to the FAT/VFS layer
 * above. The FAT layer sees a device it can overwrite at will; the FTL handles
 * erase ordering, copy-on-write relocation, stale-block reclamation, and
 * wear-levelling underneath.
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
 * @code{.c}
 * extern ra8_io_blockdev_t raw;
 * uint16_t map[24] = {};
 * ra8_ftl_pblock_t pblocks[24] = {};
 * uint8_t scratch[512];
 * ra8_ftl_t ftl = {};
 * ra8_io_blockdev_t bd = {};
 *
 * ra8_err_t err = ra8_ftl_init(&ftl, &raw, map, 16U, pblocks, 24U, scratch);
 * if (err == k_ra8_ok) {
 *   err = ra8_ftl_as_blockdev(&ftl, &bd);
 * }
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
 * addresses at most ::k_ra8_ftl_max_pblocks physical blocks.
 * ::k_ra8_ftl_unmapped is the reserved sentinel meaning "this logical block has
 * never been written".
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
typedef struct {
  uint32_t erase_count; /**< Cumulative erases of this block (wear metric). */
  uint8_t  state;       /**< ::ra8_ftl_pstate_t lifecycle state (private).  */
} ra8_ftl_pblock_t;

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
 * @invariant `reserved_tail` is non-zero and `ckbuf` non-NULL exactly when the
 *            handle was brought up by ::ra8_ftl_mount; ::ra8_ftl_init leaves
 *            both clear, because a hand-initialised FTL owns no checkpoint
 *            home.
 *
 * @since 0.1.0
 */
typedef struct {
  const ra8_io_blockdev_t* raw;             /**< Underlying erase-before-write dev. */
  uint16_t*                map;             /**< logical->physical map (private).   */
  ra8_ftl_pblock_t*        pblocks;         /**< Per-physical metadata (private).   */
  uint8_t*                 scratch;         /**< 512-byte copy scratch (private).   */
  uint8_t*                 ckbuf;           /**< Checkpoint staging buf (private).  */
  uint32_t                 ckbuf_len;       /**< Bytes in `ckbuf` (private).        */
  uint32_t                 logical_blocks;  /**< Blocks presented to FAT (private). */
  uint32_t                 physical_blocks; /**< Blocks in the raw dev (private).   */
  uint32_t                 reserved_tail;   /**< Checkpoint tail blocks (private).  */
  uint8_t                  erase_value;     /**< Raw medium erase byte (private).   */
} ra8_ftl_t;

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
 * @param[out] bd              FTL handle to initialise; caller zero-initialises
 *                             it before use.
 * @param[in]  raw             Bound underlying erase-before-write device.
 * @param[out] map             Caller array of `logical_blocks` `uint16_t`.
 * @param[in]  logical_blocks  Blocks to present to FAT (>= 1).
 * @param[out] pblocks         Caller array of `physical_blocks` metadata
 *                             entries; caller zero-initialises it.
 * @param[in]  physical_blocks Physical blocks in `raw`
 *                             (>= `logical_blocks + k_ra8_ftl_min_spare`).
 * @param[out] scratch         Caller 512-byte copy scratch buffer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  FTL initialised and ready.
 * @retval k_ra8_err_null_ptr        Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_size    `logical_blocks` was zero or
 *                                   `physical_blocks` exceeds
 *                                   ::k_ra8_ftl_max_pblocks.
 * @retval k_ra8_err_invalid_arg     Underlying caps query failed, the device is
 *                                   read-only, `erase_unit_blocks != 1`, or
 *                                   there is no spare block.
 * @retval k_ra8_err_not_initialized No backend is bound to `raw`.
 *
 * @pre All pointer arguments out-live every call through the FTL.
 * @pre `pblocks[0 .. physical_blocks)` were zero-initialised by the caller.
 * @post On success every logical block is unmapped and every physical block
 * FREE.
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

/**
 * @brief Report the physical block currently backing a logical block.
 *
 * @details
 * Read-only mapping telemetry: resolves `map[lbn]` and returns the physical
 * block index that presently holds the logical block's data, or
 * ::k_ra8_ftl_unmapped if the logical block has never been written. Because a
 * copy-on-write write re-points `map[lbn]` at a freshly allocated physical
 * block, calling this after each overwrite exposes the wear-levelling
 * relocation: the reported index migrates while the logical address stays
 * fixed. Intended for demos, tests, and telemetry -- not the data path.
 *
 * @param[in]  ftl      Initialised FTL handle.
 * @param[in]  lbn      Logical block number (`< logical_blocks`).
 * @param[out] phys_out Receives the physical block index, or
 *                      ::k_ra8_ftl_unmapped when the block is unwritten.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `*phys_out` populated.
 * @retval k_ra8_err_null_ptr        `ftl` or `phys_out` was NULL.
 * @retval k_ra8_err_not_initialized `ftl` was not initialised.
 * @retval k_ra8_err_out_of_range    `lbn >= logical_blocks`.
 *
 * @pre `ftl` was initialised by ::ra8_ftl_init.
 * @pre `phys_out` is writable.
 * @post On success `*phys_out` is either ::k_ra8_ftl_unmapped or a valid
 *       physical index (`< physical_blocks`).
 * @post No FTL or device state is mutated.
 *
 * @note Not thread-safe with respect to the data path.
 *
 * @see ra8_ftl_wear_stats
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ftl_phys_of(const ra8_ftl_t* ftl, uint32_t lbn, uint16_t* phys_out);

/**
 * @brief Report the buffer size a checkpoint of this FTL requires, in bytes.
 *
 * @details
 * A checkpoint captures the FTL's volatile mapping state (the `map` and
 * `pblocks` tables) so it can be persisted to non-volatile media and reloaded
 * after a reset. This returns the exact byte count ::ra8_ftl_checkpoint_save
 * needs for the current geometry: a fixed canonical header, two bytes per map
 * entry, five bytes per physical-block entry, and a CRC-32 trailer. The value
 * is independent of compiler padding, native integer layout, and host byte
 * order, and is stable for a given initialised handle.
 *
 * @param[in]  ftl      Initialised FTL handle.
 * @param[out] size_out Receives the required checkpoint size in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `*size_out` populated.
 * @retval k_ra8_err_null_ptr        `ftl` or `size_out` was NULL.
 * @retval k_ra8_err_not_initialized `ftl` was not initialised.
 *
 * @pre `ftl` was initialised by ::ra8_ftl_init.
 * @pre `size_out` is writable.
 * @post On success `*size_out > 0`.
 * @post No FTL or device state is mutated.
 *
 * @note Thread-safe (pure computation over immutable geometry).
 *
 * @see ra8_ftl_checkpoint_save
 * @see ra8_ftl_checkpoint_load
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ftl_checkpoint_size(const ra8_ftl_t* ftl, uint32_t* size_out);

/**
 * @brief Serialise the FTL's mapping state into a caller buffer.
 *
 * @details
 * Writes a self-describing checkpoint of the volatile mapping tables (`map` and
 * `pblocks`) into `buf`: a versioned little-endian header (magic, exact length,
 * and geometry), individually encoded map and physical-block records, and a
 * CRC-32/ISO-HDLC trailer. Persisting this blob to a non-volatile region of
 * the underlying device and reloading it after a reset (see
 * ::ra8_ftl_checkpoint_load) is what lets logical data survive a power cycle --
 * the FTL keeps no on-media metadata of its own, so without a checkpoint a cold
 * re-init cannot resolve which physical block holds which logical block.
 *
 * Version 1 is canonical across architectures: no native object representation
 * or struct padding is copied. The pre-versioned native-layout format is
 * deliberately not migrated because its producer ABI cannot be proven from the
 * bytes; load identifies either legacy byte order and fails closed with
 * ::k_ra8_err_not_supported.
 *
 * @param[in]  ftl     Initialised FTL handle.
 * @param[out] buf     Destination buffer (>= ::ra8_ftl_checkpoint_size bytes).
 * @param[in]  buf_len Capacity of `buf` in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Checkpoint written into `buf`.
 * @retval k_ra8_err_null_ptr        `ftl` or `buf` was NULL.
 * @retval k_ra8_err_not_initialized `ftl` was not initialised.
 * @retval k_ra8_err_invalid_size    `buf_len` is smaller than the checkpoint.
 * @retval k_ra8_err_invalid_arg     Output aliases FTL tables or scratch.
 * @retval k_ra8_err_invalid_state   Live mapping invariants are corrupt.
 *
 * @pre `ftl` was initialised by ::ra8_ftl_init.
 * @pre `buf` is writable for at least `buf_len` bytes and does not overlap the
 *      FTL map, physical-block table, or scratch block.
 * @post On success `buf[0 .. checkpoint_size)` holds a loadable checkpoint.
 * @post On any non-ok return `buf` is left unchanged.
 *
 * @note Not thread-safe with respect to the data path.
 *
 * @see ra8_ftl_checkpoint_load
 * @see ra8_ftl_checkpoint_size
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ftl_checkpoint_save(const ra8_ftl_t* ftl, uint8_t* buf, uint32_t buf_len);

/**
 * @brief Restore FTL mapping state from a checkpoint produced by save.
 *
 * @details
 * Validates `buf` as an exact-length checkpoint whose version and geometry
 * match `ftl`, verifies its CRC, and rejects out-of-range or duplicate map
 * entries, invalid states, unreferenced LIVE blocks, and references to non-LIVE
 * blocks. Validation uses fixed windows in the FTL's caller-owned scratch block
 * before the saved `map` and `pblocks` values are committed, so a
 * freshly ::ra8_ftl_init handle resumes the exact mapping it had when the
 * checkpoint was taken. Call ::ra8_ftl_init first (to re-bind the underlying
 * device and re-establish geometry), then this to overwrite the cold-start
 * tables with the persisted state; the underlying data blocks are untouched, so
 * a subsequent read of any logical block returns its pre-reset contents.
 *
 * Legacy native-layout checkpoints are recognized in either byte order but
 * rejected as unsupported: their producer ABI and padding cannot be recovered
 * safely from the blob.
 *
 * @param[in,out] ftl     Handle freshly initialised by ::ra8_ftl_init.
 * @param[in]     buf     Checkpoint buffer from ::ra8_ftl_checkpoint_save.
 * @param[in]     buf_len Number of valid bytes in `buf`.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Mapping state restored.
 * @retval k_ra8_err_null_ptr        `ftl` or `buf` was NULL.
 * @retval k_ra8_err_not_initialized `ftl` was not initialised.
 * @retval k_ra8_err_invalid_size    `buf_len` is not the exact encoded length.
 * @retval k_ra8_err_invalid_state   The buffer is not an FTL checkpoint (bad
 *                                   magic or mapping invariant).
 * @retval k_ra8_err_invalid_arg     The checkpoint geometry does not match
 *                                   `ftl`, or input aliases live/scratch state.
 * @retval k_ra8_err_not_supported   Unknown version or recognized legacy ABI.
 * @retval k_ra8_err_crc_mismatch    Checkpoint bytes fail their CRC-32 trailer.
 *
 * @pre `ftl` was re-initialised by ::ra8_ftl_init over the retained device.
 * @pre `buf` holds exactly one checkpoint and does not overlap the FTL map,
 *      physical-block table, or scratch block.
 * @post On success `map`/`pblocks` mirror the checkpointed state.
 * @post On any non-ok return the cold-start tables are left as ::ra8_ftl_init
 *       set them.
 *
 * @note Not thread-safe with respect to the data path.
 *
 * @see ra8_ftl_checkpoint_save
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ftl_checkpoint_load(ra8_ftl_t* ftl, const uint8_t* buf, uint32_t buf_len);

/* =============================================================================
 * Mount lifecycle
 * =============================================================================
 */

/**
 * @struct ra8_ftl_cfg_t
 * @brief Declarative FTL geometry for ::ra8_ftl_mount.
 *
 * @details
 * The house `init(handle, const cfg_t*)` form used by ::ra8_cache_store_init,
 * ::ra8_keycache_init and ::ra8_vmem_init, and the shape that lets the FTL own
 * the checkpoint home instead of asking the caller to fence it off by hand.
 *
 * The caller states how many blocks at the **end** of the underlying device
 * belong to the FTL's own checkpoint (`reserved_tail_blocks`); the FTL reads
 * `block_count` from ::ra8_io_blockdev_get_caps and derives its managed span as
 * `block_count - reserved_tail_blocks`. That is the arithmetic a caller
 * previously had to get right by deliberately under-reporting
 * `physical_blocks` to ::ra8_ftl_init: understate it and the FTL relocates a
 * block over its own checkpoint, overstate it and the checkpoint slot is
 * unreachable.
 *
 * `ckbuf` is the staging buffer ::ra8_ftl_mount and ::ra8_ftl_sync use to move
 * the checkpoint between the caller's tables and the reserved tail. It must be
 * at least `reserved_tail_blocks * 512` bytes (a whole number of blocks, since
 * the underlying device transfers blocks) and at least
 * ::ra8_ftl_checkpoint_size bytes; mount checks both and refuses rather than
 * discovering it at sync time. It must not overlap `map`, `pblocks` or
 * `scratch`.
 *
 * @invariant `reserved_tail_blocks >= 1`.
 * @invariant `ckbuf_len >= reserved_tail_blocks * 512`.
 *
 * @since 0.1.0
 */
typedef struct {
  const ra8_io_blockdev_t* raw;                  /**< Underlying erase-before-write dev. */
  uint16_t*                map;                  /**< `logical_blocks` map entries.      */
  ra8_ftl_pblock_t*        pblocks;              /**< One entry per managed phys block.  */
  uint8_t*                 scratch;              /**< 512-byte copy-on-write scratch.    */
  uint8_t*                 ckbuf;                /**< Checkpoint staging buffer.         */
  uint32_t                 ckbuf_len;            /**< Bytes available in `ckbuf`.        */
  uint32_t                 logical_blocks;       /**< Blocks to present upward (>= 1).   */
  uint32_t                 reserved_tail_blocks; /**< Tail blocks kept for the ckpt.     */
} ra8_ftl_cfg_t;

/**
 * @brief Mount an FTL over a device, resuming a checkpoint if one is present.
 *
 * @details
 * The lifecycle entry point: geometry in one struct, the checkpoint slot owned
 * by the FTL, and cold start versus resume decided from the medium rather than
 * by the caller.
 *
 * 1. Reads `block_count` from the underlying device and derives the managed
 *    span as `block_count - cfg->reserved_tail_blocks`, so the caller cannot
 *    hand the FTL a span that overlaps its own checkpoint.
 * 2. Runs the same validation and cold-start table reset as ::ra8_ftl_init over
 *    that derived span, and records the checkpoint home on the handle.
 * 3. Reads the reserved tail. A tail that is **entirely the medium erase value**
 *    is treated as "no checkpoint yet" and the mount completes cold: every
 *    logical block unmapped, every physical block FREE. Otherwise the bytes are
 *    handed to ::ra8_ftl_checkpoint_load and the mapping resumes exactly as it
 *    stood at the last ::ra8_ftl_sync.
 *
 * A tail that is neither blank nor a loadable checkpoint fails the mount and is
 * **not** silently reformatted: a corrupt or foreign checkpoint over live data
 * is the caller's decision, not the FTL's. ::ra8_ftl_init plus one
 * ::ra8_ftl_sync is the deliberate reformat path.
 *
 * Nothing is erased or programmed here, so mounting a device is read-only with
 * respect to the medium.
 *
 * @param[out] ftl Handle to mount; caller zero-initialises it before use.
 * @param[in]  cfg Geometry and caller storage (see ::ra8_ftl_cfg_t).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Mounted; cold-started or resumed.
 * @retval k_ra8_err_null_ptr        `ftl`, `cfg`, or a `cfg` pointer was NULL.
 * @retval k_ra8_err_invalid_size    `logical_blocks` was zero, `ckbuf_len` is
 *                                   below the reserved tail or the checkpoint,
 *                                   or the checkpoint cannot fit the tail.
 * @retval k_ra8_err_invalid_arg     `reserved_tail_blocks` was zero, the device
 *                                   is no larger than the reserved tail, the
 *                                   device is read-only, `erase_unit_blocks`
 *                                   != 1, or no spare block remains.
 * @retval k_ra8_err_not_initialized No backend is bound to `cfg->raw`.
 * @retval k_ra8_err_invalid_state   The tail is neither blank nor a checkpoint.
 * @retval k_ra8_err_not_supported   The tail holds a legacy checkpoint ABI.
 * @retval k_ra8_err_crc_mismatch    The tail checkpoint fails its CRC trailer.
 * @retval (other)                   Propagated underlying read error.
 *
 * @pre `cfg` and every pointer in it out-live every call through the FTL.
 * @pre `cfg->pblocks` has one entry per managed physical block, which is
 *      `block_count - cfg->reserved_tail_blocks`, and was zero-initialised.
 * @pre `cfg->ckbuf` does not overlap `map`, `pblocks` or `scratch`.
 * @post On success the handle is mounted and ::ra8_ftl_as_blockdev may bind it.
 * @post On any non-ok return `ftl` is left unbound, so a failed mount cannot be
 *       mistaken for a cold-started one.
 * @post No block of the underlying device is erased or programmed.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @see ra8_ftl_sync
 * @see ra8_ftl_unmount
 * @see ra8_ftl_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ftl_mount(ra8_ftl_t* ftl, const ra8_ftl_cfg_t* cfg);

/**
 * @brief Persist the live mapping into the mount's reserved tail.
 *
 * @details
 * Serialises the volatile `map` and `pblocks` tables with
 * ::ra8_ftl_checkpoint_save into the staging buffer, erases the reserved tail,
 * and programs the checkpoint into it. After a successful sync the mapping
 * survives a power cycle: the next ::ra8_ftl_mount over the same device resumes
 * it. The staging buffer is filled with the medium erase value first, so the
 * bytes past the checkpoint are programmed as blank rather than as whatever the
 * previous sync left in the buffer.
 *
 * Only the reserved tail is touched; no data block and no mapping state is
 * mutated, so a sync is safe to repeat and safe to call when nothing changed.
 *
 * Requires a handle brought up by ::ra8_ftl_mount. A handle initialised by
 * ::ra8_ftl_init owns no checkpoint home and is refused with
 * ::k_ra8_err_invalid_state; that caller drives ::ra8_ftl_checkpoint_save and
 * its own storage directly.
 *
 * @param[in,out] ftl Mounted FTL handle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Checkpoint programmed into the tail.
 * @retval k_ra8_err_null_ptr        `ftl` was NULL.
 * @retval k_ra8_err_not_initialized `ftl` is not initialised.
 * @retval k_ra8_err_invalid_state   `ftl` was not brought up by
 *                                   ::ra8_ftl_mount, or the live mapping
 *                                   invariants are corrupt.
 * @retval k_ra8_err_invalid_size    The checkpoint outgrew the staging buffer.
 * @retval (other)                   Propagated underlying erase/program error.
 *
 * @pre `ftl` was mounted by ::ra8_ftl_mount.
 * @pre The device is idle (no concurrent access).
 * @post On success the reserved tail holds a checkpoint of the live mapping.
 * @post On any non-ok return the mapping and every data block are unchanged;
 *       the tail may hold a partially written checkpoint, which the next mount
 *       rejects rather than loads.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @see ra8_ftl_mount
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ftl_sync(ra8_ftl_t* ftl);

/**
 * @brief Sync the mapping and release the mount.
 *
 * @details
 * Runs ::ra8_ftl_sync and, only if that succeeds, unbinds the handle: the
 * underlying device, the caller storage and the checkpoint home are forgotten
 * and the presented capacity drops to zero, so a block device still bound by
 * ::ra8_ftl_as_blockdev rejects every subsequent transfer with
 * ::k_ra8_err_out_of_range instead of reaching a stale mapping. No caller
 * storage is freed or cleared; nothing was allocated.
 *
 * A failing sync leaves the FTL mounted and returns the error, so the caller
 * can retry rather than lose the mapping it was trying to persist.
 *
 * @param[in,out] ftl Mounted FTL handle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Mapping persisted and handle released.
 * @retval k_ra8_err_null_ptr        `ftl` was NULL.
 * @retval k_ra8_err_not_initialized `ftl` is not initialised.
 * @retval k_ra8_err_invalid_state   `ftl` was not brought up by
 *                                   ::ra8_ftl_mount.
 * @retval (other)                   Propagated ::ra8_ftl_sync failure; the
 *                                   handle stays mounted.
 *
 * @pre `ftl` was mounted by ::ra8_ftl_mount.
 * @pre No transfer is in flight on the presented block device.
 * @post On success `ftl` is unbound and ::ra8_ftl_as_blockdev refuses it.
 * @post On any non-ok return `ftl` is still mounted and usable.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @see ra8_ftl_mount
 * @see ra8_ftl_sync
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ftl_unmount(ra8_ftl_t* ftl);

#ifdef __cplusplus
}
#endif
