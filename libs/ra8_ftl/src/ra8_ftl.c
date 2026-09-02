/**
 * @file ra8_ftl.c
 * @brief Flash Translation Layer implementation -- copy-on-write +
 * wear-levelling.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ::ra8_ftl_init, ::ra8_ftl_as_blockdev, and the
 * ::ra8_io_blockdev_iface vtable the FTL presents upward. Reads resolve the
 * logical->physical map and forward to the underlying device (or synthesise the
 * erase value for unmapped blocks). Writes allocate the least-erased free
 * physical block, erase it, program the data, and re-point the map -- never
 * touching a non-blank block.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_ftl.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_backend.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_ftl";

/**
 * @enum ra8_ftl_impl_const_t
 * @brief Implementation-private layout constants.
 *
 * @details
 * Single-block transfer count and the required underlying erase granularity.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_ftl_one_block      = 1,           /**< Count for a single-block raw transfer.  */
  k_ra8_ftl_req_erase_unit = 1,           /**< Required raw `erase_unit_blocks` value. */
  k_ra8_ftl_count_max      = 0xFFFFFFFFU, /**< Sentinel: highest possible erase count. */
} ra8_ftl_impl_const_t;

/* =============================================================================
 * Physical-block allocation (wear-levelling + reclamation)
 * =============================================================================
 */

/**
 * @brief Reclaim every STALE physical block by erasing it back to FREE.
 *
 * @details
 * Scans the physical metadata and, for each STALE block, issues a single-block
 * erase on the underlying device, advances its erase count, and marks it FREE.
 * The loop is statically bounded by `physical_blocks`. Called only when the
 * allocator finds no FREE block, so reclamation cost is amortised.
 *
 * @param[in,out] ftl Initialised FTL handle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok        Reclamation pass completed (some blocks may stay
 * live).
 * @retval k_ra8_err_null_ptr `ftl` was NULL.
 * @retval other          Propagated underlying-device erase error.
 *
 * @pre `ftl` is initialised.
 * @pre `ftl->pblocks` reflects the live mapping.
 * @post Every block that was STALE on entry is FREE on a successful return.
 * @post On an erase error the scan stops and the error is propagated.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_reclaim_stale(ra8_ftl_t* ftl)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  RA8_CHECK_NULL_PTR(ftl->raw, s_tag, "raw must not be nullptr");
  for (uint32_t p = 0; p < ftl->physical_blocks; ++p) {
    if (ftl->pblocks[p].state != (uint8_t)k_ra8_ftl_pstate_stale) {
      continue;
    }
    const ra8_err_t e = ra8_io_blockdev_erase(ftl->raw, p, (uint32_t)k_ra8_ftl_one_block);
    if (e != k_ra8_ok) {
      return e;
    }
    ftl->pblocks[p].erase_count += 1U;
    ftl->pblocks[p].state = (uint8_t)k_ra8_ftl_pstate_free;
  }
  return k_ra8_ok;
}

/**
 * @brief Select the least-erased FREE physical block (wear-levelling).
 *
 * @details
 * Linear scan bounded by `physical_blocks` tracking the FREE block with the
 * lowest erase count. Spreading allocation toward the least-worn free block is
 * the wear-levelling policy. Returns ::k_ra8_err_no_data if no FREE block
 * exists so the caller can trigger reclamation and retry.
 *
 * @param[in]  ftl Initialised FTL handle.
 * @param[out] out Receives the chosen physical block index.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           A free block was chosen into `*out`.
 * @retval k_ra8_err_null_ptr `ftl` or `out` was NULL.
 * @retval k_ra8_err_no_data  No FREE physical block is available.
 *
 * @pre `ftl` is initialised.
 * @pre `out` is writable.
 * @post On success `*out < ftl->physical_blocks` and that block is FREE.
 * @post No FTL or device state is mutated.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_pick_free(const ra8_ftl_t* ftl, uint32_t* out)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  bool     found = false;
  uint32_t best  = 0;
  uint32_t bestc = 0;
  for (uint32_t p = 0; p < ftl->physical_blocks; ++p) {
    if (ftl->pblocks[p].state != (uint8_t)k_ra8_ftl_pstate_free) {
      continue;
    }
    if (found) {
      if (ftl->pblocks[p].erase_count >= bestc) {
        continue;
      }
    }
    found = true;
    best  = p;
    bestc = ftl->pblocks[p].erase_count;
  }
  if (!found) {
    return k_ra8_err_no_data;
  }
  *out = best;
  return k_ra8_ok;
}

/**
 * @brief Allocate a blank physical block, reclaiming stale blocks if needed.
 *
 * @details
 * Tries ::internal_pick_free; if no free block exists it runs ::internal_reclaim_stale
 * and retries exactly once. Two attempts suffice because the FTL guarantees at
 * least one spare block, so after reclamation a free block always exists. The
 * chosen block is erased (advancing its erase count) so it is blank before the
 * caller programs it.
 *
 * @param[in,out] ftl Initialised FTL handle.
 * @param[out]    out Receives the allocated, erased physical block index.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           A blank block was allocated into `*out`.
 * @retval k_ra8_err_null_ptr `ftl` or `out` was NULL.
 * @retval k_ra8_err_no_mem   No block available even after reclamation.
 * @retval other             Propagated underlying erase error.
 *
 * @pre `ftl` is initialised with at least one spare block.
 * @pre `out` is writable.
 * @post On success block `*out` is FREE-tagged and reads back as erase value.
 * @post On any non-ok return no map entry is changed.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_alloc_blank(ra8_ftl_t* ftl, uint32_t* out)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  uint32_t  phys = 0;
  ra8_err_t pick = internal_pick_free(ftl, &phys);
  if (pick == k_ra8_err_no_data) {
    const ra8_err_t rec = internal_reclaim_stale(ftl);
    if (rec != k_ra8_ok) {
      return rec;
    }
    pick = internal_pick_free(ftl, &phys);
  }
  if (pick == k_ra8_err_no_data) {
    return k_ra8_err_no_mem;
  }
  /* internal_pick_free with non-null args returns only k_ra8_ok or
   * k_ra8_err_no_data. */
  if (pick != k_ra8_ok) {
    return pick; /* GCOVR_EXCL_LINE -- pick_free result set is ok or no-data only */
  }
  const ra8_err_t e = ra8_io_blockdev_erase(ftl->raw, phys, (uint32_t)k_ra8_ftl_one_block);
  if (e != k_ra8_ok) {
    return e;
  }
  ftl->pblocks[phys].erase_count += 1U;
  *out = phys;
  return k_ra8_ok;
}

/* =============================================================================
 * Single-logical-block read / write
 * =============================================================================
 */

/**
 * @brief Read one logical block, resolving the map (or erase value if
 * unmapped).
 *
 * @details
 * If the logical block is unmapped the destination is filled with the medium
 * erase value (the block has never been written). Otherwise the underlying
 * physical block is read through.
 *
 * @param[in]  ftl Initialised FTL handle.
 * @param[in]  lbn Logical block number (< logical_blocks).
 * @param[out] dst 512-byte destination buffer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Block resolved into `dst`.
 * @retval k_ra8_err_null_ptr `ftl` or `dst` was NULL.
 * @retval other             Propagated underlying read error.
 *
 * @pre `ftl` is initialised.
 * @pre `lbn < ftl->logical_blocks`.
 * @post On success `dst` holds the logical block's current contents.
 * @post No FTL or device state is mutated.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_read_one(const ra8_ftl_t* ftl, uint32_t lbn, uint8_t* dst)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  RA8_CHECK_NULL_PTR(dst, s_tag, "dst must not be nullptr");
  const uint16_t phys = ftl->map[lbn];
  if (phys == (uint16_t)k_ra8_ftl_unmapped) {
    (void)memset(dst, (int)ftl->erase_value, (size_t)k_ra8_io_block_size_bytes);
    return k_ra8_ok;
  }
  return ra8_io_blockdev_read(ftl->raw, phys, (uint32_t)k_ra8_ftl_one_block, dst);
}

/**
 * @brief Write one logical block via copy-on-write relocation.
 *
 * @details
 * Allocates a blank physical block, programs `src` into it, then re-points the
 * map and marks the previous physical block (if any) STALE for later
 * reclamation. The old block is never overwritten, so the erase-before-write
 * contract holds. On a program failure the freshly erased block is left FREE
 * and the map is unchanged, so the prior data remains intact.
 *
 * @param[in,out] ftl Initialised FTL handle.
 * @param[in]     lbn Logical block number (< logical_blocks).
 * @param[in]     src 512-byte source buffer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Logical block now reflects `src`.
 * @retval k_ra8_err_null_ptr `ftl` or `src` was NULL.
 * @retval k_ra8_err_no_mem   No physical block available to relocate into.
 * @retval other             Propagated underlying erase/program error.
 *
 * @pre `ftl` is initialised and not read-only.
 * @pre `lbn < ftl->logical_blocks`.
 * @post On success `map[lbn]` points at a LIVE block holding `src`.
 * @post On any non-ok return the prior mapping for `lbn` is preserved.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_write_one(ra8_ftl_t* ftl, uint32_t lbn, const uint8_t* src)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  uint32_t        phys  = 0;
  const ra8_err_t alloc = internal_alloc_blank(ftl, &phys);
  if (alloc != k_ra8_ok) {
    return alloc;
  }
  const ra8_err_t prog = ra8_io_blockdev_write(ftl->raw, phys, (uint32_t)k_ra8_ftl_one_block, src);
  if (prog != k_ra8_ok) {
    return prog;
  }
  const uint16_t old = ftl->map[lbn];
  if (old != (uint16_t)k_ra8_ftl_unmapped) {
    ftl->pblocks[old].state = (uint8_t)k_ra8_ftl_pstate_stale;
  }
  ftl->pblocks[phys].state = (uint8_t)k_ra8_ftl_pstate_live;
  ftl->map[lbn]            = (uint16_t)phys;
  return k_ra8_ok;
}

/* =============================================================================
 * Presented vtable
 * =============================================================================
 */

/**
 * @brief Reject an out-of-range presented `[lba, lba+count)` range.
 *
 * @details
 * Two single-condition checks (no compound decision) guarding against count
 * overflow and a range that runs past `logical_blocks`.
 *
 * @param[in] ftl   Initialised FTL handle.
 * @param[in] lba   First logical block address.
 * @param[in] count Number of logical blocks in the range.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Range is within the presented capacity.
 * @retval k_ra8_err_out_of_range Range exceeds `logical_blocks`.
 *
 * @pre `ftl` is initialised.
 * @pre `ftl->logical_blocks` reflects the presented size.
 * @post No state is mutated.
 * @post The return reflects only the range/capacity comparison.
 *
 * @note Thread-safe (pure comparison).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_bounds(const ra8_ftl_t* ftl, uint32_t lba, uint32_t count)
{
  if (count > ftl->logical_blocks) {
    return k_ra8_err_out_of_range;
  }
  if (lba > ftl->logical_blocks - count) {
    return k_ra8_err_out_of_range;
  }
  return k_ra8_ok;
}

/**
 * @brief Presented vtable: read `count` logical blocks at `lba`.
 *
 * @details
 * Bounds-checks then resolves each logical block one at a time through
 * ::internal_read_one. The per-block loop is statically bounded by `count`.
 *
 * @param[in]  ctx   FTL handle (as a void cookie).
 * @param[in]  lba   First logical block address.
 * @param[in]  count Number of logical blocks to read.
 * @param[out] buf   Destination buffer (>= `count * 512` bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Blocks resolved into `buf`.
 * @retval k_ra8_err_null_ptr     `ctx` or `buf` was NULL.
 * @retval k_ra8_err_out_of_range Range past presented capacity.
 * @retval other                 Propagated underlying read error.
 *
 * @pre `ctx` is an initialised FTL.
 * @pre `buf` is writable for `count * 512` bytes.
 * @post On success `buf` mirrors the logical contents.
 * @post On failure `buf` may be partially written; callers retry the range.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_dev_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  const ra8_ftl_t* ftl = (const ra8_ftl_t*)ctx;
  const ra8_err_t  b   = internal_bounds(ftl, lba, count);
  if (b != k_ra8_ok) {
    return b;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const size_t    off = (size_t)i * (size_t)k_ra8_io_block_size_bytes;
    const ra8_err_t e   = internal_read_one(ftl, lba + i, &buf[off]);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Presented vtable: write `count` logical blocks at `lba`.
 *
 * @details
 * Bounds-checks then relocates each logical block one at a time through
 * ::internal_write_one (copy-on-write). The per-block loop is statically bounded by
 * `count`.
 *
 * @param[in] ctx   FTL handle (as a void cookie).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of logical blocks to write.
 * @param[in] buf   Source buffer (>= `count * 512` bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Blocks written.
 * @retval k_ra8_err_null_ptr     `ctx` or `buf` was NULL.
 * @retval k_ra8_err_out_of_range Range past presented capacity.
 * @retval other                 Propagated underlying erase/program error.
 *
 * @pre `ctx` is an initialised FTL.
 * @pre `buf` is readable for `count * 512` bytes.
 * @post On success logical blocks `[lba, lba+count)` equal `buf`.
 * @post On failure earlier blocks in the range may already be committed.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_dev_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  ra8_ftl_t*      ftl = (ra8_ftl_t*)ctx;
  const ra8_err_t b   = internal_bounds(ftl, lba, count);
  if (b != k_ra8_ok) {
    return b;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const size_t    off = (size_t)i * (size_t)k_ra8_io_block_size_bytes;
    const ra8_err_t e   = internal_write_one(ftl, lba + i, &buf[off]);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Presented vtable: erase `count` logical blocks at `lba`.
 *
 * @details
 * For the free-overwrite device the FTL presents, an erase simply unmaps each
 * logical block: its current physical block becomes STALE (reclaimed later) and
 * a subsequent read returns the erase value. The per-block loop is statically
 * bounded by `count`.
 *
 * @param[in] ctx   FTL handle (as a void cookie).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of logical blocks to erase.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Logical blocks unmapped.
 * @retval k_ra8_err_null_ptr     `ctx` was NULL.
 * @retval k_ra8_err_out_of_range Range past presented capacity.
 *
 * @pre `ctx` is an initialised FTL.
 * @pre `count` is non-zero for any observable effect.
 * @post On success each block in range reads back as the erase value.
 * @post On failure no mapping is changed.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_dev_erase(void* ctx, uint32_t lba, uint32_t count)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  ra8_ftl_t*      ftl = (ra8_ftl_t*)ctx;
  const ra8_err_t b   = internal_bounds(ftl, lba, count);
  if (b != k_ra8_ok) {
    return b;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint16_t old = ftl->map[lba + i];
    if (old != (uint16_t)k_ra8_ftl_unmapped) {
      ftl->pblocks[old].state = (uint8_t)k_ra8_ftl_pstate_stale;
      ftl->map[lba + i]       = (uint16_t)k_ra8_ftl_unmapped;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Presented vtable: report the FTL's free-overwrite capabilities.
 *
 * @details
 * Reports `logical_blocks` blocks of a medium that needs no erase-before-write
 * (the FTL hides that), with the underlying erase value preserved so the FAT
 * bridge handles formatting consistently.
 *
 * @param[in]  ctx FTL handle (as a const void cookie).
 * @param[out] out Capabilities snapshot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           `*out` populated.
 * @retval k_ra8_err_null_ptr `ctx` or `out` was NULL.
 *
 * @pre `ctx` is an initialised FTL.
 * @pre `out` is writable.
 * @post On success `*out` advertises `must_erase_before_write == false`.
 * @post No FTL or device state is mutated.
 *
 * @note Thread-safe (pure read).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_dev_get_caps(const void* ctx, ra8_io_blockdev_caps_t* out)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  const ra8_ftl_t* ftl         = (const ra8_ftl_t*)ctx;
  out->block_count             = ftl->logical_blocks;
  out->erase_unit_blocks       = (uint32_t)k_ra8_ftl_one_block;
  out->program_size_bytes      = (uint32_t)k_ra8_io_block_size_bytes;
  out->logical_block_bytes     = (uint16_t)k_ra8_io_block_size_bytes;
  out->erase_value             = ftl->erase_value;
  out->must_erase_before_write = false;
  out->read_only               = false;
  return k_ra8_ok;
}

/**
 * @brief Presented vtable: flush the underlying device.
 *
 * @details
 * The FTL holds no write buffering of its own (each write commits immediately),
 * so it simply forwards the sync to the underlying device.
 *
 * @param[in] ctx FTL handle (as a void cookie).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Underlying device flushed (or nothing to flush).
 * @retval k_ra8_err_null_ptr `ctx` was NULL.
 * @retval other             Propagated underlying sync error.
 *
 * @pre `ctx` is an initialised FTL.
 * @pre The device is idle (no concurrent access).
 * @post On success the medium reflects every prior successful write.
 * @post No FTL state is mutated.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_dev_sync(void* ctx)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  const ra8_ftl_t* ftl = (const ra8_ftl_t*)ctx;
  RA8_CHECK_NULL_PTR(ftl->raw, s_tag, "raw must not be nullptr");
  return ra8_io_blockdev_sync(ftl->raw);
}

/** @brief Presented FTL vtable -- a clean free-overwrite block device. */
static const ra8_io_blockdev_iface_t s_ftl_iface = {
  .read     = internal_dev_read,
  .write    = internal_dev_write,
  .erase    = internal_dev_erase,
  .get_caps = internal_dev_get_caps,
  .sync     = internal_dev_sync,
};

/* =============================================================================
 * Init / bind / diagnostics
 * =============================================================================
 */

/**
 * @brief Validate the underlying device's capabilities for FTL use.
 *
 * @details
 * Single-condition checks (no compound decisions): the device must not be
 * read-only, must have `erase_unit_blocks == 1`, must hold at least
 * `physical_blocks` blocks, and must leave at least one spare over
 * `logical_blocks`. Also snapshots the erase value into `*erase_out`.
 *
 * @param[in]  caps            Underlying device capabilities.
 * @param[in]  logical_blocks  Blocks the FTL will present.
 * @param[in]  physical_blocks Blocks the caller claims the device has.
 * @param[out] erase_out       Receives the medium erase value.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Underlying device is suitable.
 * @retval k_ra8_err_null_ptr    `erase_out` was NULL.
 * @retval k_ra8_err_invalid_arg Device unsuitable (read-only, wrong erase unit,
 *                              too small, or no spare block).
 *
 * @pre `caps` was filled by ::ra8_io_blockdev_get_caps.
 * @pre `erase_out` is writable.
 * @post On success `*erase_out` holds the medium erase byte.
 * @post No device state is mutated.
 *
 * @note Thread-safe (pure validation).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_check_caps(const ra8_io_blockdev_caps_t* caps,
                                     uint32_t                      logical_blocks,
                                     uint32_t                      physical_blocks,
                                     uint8_t*                      erase_out)
{
  RA8_CHECK_NULL_PTR(caps, s_tag, "caps must not be nullptr");
  RA8_CHECK_NULL_PTR(erase_out, s_tag, "erase_out must not be nullptr");
  if (caps->read_only) {
    return k_ra8_err_invalid_arg;
  }
  if (caps->erase_unit_blocks != (uint32_t)k_ra8_ftl_req_erase_unit) {
    return k_ra8_err_invalid_arg;
  }
  if (caps->block_count < physical_blocks) {
    return k_ra8_err_invalid_arg;
  }
  if (physical_blocks < logical_blocks + (uint32_t)k_ra8_ftl_min_spare) {
    return k_ra8_err_invalid_arg;
  }
  *erase_out = caps->erase_value;
  return k_ra8_ok;
}

/**
 * @brief Cold-start the FTL bookkeeping tables.
 *
 * @details
 * Sets every logical map entry to ::k_ra8_ftl_unmapped and every physical block
 * to FREE with a zero erase count. Both loops are statically bounded by their
 * respective table sizes.
 *
 * @param[in,out] ftl Handle whose `map`/`pblocks`/sizes are already populated.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Tables initialised to the cold-start state.
 * @retval k_ra8_err_null_ptr `ftl` was NULL.
 *
 * @pre `ftl->map` and `ftl->pblocks` point at correctly sized caller storage.
 * @pre `ftl->logical_blocks` and `ftl->physical_blocks` are set.
 * @post Every logical block is unmapped.
 * @post Every physical block is FREE with `erase_count == 0`.
 *
 * @note Not thread-safe with respect to the same FTL.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_reset_tables(ra8_ftl_t* ftl)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  RA8_CHECK_NULL_PTR(ftl->map, s_tag, "map must not be nullptr");
  for (uint32_t l = 0; l < ftl->logical_blocks; ++l) {
    ftl->map[l] = (uint16_t)k_ra8_ftl_unmapped;
  }
  for (uint32_t p = 0; p < ftl->physical_blocks; ++p) {
    ftl->pblocks[p].state       = (uint8_t)k_ra8_ftl_pstate_free;
    ftl->pblocks[p].erase_count = 0U;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate the pointer + sizing arguments to ::ra8_ftl_init.
 *
 * @details Split out of ::ra8_ftl_init so that function stays within the
 *          clang-tidy statement/complexity budget. Rejects any null storage
 *          pointer, a zero logical-block count, and a physical-block count past
 *          the static ::k_ra8_ftl_max_pblocks ceiling.
 *
 * @param[in] bd              FTL state to populate (non-NULL).
 * @param[in] raw             Backing erase-before-write block device
 * (non-NULL).
 * @param[in] map             Logical->physical map storage (non-NULL).
 * @param[in] pblocks         Per-physical-block metadata storage (non-NULL).
 * @param[in] scratch         Copy-on-write scratch buffer (non-NULL).
 * @param[in] logical_blocks  Logical block count (> 0).
 * @param[in] physical_blocks Physical block count (<= ::k_ra8_ftl_max_pblocks).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               All arguments valid.
 * @retval k_ra8_err_null_ptr     A required pointer argument was NULL.
 * @retval k_ra8_err_invalid_size @p logical_blocks is 0 or @p physical_blocks
 *                               exceeds the ceiling.
 *
 * @pre  The caller passes ::ra8_ftl_init's arguments verbatim.
 * @pre  Storage arrays out-live the FTL when non-NULL.
 * @post On k_ra8_ok every checked argument is safe to record.
 * @post On any non-ok return no state is mutated.
 *
 * @note Not thread-safe with respect to the same FTL.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_init_args(const ra8_ftl_t*         bd,
                                             const ra8_io_blockdev_t* raw,
                                             const uint16_t*          map,
                                             const ra8_ftl_pblock_t*  pblocks,
                                             const uint8_t*           scratch,
                                             uint32_t                 logical_blocks,
                                             uint32_t                 physical_blocks)
{
  RA8_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  RA8_CHECK_NULL_PTR(raw, s_tag, "raw must not be nullptr");
  RA8_CHECK_NULL_PTR(map, s_tag, "map must not be nullptr");
  RA8_CHECK_NULL_PTR(pblocks, s_tag, "pblocks must not be nullptr");
  RA8_CHECK_NULL_PTR(scratch, s_tag, "scratch must not be nullptr");
  if (logical_blocks == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (physical_blocks > (uint32_t)k_ra8_ftl_max_pblocks) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_ftl_init(ra8_ftl_t*               bd,
                       const ra8_io_blockdev_t* raw,
                       uint16_t*                map,
                       uint32_t                 logical_blocks,
                       ra8_ftl_pblock_t*        pblocks,
                       uint32_t                 physical_blocks,
                       uint8_t*                 scratch)
{
  const ra8_err_t va =
    internal_validate_init_args(bd, raw, map, pblocks, scratch, logical_blocks, physical_blocks);
  if (va != k_ra8_ok) {
    return va;
  }
  ra8_io_blockdev_caps_t caps = {};
  const ra8_err_t        cq   = ra8_io_blockdev_get_caps(raw, &caps);
  if (cq != k_ra8_ok) {
    return cq;
  }
  uint8_t         erase_value = 0;
  const ra8_err_t ck = internal_check_caps(&caps, logical_blocks, physical_blocks, &erase_value);
  if (ck != k_ra8_ok) {
    return ck;
  }
  bd->raw             = raw;
  bd->map             = map;
  bd->pblocks         = pblocks;
  bd->scratch         = scratch;
  bd->logical_blocks  = logical_blocks;
  bd->physical_blocks = physical_blocks;
  bd->erase_value     = erase_value;
  return internal_reset_tables(bd);
}

ra8_err_t ra8_ftl_as_blockdev(ra8_ftl_t* ftl, ra8_io_blockdev_t* out)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (ftl->raw == nullptr) {
    return k_ra8_err_not_initialized;
  }
  out->iface = &s_ftl_iface;
  out->ctx   = ftl;
  return k_ra8_ok;
}

ra8_err_t ra8_ftl_wear_stats(const ra8_ftl_t* ftl, uint32_t* max_out, uint32_t* min_out)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  RA8_CHECK_NULL_PTR(max_out, s_tag, "max_out must not be nullptr");
  RA8_CHECK_NULL_PTR(min_out, s_tag, "min_out must not be nullptr");
  if (ftl->pblocks == nullptr) {
    return k_ra8_err_not_initialized;
  }
  uint32_t hi = 0;
  uint32_t lo = (uint32_t)k_ra8_ftl_count_max;
  for (uint32_t p = 0; p < ftl->physical_blocks; ++p) {
    const uint32_t c = ftl->pblocks[p].erase_count;
    if (c > hi) {
      hi = c;
    }
    if (c < lo) {
      lo = c;
    }
  }
  *max_out = hi;
  *min_out = lo;
  return k_ra8_ok;
}

ra8_err_t ra8_ftl_phys_of(const ra8_ftl_t* ftl, uint32_t lbn, uint16_t* phys_out)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  RA8_CHECK_NULL_PTR(phys_out, s_tag, "phys_out must not be nullptr");
  if (ftl->map == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (lbn >= ftl->logical_blocks) {
    return k_ra8_err_out_of_range;
  }
  *phys_out = ftl->map[lbn];
  return k_ra8_ok;
}
