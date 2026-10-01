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
 * data.
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
 * ::ra8_ftl_mount fills the same fields as ::ra8_ftl_init and additionally
 * records where the checkpoint lives (`reserved_lba`, `reserved_blocks`) and
 * the staging buffer it is serialised through. A handle initialised by
 * ::ra8_ftl_init alone leaves those zero, which is what makes ::ra8_ftl_sync
 * refuse it.
 *
 * @invariant `logical_blocks < physical_blocks` (at least one spare block).
 * @invariant `map` has `logical_blocks` entries; `pblocks` has
 *            `physical_blocks` entries.
 * @invariant `reserved_blocks == 0`, or `reserved_lba == physical_blocks` and
 *            the reserved tail lies inside the underlying device.
 *
 * @since 0.1.0
 */
typedef struct {
  const ra8_io_blockdev_t* raw;             /**< Underlying erase-before-write dev. */
  uint16_t*                map;             /**< logical->physical map (private).   */
  ra8_ftl_pblock_t*        pblocks;         /**< Per-physical metadata (private).   */
  uint8_t*                 scratch;         /**< 512-byte copy scratch (private).   */
  uint8_t*                 checkpoint;      /**< Checkpoint staging buf (private).  */
  uint32_t                 ck_bytes;        /**< Staging capacity (private).        */
  uint32_t                 logical_blocks;  /**< Blocks presented to FAT (private). */
  uint32_t                 physical_blocks; /**< Blocks in the raw dev (private).   */
  uint32_t                 reserved_lba;    /**< First reserved tail block (priv).  */
  uint32_t                 reserved_blocks; /**< Reserved tail length (private).    */
  uint8_t                  erase_value;     /**< Raw medium erase byte (private).   */
} ra8_ftl_t;

/* =============================================================================
 * Mount lifecycle
 * =============================================================================
 */

/**
 * @enum ra8_ftl_mount_const_t
 * @brief Sizing floors for the mount lifecycle.
 *
 * @details
 * The checkpoint has to live somewhere, so a mount reserves at least
 * ::k_ra8_ftl_reserved_tail_min block at the top of the underlying device and
 * hands the FTL only what is left below it.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_ftl_reserved_tail_min = 1, /**< Smallest reserved checkpoint tail. */
} ra8_ftl_mount_const_t;

/**
 * @enum ra8_ftl_mount_state_t
 * @brief How a successful ::ra8_ftl_mount resolved the medium.
 *
 * @details
 * A mount either found a loadable checkpoint in the reserved tail and resumed
 * the mapping it describes, or found the tail blank and cold-started. A tail
 * that holds something which is *not* a loadable checkpoint is neither: the
 * mount fails rather than discarding it (see ::ra8_ftl_mount).
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_ftl_mount_cold    = 0, /**< Reserved tail blank; tables cold-started. */
  k_ra8_ftl_mount_resumed = 1, /**< Checkpoint loaded; mapping resumed.       */
} ra8_ftl_mount_state_t;

/**
 * @struct ra8_ftl_cfg_t
 * @brief Declarative description of an FTL over a device with a reserved tail.
 *
 * @details
 * The house `init(handle, const cfg_t*)` form, and the reason it exists here:
 * ::ra8_ftl_init takes the physical block count from the caller, so a caller
 * that wants to keep a checkpoint on the same medium has to under-report the
 * device and then drive raw block writes at the blocks it withheld. Getting
 * that arithmetic wrong lets the FTL relocate a block on top of its own
 * checkpoint. Here the caller declares only how many blocks it wants **kept
 * back** (`reserved_tail_blocks`); the FTL reads the device's real size from
 * ::ra8_io_blockdev_get_caps and derives its own span, so the two can no longer
 * disagree.
 *
 * All storage is caller-owned and must out-live the mount, exactly as with
 * ::ra8_ftl_init. `checkpoint` is the staging buffer the checkpoint is
 * serialised through on the way to the medium; it is never read or written
 * outside a ::ra8_ftl_mount, ::ra8_ftl_sync or ::ra8_ftl_unmount call, and it
 * must not overlap `map`, `pblocks` or `scratch`.
 *
 * @invariant `logical_blocks >= 1`.
 * @invariant `reserved_tail_blocks >= ::k_ra8_ftl_reserved_tail_min`.
 * @invariant `checkpoint_bytes >= reserved_tail_blocks * 512`.
 *
 * @see ra8_ftl_mount
 * @since 0.1.0
 */
typedef struct {
  const ra8_io_blockdev_t* raw;                  /**< Underlying erase-before-write dev. */
  uint16_t*                map;                  /**< `logical_blocks` map entries.      */
  ra8_ftl_pblock_t*        pblocks;              /**< One entry per derived phys block.  */
  uint8_t*                 scratch;              /**< 512-byte copy scratch.             */
  uint8_t*                 checkpoint;           /**< Checkpoint staging buffer.         */
  uint32_t                 checkpoint_bytes;     /**< Capacity of `checkpoint`.          */
  uint32_t                 logical_blocks;       /**< Blocks to present to FAT.          */
  uint32_t                 reserved_tail_blocks; /**< Blocks kept back for the           */
                                                 /**< checkpoint, at the top of the dev. */
} ra8_ftl_cfg_t;

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

/**
 * @brief Mount an FTL over a device, keeping a tail of it for the checkpoint.
 *
 * @details
 * The lifecycle entry point ::ra8_ftl_init stops one step short of. It reads
 * the underlying device's real block count from ::ra8_io_blockdev_get_caps,
 * subtracts `cfg->reserved_tail_blocks`, and initialises the FTL over exactly
 * the blocks below that tail, so the FTL provably cannot relocate a block onto
 * its own metadata: the reserved blocks are outside its physical range by
 * construction rather than by the caller having passed a smaller number than
 * the device reports.
 *
 * It then reads the reserved tail. A tail that reads back entirely as the
 * medium erase value is an unwritten one: the mount cold-starts and reports
 * ::k_ra8_ftl_mount_cold. A tail holding a checkpoint that loads is resumed,
 * reporting ::k_ra8_ftl_mount_resumed, and every logical block written before
 * the last ::ra8_ftl_sync reads back intact. A tail holding anything else
 * fails the mount with the load's own error: a checkpoint that does not load
 * is never silently discarded, because doing so presents a full medium as an
 * empty one. Recovering from that is a deliberate act (erase the reserved
 * tail, then mount again).
 *
 * @param[out] ftl       FTL handle to mount; caller zero-initialises it.
 * @param[in]  cfg       Declarative configuration; see ::ra8_ftl_cfg_t.
 * @param[out] state_out Optional: receives how the mount resolved. May be NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Mounted; `*state_out` says how.
 * @retval k_ra8_err_null_ptr        `ftl`, `cfg`, or a `cfg` storage pointer
 *                                   was NULL.
 * @retval k_ra8_err_invalid_size    `logical_blocks` was zero,
 *                                   `reserved_tail_blocks` was below
 *                                   ::k_ra8_ftl_reserved_tail_min, or
 *                                   `checkpoint_bytes` is smaller than the
 *                                   reserved tail.
 * @retval k_ra8_err_invalid_arg     The device cannot host this geometry
 *                                   (reserved tail plus logical blocks plus a
 *                                   spare exceed its block count), or it is
 *                                   otherwise unsuitable (read-only, wrong
 *                                   erase unit).
 * @retval k_ra8_err_invalid_state   The reserved tail is not a checkpoint.
 * @retval k_ra8_err_crc_mismatch    The reserved tail failed its CRC trailer.
 * @retval k_ra8_err_not_supported   The reserved tail holds a checkpoint this
 *                                   build cannot load.
 *
 * @pre `cfg` and every buffer it names out-live the mount.
 * @pre `pblocks` has at least `block_count - reserved_tail_blocks` entries.
 * @post On success the FTL presents `logical_blocks` blocks and knows where
 *       its checkpoint lives, so ::ra8_ftl_sync needs no further arguments.
 * @post On any non-ok return `ftl` is left unbound.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @see ra8_ftl_sync
 * @see ra8_ftl_unmount
 * @see ra8_ftl_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ftl_mount(ra8_ftl_t* ftl, const ra8_ftl_cfg_t* cfg, ra8_ftl_mount_state_t* state_out);

/**
 * @brief Persist the current mapping into the reserved tail.
 *
 * @details
 * Serialises the FTL's volatile mapping tables with ::ra8_ftl_checkpoint_save
 * into the staging buffer the mount recorded, pads the remainder of the tail
 * with the medium erase value, erases the reserved blocks and programs them,
 * then syncs the underlying device. After this returns ok, a reset that loses
 * SRAM but retains the medium is recoverable: the next ::ra8_ftl_mount resumes
 * this exact mapping.
 *
 * Accepts only a handle that ::ra8_ftl_mount bound. A handle from
 * ::ra8_ftl_init alone has no reserved tail and no staging buffer, so there is
 * nowhere for the checkpoint to go and the call is refused rather than guessed
 * at.
 *
 * @param[in,out] ftl Mounted FTL handle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Checkpoint programmed into the tail.
 * @retval k_ra8_err_null_ptr        `ftl` was NULL.
 * @retval k_ra8_err_not_initialized `ftl` is not initialised.
 * @retval k_ra8_err_invalid_state   `ftl` was initialised but never mounted, or
 *                                   its recorded tail no longer matches the
 *                                   device and geometry it is bound to.
 * @retval k_ra8_err_invalid_size    The checkpoint does not fit the reserved
 *                                   tail.
 *
 * @pre `ftl` was mounted by ::ra8_ftl_mount.
 * @post On success the reserved tail holds a loadable checkpoint of the
 *       mapping as it stood at the call.
 * @post On any non-ok return the FTL mapping is unchanged; the reserved tail
 *       may have been erased.
 *
 * @note Not thread-safe with respect to the data path.
 *
 * @see ra8_ftl_mount
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ftl_sync(ra8_ftl_t* ftl);

/**
 * @brief Sync the mapping, then release the mount.
 *
 * @details
 * ::ra8_ftl_sync followed by unbinding the handle, which is the orderly end of
 * a mount: the medium is left recoverable and the handle is left as though it
 * had never been initialised, so a later call through it is refused instead of
 * reaching a device the caller believes it released. A failed sync aborts the
 * unmount and leaves the handle mounted, so the caller can retry rather than
 * lose the mapping silently.
 *
 * @param[in,out] ftl Mounted FTL handle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Checkpoint written and handle released.
 * @retval k_ra8_err_null_ptr        `ftl` was NULL.
 * @retval k_ra8_err_not_initialized `ftl` is not initialised.
 * @retval k_ra8_err_invalid_state   `ftl` was initialised but never mounted.
 * @retval k_ra8_err_invalid_size    The checkpoint does not fit the reserved
 *                                   tail.
 *
 * @pre `ftl` was mounted by ::ra8_ftl_mount.
 * @pre No block device produced by ::ra8_ftl_as_blockdev is still in use.
 * @post On success `ftl` is unbound and the medium holds a loadable
 *       checkpoint.
 * @post On any non-ok return `ftl` stays mounted.
 *
 * @note Not thread-safe with respect to the data path.
 *
 * @see ra8_ftl_sync
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ftl_unmount(ra8_ftl_t* ftl);

#ifdef __cplusplus
}
#endif
