/**
 * @file ra_io_blockdev.c
 * @brief Block-device dispatcher -- forwards each public call into the bound
 *        backend vtable, and bridges any backend to an `ra_fs_backend_t`.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Stateless dispatcher: it validates the handle, then forwards through the
 * bound ::ra_io_blockdev_iface. A NULL optional callback maps to a defined
 * result (no erase => not-supported, no sync => success). The bridge installs
 * static trampolines whose `ctx` is the ::ra_io_blockdev_t itself.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_io_blockdev.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_io_blockdev_internal.h"
#include "ra_log.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_blockdev";

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Reject a handle that is NULL or has no backend bound.
 *
 * @details
 * Run on every dispatch path. Kept tiny so each public entry point stays well
 * under the NASA Power-of-10 Rule 4 sixty-line cap.
 *
 * @param[in] bd Candidate handle.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  `bd` is non-NULL with a bound backend.
 * @retval k_ra_err_null_ptr        `bd` was NULL.
 * @retval k_ra_err_not_initialized `bd->iface` was NULL (never bound).
 *
 * @pre None.
 * @pre None.
 * @post No state is mutated.
 * @post The return reflects only the binding state of `bd`.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_validate(const ra_io_blockdev_t* bd)
{
  if (bd == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (bd->iface == nullptr) {
    return k_ra_err_not_initialized;
  }
  return k_ra_ok;
}

/* =============================================================================
 * Public API -- dispatch through the bound backend
 * =============================================================================
 */

ra_err_t ra_io_blockdev_read(const ra_io_blockdev_t* bd, uint32_t lba, uint32_t count, uint8_t* buf)
{
  const ra_err_t v = internal_validate(bd);
  if (v != k_ra_ok) {
    return v;
  }
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  RA_CHECK_NULL_PTR(bd->iface->read, s_tag, "backend read op missing");
  return bd->iface->read(bd->ctx, lba, count, buf);
}

ra_err_t
ra_io_blockdev_write(const ra_io_blockdev_t* bd, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  const ra_err_t v = internal_validate(bd);
  if (v != k_ra_ok) {
    return v;
  }
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  RA_CHECK_NULL_PTR(bd->iface->write, s_tag, "backend write op missing");
  return bd->iface->write(bd->ctx, lba, count, buf);
}

ra_err_t ra_io_blockdev_erase(const ra_io_blockdev_t* bd, uint32_t lba, uint32_t count)
{
  const ra_err_t v = internal_validate(bd);
  if (v != k_ra_ok) {
    return v;
  }
  if (bd->iface->erase == nullptr) {
    return k_ra_err_not_supported;
  }
  return bd->iface->erase(bd->ctx, lba, count);
}

ra_err_t ra_io_blockdev_get_caps(const ra_io_blockdev_t* bd, ra_io_blockdev_caps_t* out)
{
  const ra_err_t v = internal_validate(bd);
  if (v != k_ra_ok) {
    return v;
  }
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA_CHECK_NULL_PTR(bd->iface->get_caps, s_tag, "backend get_caps op missing");
  return bd->iface->get_caps(bd->ctx, out);
}

ra_err_t ra_io_blockdev_sync(const ra_io_blockdev_t* bd)
{
  const ra_err_t v = internal_validate(bd);
  if (v != k_ra_ok) {
    return v;
  }
  if (bd->iface->sync == nullptr) {
    return k_ra_ok;
  }
  return bd->iface->sync(bd->ctx);
}

/* =============================================================================
 * ra_fs_backend_t bridge
 * =============================================================================
 */

/**
 * @brief `ra_fs` read trampoline -- forward into the bound block device.
 *
 * @details
 * Casts the `ra_fs` cookie back to the block-device handle and dispatches the
 * read.
 *
 * @param[in]  ctx   The ::ra_io_blockdev_t handle (as a void cookie).
 * @param[in]  lba   First logical block address.
 * @param[in]  count Number of blocks to read.
 * @param[out] buf   Destination buffer.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Blocks read.
 * @retval k_ra_err_null_ptr `ctx` or `buf` was NULL.
 * @retval k_ra_err_*        Propagated from ::ra_io_blockdev_read.
 *
 * @pre `ctx` is a bound ::ra_io_blockdev_t.
 * @pre `buf` is writable for `count * 512` bytes.
 * @post On success `buf` mirrors the device contents.
 * @post On failure `buf` is untouched by the fabric.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
static ra_err_t internal_fs_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx (blockdev) must not be nullptr");
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  return ra_io_blockdev_read((const ra_io_blockdev_t*)ctx, lba, count, buf);
}

/**
 * @brief `ra_fs` write trampoline -- forward into the bound block device.
 *
 * @details
 * Casts the `ra_fs` cookie back to the block-device handle and dispatches the
 * write.
 *
 * @param[in] ctx   The ::ra_io_blockdev_t handle (as a void cookie).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks to write.
 * @param[in] buf   Source buffer.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Blocks written.
 * @retval k_ra_err_null_ptr `ctx` or `buf` was NULL.
 * @retval k_ra_err_*        Propagated from ::ra_io_blockdev_write.
 *
 * @pre `ctx` is a bound ::ra_io_blockdev_t.
 * @pre `buf` is readable for `count * 512` bytes.
 * @post On success the device blocks equal `buf`.
 * @post On failure the device is untouched by the fabric.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
static ra_err_t internal_fs_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx (blockdev) must not be nullptr");
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  return ra_io_blockdev_write((const ra_io_blockdev_t*)ctx, lba, count, buf);
}

/**
 * @brief `ra_fs` capacity trampoline -- map block-device caps to (count, size).
 *
 * @details
 * Queries the block-device capabilities and reports `block_count` plus the
 * 512-byte logical block size `ra_fs` expects.
 *
 * @param[in]  ctx         The ::ra_io_blockdev_t handle (as a void cookie).
 * @param[out] block_count Total blocks reported to `ra_fs`.
 * @param[out] block_size  Bytes per block reported to `ra_fs` (always 512).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Capacity reported.
 * @retval k_ra_err_null_ptr Any pointer argument was NULL.
 * @retval k_ra_err_*        Propagated from ::ra_io_blockdev_get_caps.
 *
 * @pre `ctx` is a bound ::ra_io_blockdev_t.
 * @pre `block_count` and `block_size` are writable.
 * @post On success both outputs describe the medium.
 * @post On failure the outputs are untouched.
 *
 * @note Thread-safe (pure read of immutable backend state).
 *
 * @since 0.1.0
 */
static ra_err_t internal_fs_get_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx (blockdev) must not be nullptr");
  RA_CHECK_NULL_PTR(block_count, s_tag, "block_count must not be nullptr");
  RA_CHECK_NULL_PTR(block_size, s_tag, "block_size must not be nullptr");
  ra_io_blockdev_caps_t caps = {};
  const ra_err_t        e    = ra_io_blockdev_get_caps((const ra_io_blockdev_t*)ctx, &caps);
  if (e != k_ra_ok) {
    return e;
  }
  *block_count = caps.block_count;
  *block_size  = (uint32_t)caps.logical_block_bytes;
  return k_ra_ok;
}

/**
 * @brief `ra_fs` erase trampoline -- only advertise erase on zero-erase media.
 *
 * @details
 * `ra_fs`'s formatter requires an erased range to read back as `0x00`. Flash
 * media erase to `0xFF`, so for those this returns ::k_ra_err_not_supported and
 * the formatter falls back to writing zeros explicitly.
 *
 * @param[in] ctx   The ::ra_io_blockdev_t handle (as a void cookie).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks to erase.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Range erased to zero.
 * @retval k_ra_err_null_ptr      `ctx` was NULL.
 * @retval k_ra_err_not_supported Medium does not erase to zero.
 * @retval k_ra_err_*             Propagated from ::ra_io_blockdev_erase.
 *
 * @pre `ctx` is a bound ::ra_io_blockdev_t.
 * @pre `count` is non-zero for any observable effect.
 * @post On success the range reads back as `0x00`.
 * @post On failure the device is untouched.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
static ra_err_t internal_fs_erase(void* ctx, uint32_t lba, uint32_t count)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx (blockdev) must not be nullptr");
  const ra_io_blockdev_t* bd   = (const ra_io_blockdev_t*)ctx;
  ra_io_blockdev_caps_t   caps = {};
  const ra_err_t          e    = ra_io_blockdev_get_caps(bd, &caps);
  if (e != k_ra_ok) {
    return e;
  }
  if (caps.erase_value != (uint8_t)k_ra_io_erase_value_zero) {
    return k_ra_err_not_supported;
  }
  return ra_io_blockdev_erase(bd, lba, count);
}

ra_err_t ra_io_blockdev_as_fs_backend(const ra_io_blockdev_t* bd, ra_fs_backend_t* out)
{
  const ra_err_t v = internal_validate(bd);
  if (v != k_ra_ok) {
    return v;
  }
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  out->read_block   = internal_fs_read;
  out->write_block  = internal_fs_write;
  out->get_capacity = internal_fs_get_capacity;
  out->erase_blocks = internal_fs_erase;
  out->ctx          = (void*)(uintptr_t)bd;
  return k_ra_ok;
}
