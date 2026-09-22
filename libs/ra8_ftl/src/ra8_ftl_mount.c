/**
 * @file ra8_ftl_mount.c
 * @brief FTL mount lifecycle -- mount / sync / unmount over a reserved tail.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ::ra8_ftl_mount, ::ra8_ftl_sync and ::ra8_ftl_unmount: the half of
 * the FTL that owns where its checkpoint lives. ::ra8_ftl_init records a span
 * the caller derived and stops there, which left every consumer partitioning
 * the medium by hand and driving raw erase/program at a metadata LBA on the
 * device the FTL was managing (issue #763). Here the FTL reads the device's own
 * block count, keeps `reserved_tail_blocks` at the end for itself, and decides
 * cold start versus resume by looking at that tail.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
 * @enum ra8_ftl_mount_const_t
 * @brief Mount-path sizing constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_ftl_mount_min_tail = 1, /**< Smallest reserved tail (one block). */
} ra8_ftl_mount_const_t;

/**
 * @brief Forget every binding on a handle, leaving caller storage untouched.
 *
 * @details
 * Zeroes the device pointer, the caller storage pointers, the checkpoint home
 * and both block counts. Dropping `logical_blocks` to zero is what makes an
 * already-bound ::ra8_io_blockdev_t refuse every transfer with
 * ::k_ra8_err_out_of_range instead of reaching a stale mapping, so a released
 * mount cannot be used through a block device the caller still holds.
 *
 * @param[in,out] ftl Handle to unbind.
 *
 * @return None.
 *
 * @pre `ftl` is non-NULL.
 * @post `ftl->raw` is NULL and `ftl->logical_blocks` is zero.
 * @post No caller storage is written.
 *
 * @note Not thread-safe with respect to the same FTL.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_unbind(ra8_ftl_t* ftl)
{
  ftl->raw             = nullptr;
  ftl->map             = nullptr;
  ftl->pblocks         = nullptr;
  ftl->scratch         = nullptr;
  ftl->ckbuf           = nullptr;
  ftl->ckbuf_len       = 0U;
  ftl->logical_blocks  = 0U;
  ftl->physical_blocks = 0U;
  ftl->reserved_tail   = 0U;
}

/**
 * @brief Validate a mount configuration before any device access.
 *
 * @details
 * Rejects a null cfg or any null pointer inside it, a zero logical-block count,
 * a zero reserved tail, and a staging buffer that cannot hold the whole
 * reserved tail. The tail-sized buffer requirement is checked here rather than
 * at sync time so a mount either works or says why.
 *
 * @param[in] cfg Configuration to validate.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Configuration is structurally valid.
 * @retval k_ra8_err_null_ptr     `cfg` or a pointer inside it was NULL.
 * @retval k_ra8_err_invalid_size `logical_blocks` was zero, or `ckbuf_len` is
 *                                below `reserved_tail_blocks * 512`.
 * @retval k_ra8_err_invalid_arg  `reserved_tail_blocks` was zero.
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
  RA8_CHECK_NULL_PTR(cfg->ckbuf, s_tag, "cfg->ckbuf must not be nullptr");
  if (cfg->logical_blocks == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (cfg->reserved_tail_blocks < (uint32_t)k_ra8_ftl_mount_min_tail) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->ckbuf_len <
      (cfg->reserved_tail_blocks * (uint32_t)k_ra8_io_block_size_bytes)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Read the reserved tail into the staging buffer.
 *
 * @details
 * The tail starts at `physical_blocks`, immediately after the span the FTL
 * manages, and runs to the end of the device. One transfer of
 * `reserved_tail` blocks.
 *
 * @param[in] ftl Handle whose geometry and checkpoint home are recorded.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Tail bytes are in `ftl->ckbuf`.
 * @retval (other) Propagated underlying read error.
 *
 * @pre `ftl->ckbuf` holds at least `reserved_tail * 512` bytes.
 * @post On success the tail bytes are staged; no device state is mutated.
 *
 * @note Not thread-safe with respect to the same FTL.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_read_tail(const ra8_ftl_t* ftl)
{
  return ra8_io_blockdev_read(ftl->raw, ftl->physical_blocks, ftl->reserved_tail, ftl->ckbuf);
}

/**
 * @brief Report whether the staged tail bytes are entirely the erase value.
 *
 * @details
 * A tail that reads back blank means no checkpoint has ever been programmed
 * there, which is the cold-start case. Any non-erase byte means the tail claims
 * to hold a checkpoint and is handed to ::ra8_ftl_checkpoint_load, whose own
 * magic, geometry and CRC checks decide whether it really does.
 *
 * @param[in] ftl Handle whose `ckbuf` holds the staged tail.
 *
 * @return bool true when every staged byte equals the medium erase value.
 *
 * @pre ::internal_read_tail succeeded.
 * @post No state is mutated.
 *
 * @note Thread-safe (pure comparison over the staging buffer).
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_tail_is_blank(const ra8_ftl_t* ftl)
{
  const uint32_t bytes = ftl->reserved_tail * (uint32_t)k_ra8_io_block_size_bytes;
  for (uint32_t i = 0; i < bytes; ++i) {
    if (ftl->ckbuf[i] != ftl->erase_value) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Resume a checkpoint from the reserved tail, or cold-start on a blank
 *        tail.
 *
 * @details
 * Split out of ::ra8_ftl_mount so that function stays inside the statement
 * budget. Sizes the checkpoint for the mounted geometry, refuses a staging
 * buffer or a reserved tail that cannot hold it, reads the tail, and either
 * accepts it as blank or loads it.
 *
 * @param[in,out] ftl Handle already initialised over the derived span, with the
 *                    checkpoint home recorded.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Cold start accepted, or mapping resumed.
 * @retval k_ra8_err_invalid_size The checkpoint does not fit the staging buffer
 *                                or the reserved tail.
 * @retval (other)                Propagated read or ::ra8_ftl_checkpoint_load
 *                                error.
 *
 * @pre `ftl` was initialised by ::ra8_ftl_init this call.
 * @post On success `map`/`pblocks` are either cold-start or checkpointed state.
 * @post No block of the underlying device is erased or programmed.
 *
 * @note Not thread-safe with respect to the same FTL.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_resume(ra8_ftl_t* ftl)
{
  uint32_t        need = 0U;
  const ra8_err_t sz   = ra8_ftl_checkpoint_size(ftl, &need);
  if (sz != k_ra8_ok) {
    return sz;
  }
  const uint32_t tail_bytes = ftl->reserved_tail * (uint32_t)k_ra8_io_block_size_bytes;
  if ((need > ftl->ckbuf_len) || (need > tail_bytes)) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t rd = internal_read_tail(ftl);
  if (rd != k_ra8_ok) {
    return rd;
  }
  if (internal_tail_is_blank(ftl)) {
    return k_ra8_ok;
  }
  return ra8_ftl_checkpoint_load(ftl, ftl->ckbuf, need);
}

ra8_err_t ra8_ftl_mount(ra8_ftl_t* ftl, const ra8_ftl_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  const ra8_err_t vc = internal_validate_cfg(cfg);
  if (vc != k_ra8_ok) {
    return vc;
  }
  ra8_io_blockdev_caps_t caps = {};
  const ra8_err_t        cq   = ra8_io_blockdev_get_caps(cfg->raw, &caps);
  if (cq != k_ra8_ok) {
    return cq;
  }
  if (caps.block_count <= cfg->reserved_tail_blocks) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t managed = caps.block_count - cfg->reserved_tail_blocks;
  const ra8_err_t init   = ra8_ftl_init(ftl,
                                        cfg->raw,
                                        cfg->map,
                                        cfg->logical_blocks,
                                        cfg->pblocks,
                                        managed,
                                        cfg->scratch);
  if (init != k_ra8_ok) {
    return init;
  }
  ftl->ckbuf         = cfg->ckbuf;
  ftl->ckbuf_len     = cfg->ckbuf_len;
  ftl->reserved_tail = cfg->reserved_tail_blocks;
  const ra8_err_t rs = internal_resume(ftl);
  if (rs != k_ra8_ok) {
    internal_unbind(ftl);
    return rs;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_ftl_sync(ra8_ftl_t* ftl)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  if (ftl->raw == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if ((ftl->reserved_tail == 0U) || (ftl->ckbuf == nullptr)) {
    return k_ra8_err_invalid_state;
  }
  uint32_t        need = 0U;
  const ra8_err_t sz   = ra8_ftl_checkpoint_size(ftl, &need);
  if (sz != k_ra8_ok) {
    return sz;
  }
  const uint32_t tail_bytes = ftl->reserved_tail * (uint32_t)k_ra8_io_block_size_bytes;
  if ((need > ftl->ckbuf_len) || (need > tail_bytes)) {
    return k_ra8_err_invalid_size;
  }
  (void)memset(ftl->ckbuf, (int)ftl->erase_value, (size_t)tail_bytes);
  const ra8_err_t sv = ra8_ftl_checkpoint_save(ftl, ftl->ckbuf, tail_bytes);
  if (sv != k_ra8_ok) {
    return sv;
  }
  const ra8_err_t er =
    ra8_io_blockdev_erase(ftl->raw, ftl->physical_blocks, ftl->reserved_tail);
  if (er != k_ra8_ok) {
    return er;
  }
  return ra8_io_blockdev_write(ftl->raw, ftl->physical_blocks, ftl->reserved_tail, ftl->ckbuf);
}

ra8_err_t ra8_ftl_unmount(ra8_ftl_t* ftl)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  const ra8_err_t sy = ra8_ftl_sync(ftl);
  if (sy != k_ra8_ok) {
    return sy;
  }
  internal_unbind(ftl);
  return k_ra8_ok;
}
