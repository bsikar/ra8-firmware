/**
 * @file ra8_ftl_mount.c
 * @brief FTL mount lifecycle -- the checkpoint gets a place to live.
 * @ingroup grp_storage
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * ::ra8_ftl_init takes the physical block count from its caller, so a caller
 * keeping a checkpoint on the same medium has to under-report the device and
 * then drive raw block writes at the blocks it withheld. This module inverts
 * that: the caller declares how many blocks to keep back, the FTL reads the
 * device's real size from ::ra8_io_blockdev_get_caps and derives its own span
 * below the reserved tail, and mount/sync/unmount own the checkpoint's place
 * on the medium. Composed entirely from the public `ra8_ftl_*` and
 * `ra8_io_blockdev_*` entry points; no FTL internals are reached into.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_ftl.h"
#include "ra8_io_blockdev.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_ftl_mount";

/**
 * @brief Validate the pointer + sizing fields of a mount configuration.
 *
 * @details
 * Single-condition checks only. Rejects any null storage pointer, a zero
 * logical-block count, a reserved tail below ::k_ra8_ftl_reserved_tail_min,
 * and a zero staging capacity. Geometry against the real device is checked
 * separately, once its capabilities are known.
 *
 * @param[in] cfg Configuration handed to ::ra8_ftl_mount.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Every field is safe to act on.
 * @retval k_ra8_err_null_ptr     `cfg` or one of its pointers was NULL.
 * @retval k_ra8_err_invalid_size A count or capacity was below its floor.
 *
 * @pre None.
 * @post No state is mutated.
 *
 * @note Thread-safe (pure validation).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_cfg(const ra8_ftl_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->raw, s_tag, "cfg->raw must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->map, s_tag, "cfg->map must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->pblocks, s_tag, "cfg->pblocks must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->scratch, s_tag, "cfg->scratch must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->checkpoint, s_tag, "cfg->checkpoint must not be nullptr");
  if (cfg->logical_blocks == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (cfg->reserved_tail_blocks < (uint32_t)k_ra8_ftl_reserved_tail_min) {
    return k_ra8_err_invalid_size;
  }
  if (cfg->checkpoint_bytes == 0U) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Split a device's block count into an FTL span and a reserved tail.
 *
 * @details
 * The tail sits at the top of the medium, so the FTL span is everything below
 * it and the first reserved block index equals that span. The device must be
 * large enough for the tail, the logical blocks, and at least one spare, and
 * the staging buffer must cover a whole tail (programs are block-granular).
 *
 * @param[in]  caps      Underlying device capabilities.
 * @param[in]  cfg       Mount configuration.
 * @param[out] phys_out  Receives the physical blocks handed to the FTL.
 * @param[out] tail_out  Receives the reserved tail size in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Split computed.
 * @retval k_ra8_err_null_ptr     An argument was NULL.
 * @retval k_ra8_err_invalid_arg  The device cannot host this geometry.
 * @retval k_ra8_err_invalid_size The staging buffer is smaller than the tail.
 *
 * @pre `caps` was filled by ::ra8_io_blockdev_get_caps.
 * @post On success `*phys_out >= cfg->logical_blocks + k_ra8_ftl_min_spare`.
 * @post No device state is mutated.
 *
 * @note Thread-safe (pure computation).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_split(const ra8_io_blockdev_caps_t* caps,
                                const ra8_ftl_cfg_t*          cfg,
                                uint32_t*                     phys_out,
                                uint32_t*                     tail_out)
{
  RA8_CHECK_NULL_PTR(caps, s_tag, "caps must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA8_CHECK_NULL_PTR(phys_out, s_tag, "phys_out must not be nullptr");
  RA8_CHECK_NULL_PTR(tail_out, s_tag, "tail_out must not be nullptr");
  if (caps->block_count <= cfg->reserved_tail_blocks) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t phys = caps->block_count - cfg->reserved_tail_blocks;
  if (phys < cfg->logical_blocks + (uint32_t)k_ra8_ftl_min_spare) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t tail_bytes = cfg->reserved_tail_blocks * (uint32_t)caps->logical_block_bytes;
  if (cfg->checkpoint_bytes < tail_bytes) {
    return k_ra8_err_invalid_size;
  }
  *phys_out = phys;
  *tail_out = tail_bytes;
  return k_ra8_ok;
}

/**
 * @brief Report whether a buffer reads back entirely as the erase value.
 *
 * @details
 * A reserved tail that is wholly blank has never been programmed, which is the
 * one condition under which a mount may cold-start without discarding
 * anything. The loop is statically bounded by `len`.
 *
 * @param[in] buf   Buffer to scan.
 * @param[in] len   Bytes to scan.
 * @param[in] erase Medium erase value.
 *
 * @return bool True when every scanned byte equals `erase`.
 *
 * @pre `buf` is readable for `len` bytes.
 * @post No state is mutated.
 *
 * @note Thread-safe (pure read).
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_all_erased(const uint8_t* buf, uint32_t len, uint8_t erase)
{
  for (uint32_t i = 0; i < len; ++i) {
    if (buf[i] != erase) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Re-derive and check the reserved tail a mounted handle recorded.
 *
 * @details
 * Guards ::ra8_ftl_sync against a handle that was mounted and then re-bound by
 * ::ra8_ftl_init with a different geometry: the recorded tail must still start
 * exactly where the FTL's physical span ends and still lie inside the device.
 * Also re-reads the medium erase value, which the pad uses.
 *
 * @param[in]  ftl       Handle to check.
 * @param[out] tail_out  Receives the reserved tail size in bytes.
 * @param[out] erase_out Receives the medium erase value.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Handle is mounted and self-consistent.
 * @retval k_ra8_err_null_ptr        An argument was NULL.
 * @retval k_ra8_err_not_initialized No device is bound to `ftl`.
 * @retval k_ra8_err_invalid_state   Never mounted, or the tail no longer fits
 *                                   the device and geometry it is bound to.
 * @retval k_ra8_err_invalid_size    The staging buffer is smaller than the
 *                                   tail.
 *
 * @pre None.
 * @post No device state is mutated.
 *
 * @note Not thread-safe with respect to the same FTL.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mounted(const ra8_ftl_t* ftl, uint32_t* tail_out, uint8_t* erase_out)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  RA8_CHECK_NULL_PTR(tail_out, s_tag, "tail_out must not be nullptr");
  RA8_CHECK_NULL_PTR(erase_out, s_tag, "erase_out must not be nullptr");
  if (ftl->raw == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (ftl->checkpoint == nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (ftl->reserved_blocks == 0U) {
    return k_ra8_err_invalid_state;
  }
  if (ftl->reserved_lba != ftl->physical_blocks) {
    return k_ra8_err_invalid_state;
  }
  ra8_io_blockdev_caps_t caps = {};
  const ra8_err_t        cq   = ra8_io_blockdev_get_caps(ftl->raw, &caps);
  if (cq != k_ra8_ok) {
    return cq;
  }
  if (ftl->reserved_lba + ftl->reserved_blocks > caps.block_count) {
    return k_ra8_err_invalid_state;
  }
  const uint32_t tail_bytes = ftl->reserved_blocks * (uint32_t)caps.logical_block_bytes;
  if (ftl->ck_bytes < tail_bytes) {
    return k_ra8_err_invalid_size;
  }
  *tail_out  = tail_bytes;
  *erase_out = caps.erase_value;
  return k_ra8_ok;
}

/**
 * @brief Resolve a freshly initialised handle against the reserved tail.
 *
 * @details
 * Reads the tail into the staging buffer. Blank means cold start. Anything
 * else is offered to ::ra8_ftl_checkpoint_load, whose error is returned as-is
 * rather than swallowed, because presenting a full medium as an empty one is
 * worse than refusing to mount it.
 *
 * @param[in,out] ftl        Handle already initialised over the FTL span.
 * @param[in]     tail_bytes Reserved tail size in bytes.
 * @param[in]     erase      Medium erase value.
 * @param[out]    state_out  Receives how the mount resolved.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Tail was blank, or its checkpoint loaded.
 * @retval others   The read's or the load's own error.
 *
 * @pre `ftl` was initialised over the blocks below the tail.
 * @post On k_ra8_ok `*state_out` distinguishes cold from resumed.
 *
 * @note Not thread-safe with respect to the same FTL.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_resolve(ra8_ftl_t*             ftl,
                                  uint32_t               tail_bytes,
                                  uint8_t                erase,
                                  ra8_ftl_mount_state_t* state_out)
{
  const ra8_err_t rd =
    ra8_io_blockdev_read(ftl->raw, ftl->reserved_lba, ftl->reserved_blocks, ftl->checkpoint);
  if (rd != k_ra8_ok) {
    return rd;
  }
  if (internal_all_erased(ftl->checkpoint, tail_bytes, erase)) {
    *state_out = k_ra8_ftl_mount_cold;
    return k_ra8_ok;
  }
  uint32_t        need = 0;
  const ra8_err_t sz   = ra8_ftl_checkpoint_size(ftl, &need);
  if (sz != k_ra8_ok) {
    return sz;
  }
  if (need > tail_bytes) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t ld = ra8_ftl_checkpoint_load(ftl, ftl->checkpoint, need);
  if (ld != k_ra8_ok) {
    return ld;
  }
  *state_out = k_ra8_ftl_mount_resumed;
  return k_ra8_ok;
}

ra8_err_t ra8_ftl_mount(ra8_ftl_t* ftl, const ra8_ftl_cfg_t* cfg, ra8_ftl_mount_state_t* state_out)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  const ra8_err_t va = internal_validate_cfg(cfg);
  if (va != k_ra8_ok) {
    return va;
  }
  ra8_io_blockdev_caps_t caps = {};
  const ra8_err_t        cq   = ra8_io_blockdev_get_caps(cfg->raw, &caps);
  if (cq != k_ra8_ok) {
    return cq;
  }
  uint32_t        phys       = 0;
  uint32_t        tail_bytes = 0;
  const ra8_err_t sp         = internal_split(&caps, cfg, &phys, &tail_bytes);
  if (sp != k_ra8_ok) {
    return sp;
  }
  const ra8_err_t in =
    ra8_ftl_init(ftl, cfg->raw, cfg->map, cfg->logical_blocks, cfg->pblocks, phys, cfg->scratch);
  if (in != k_ra8_ok) {
    return in;
  }
  ftl->checkpoint       = cfg->checkpoint;
  ftl->ck_bytes = cfg->checkpoint_bytes;
  ftl->reserved_lba     = phys;
  ftl->reserved_blocks  = cfg->reserved_tail_blocks;

  ra8_ftl_mount_state_t state = k_ra8_ftl_mount_cold;
  const ra8_err_t       rv    = internal_resolve(ftl, tail_bytes, caps.erase_value, &state);
  if (rv != k_ra8_ok) {
    *ftl = (ra8_ftl_t){};
    return rv;
  }
  if (state_out != nullptr) {
    *state_out = state;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_ftl_sync(ra8_ftl_t* ftl)
{
  uint32_t        tail_bytes = 0;
  uint8_t         erase      = 0;
  const ra8_err_t mt         = internal_mounted(ftl, &tail_bytes, &erase);
  if (mt != k_ra8_ok) {
    return mt;
  }
  uint32_t        need = 0;
  const ra8_err_t sz   = ra8_ftl_checkpoint_size(ftl, &need);
  if (sz != k_ra8_ok) {
    return sz;
  }
  if (need > tail_bytes) {
    return k_ra8_err_invalid_size;
  }
  (void)memset(ftl->checkpoint, (int)erase, (size_t)tail_bytes);
  const ra8_err_t sv = ra8_ftl_checkpoint_save(ftl, ftl->checkpoint, tail_bytes);
  if (sv != k_ra8_ok) {
    return sv;
  }
  const ra8_err_t er = ra8_io_blockdev_erase(ftl->raw, ftl->reserved_lba, ftl->reserved_blocks);
  if (er != k_ra8_ok) {
    return er;
  }
  const ra8_err_t wr =
    ra8_io_blockdev_write(ftl->raw, ftl->reserved_lba, ftl->reserved_blocks, ftl->checkpoint);
  if (wr != k_ra8_ok) {
    return wr;
  }
  return ra8_io_blockdev_sync(ftl->raw);
}

ra8_err_t ra8_ftl_unmount(ra8_ftl_t* ftl)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  const ra8_err_t sy = ra8_ftl_sync(ftl);
  if (sy != k_ra8_ok) {
    return sy;
  }
  *ftl = (ra8_ftl_t){};
  return k_ra8_ok;
}
