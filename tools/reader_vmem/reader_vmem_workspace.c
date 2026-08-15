/**
 * @file reader_vmem_workspace.c
 * @brief Checked split-region planning for reader_vmem cache RAM
 *
 * @details
 * The planner has no storage ownership and no process-global state. Distinct
 * callers may bind distinct backings concurrently; the standalone CLI chooses
 * one explicit composition-root BSS backing for its single cache instance.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "reader_vmem_internal.h"

/**
 * @brief Multiply two sizes without wrapping.
 * @details Performs the division guard before the only multiplication.
 * @param[in] left First factor.
 * @param[in] right Second factor.
 * @param[out] result Receives the exact product on success.
 * @return Whether the product is representable by `size_t`.
 * @retval true Exact product was stored.
 * @retval false Product would overflow.
 * @pre @p result is non-null and writable.
 * @pre Both factors may be any representable size.
 * @post Success initializes @p result exactly once.
 * @post Failure leaves @p result unchanged.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_size_mul(size_t left, size_t right, size_t* result)
{
  if (left != 0U && right > (SIZE_MAX / left)) {
    return false;
  }
  *result = left * right;
  return true;
}

/**
 * @brief Place one maximally aligned region after a prior high-water.
 * @details Computes bounded padding, aligned start, and next high-water without overflow.
 * @param[in] cursor Previous one-past high-water offset.
 * @param[in] bytes Region byte count.
 * @param[out] offset Receives aligned region start.
 * @param[out] next Receives one-past region end.
 * @return Whether both output coordinates are representable.
 * @retval true Outputs describe a complete aligned region.
 * @retval false Padding or end arithmetic would overflow.
 * @pre @p offset and @p next are non-null and writable.
 * @pre Maximum fundamental alignment is non-zero.
 * @post Success initializes both outputs with `offset <= next`.
 * @post Failure leaves both outputs unchanged.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_region(size_t cursor, size_t bytes, size_t* offset, size_t* next)
{
  const size_t alignment = _Alignof(max_align_t);
  const size_t remainder = cursor % alignment;
  const size_t padding   = (remainder == 0U) ? 0U : alignment - remainder;
  if (cursor > (SIZE_MAX - padding) || (cursor + padding) > (SIZE_MAX - bytes)) {
    return false;
  }
  *offset = cursor + padding;
  *next   = *offset + bytes;
  return true;
}

bool priv_rv_workspace_require(uint32_t budget, rv_workspace_need_t* need)
{
  if (budget == 0U || need == nullptr) {
    return false;
  }
  rv_workspace_need_t result = {};
  if (!internal_size_mul((size_t)budget, (size_t)k_rv_frame_bytes, &result.frame_bytes) ||
      !internal_size_mul((size_t)budget, sizeof(ra8_vmem_frame_t), &result.meta_bytes) ||
      !internal_size_mul((size_t)budget, sizeof(ra8_vmem_key_t), &result.key_bytes) ||
      !internal_size_mul((size_t)k_rv_bucket_count, sizeof(int32_t), &result.bucket_bytes)) {
    return false;
  }
  size_t cursor = 0U;
  if (!internal_region(cursor, result.frame_bytes, &result.frame_offset, &cursor) ||
      !internal_region(cursor, result.meta_bytes, &result.meta_offset, &cursor) ||
      !internal_region(cursor, result.key_bytes, &result.key_offset, &cursor) ||
      !internal_region(cursor, result.bucket_bytes, &result.bucket_offset, &cursor)) {
    return false;
  }
  result.total_bytes = cursor;
  *need              = result;
  return true;
}

/**
 * @brief Check one region lies wholly inside a workspace high-water.
 * @details Uses subtractive bounds checks and enforces maximum alignment.
 * @param[in] offset Region start relative to backing.
 * @param[in] bytes Region byte count.
 * @param[in] total Workspace high-water byte count.
 * @return Whether the half-open region is aligned and wholly bounded.
 * @retval true Region fits within `[0,total)` or is a valid empty endpoint.
 * @retval false Offset, length, or alignment is invalid.
 * @pre All values are offsets into one conceptual caller backing.
 * @pre Maximum fundamental alignment is non-zero.
 * @post No state changes.
 * @post No pointer is formed or dereferenced.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_region_valid(size_t offset, size_t bytes, size_t total)
{
  return (offset <= total) && (bytes <= (total - offset)) &&
         ((offset % _Alignof(max_align_t)) == 0U);
}

bool priv_rv_workspace_bind(void*                      backing,
                            size_t                     backing_bytes,
                            const rv_workspace_need_t* need,
                            rv_workspace_t*            workspace)
{
  if (backing == nullptr || need == nullptr || workspace == nullptr || need->total_bytes == 0U ||
      need->total_bytes > backing_bytes || (((uintptr_t)backing % _Alignof(max_align_t)) != 0U) ||
      !internal_region_valid(need->frame_offset, need->frame_bytes, need->total_bytes) ||
      !internal_region_valid(need->meta_offset, need->meta_bytes, need->total_bytes) ||
      !internal_region_valid(need->key_offset, need->key_bytes, need->total_bytes) ||
      !internal_region_valid(need->bucket_offset, need->bucket_bytes, need->total_bytes)) {
    return false;
  }
  if ((need->frame_offset + need->frame_bytes) > need->meta_offset ||
      (need->meta_offset + need->meta_bytes) > need->key_offset ||
      (need->key_offset + need->key_bytes) > need->bucket_offset) {
    return false;
  }
  uint8_t* const bytes = (uint8_t*)backing;
  *workspace           = (rv_workspace_t){
    .frame_mem = &bytes[need->frame_offset],
    .meta      = (ra8_vmem_frame_t*)&bytes[need->meta_offset],
    .keys      = (ra8_vmem_key_t*)&bytes[need->key_offset],
    .buckets   = (int32_t*)&bytes[need->bucket_offset],
  };
  return true;
}
