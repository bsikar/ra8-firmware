/**
 * @file ra8_npu_loader.c
 * @brief On-target `.npub` Vela blob loader over `ra8_npu` (RA8P1-only)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Implementation of the `ra8_npu_loader.h` contract. The whole translation unit
 * is gated behind `RA8_HAS_NPU` so it compiles to nothing on the RA8D2 and is
 * built live only for the RA8P1 (`-DRA8_DEVICE_RA8P1`). It touches no NPU
 * register -- it parses the caller's `.npub` byte buffer (`ra8_npu_blob.h`) and
 * fills an ::ra8_npu_job_t -- so it carries no HUM citation and is fully
 * host-testable. Every guard is a single condition (no compound decision), so the
 * loader adds no MC/DC obligation; the host test pins the happy path and each
 * rejection.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_device.h"

#ifdef RA8_HAS_NPU

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_npu.h"
#include "ra8_npu_blob.h"
#include "ra8_npu_loader.h"

/**
 * @var s_tag
 * @brief Log component tag for the blob loader.
 * @details Passed to the `ra8_log_*` / `RA8_*` macros as the source tag.
 * @note Read-only literal; never modified.
 * @since 0.1.0
 */
static const char* s_tag = "NPU-LOAD";

/**
 * @enum ra8_npu_loader_const_t
 * @brief Local constants for arena alignment arithmetic.
 *
 * @details ::k_ra8_npu_loader_align_mask is `alignment - 1`, used to round a
 *          runtime-region offset up to ::k_ra8_npu_blob_arena_align. Kept as a
 *          named constant so the round-up carries no bare literal.
 *
 * @invariant ::k_ra8_npu_loader_align_mask == ::k_ra8_npu_blob_arena_align - 1.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_loader_align_mask =
    (uint32_t)k_ra8_npu_blob_arena_align - 1U, /**< RA8 npu loader align mask. */
} ra8_npu_loader_const_t;

/**
 * @brief Test that `[offset, offset + size)` lies within `limit`, overflow-safe.
 *
 * @details Two single-condition comparisons so no addition can wrap: first that
 *          the span is not larger than the limit, then that the offset leaves room
 *          for it. Used to bound every declared offset/size against the blob.
 *
 * @param[in] offset Start byte offset.
 * @param[in] size   Span length in bytes.
 * @param[in] limit  Exclusive upper bound (buffer length).
 *
 * @return `true` when the whole span fits within @p limit, else `false`.
 * @retval true  `offset + size <= limit` with no overflow.
 * @retval false The span would exceed @p limit.
 *
 * @pre @p limit is the trustworthy buffer length.
 * @pre @p offset and @p size are the values under test.
 * @post No input is modified; the function is pure.
 * @post The result is exact (no wraparound).
 *
 * @note Re-entrant and thread-safe.
 * @since 0.1.0
 */
static bool internal_npu_span_ok(uint32_t offset, uint32_t size, uint32_t limit)
{
  if (size > limit) {
    return false;
  }
  if (offset > (limit - size)) {
    return false;
  }
  return true;
}

/**
 * @brief Validate the fixed `.npub` header and return its located fields.
 *
 * @details Checks magic, version, and that the declared `total_bytes` and command
 *          stream fit within @p blob_bytes, then hands back the region count and
 *          command-stream location for the caller to act on.
 *
 * @param[in]  p          `.npub` byte buffer base.
 * @param[in]  blob_bytes Buffer length in bytes.
 * @param[out] out_total  Declared whole-blob length on success.
 * @param[out] out_rcount Region descriptor count on success.
 * @param[out] out_coff   Command-stream byte offset on success.
 * @param[out] out_cbytes Command-stream length on success.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Header valid; outputs populated.
 * @retval k_ra8_err_invalid_size @p blob_bytes below the header, or cmd length 0.
 * @retval k_ra8_err_invalid_arg Bad magic, version, or region count.
 * @retval k_ra8_err_out_of_range A declared span falls outside the buffer.
 *
 * @pre @p p addresses at least @p blob_bytes readable bytes.
 * @pre Every out-pointer is non-NULL (caller-guaranteed).
 * @post On success the four out-values locate the payload.
 * @post On failure no out-value is relied upon.
 *
 * @note Re-entrant; reads only.
 * @since 0.1.0
 */
static ra8_err_t internal_npu_check_header(const uint8_t* p,
                                           uint32_t       blob_bytes,
                                           uint32_t*      out_total,
                                           uint32_t*      out_rcount,
                                           uint32_t*      out_coff,
                                           uint32_t*      out_cbytes)
{
  if (blob_bytes < (uint32_t)k_ra8_npu_blob_header_bytes) {
    ra8_log_error(s_tag, "load: buffer smaller than header");
    return k_ra8_err_invalid_size;
  }
  if (ra8_npu_blob_read_word(p,
                             (uint32_t)k_ra8_npu_blob_word_magic *
                               (uint32_t)k_ra8_npu_blob_word_bytes) !=
      (uint32_t)k_ra8_npu_blob_magic) {
    ra8_log_error(s_tag, "load: bad magic");
    return k_ra8_err_invalid_arg;
  }
  if (ra8_npu_blob_read_word(p,
                             (uint32_t)k_ra8_npu_blob_word_version *
                               (uint32_t)k_ra8_npu_blob_word_bytes) !=
      (uint32_t)k_ra8_npu_blob_version) {
    ra8_log_error(s_tag, "load: unsupported version");
    return k_ra8_err_invalid_arg;
  }
  const uint32_t total = ra8_npu_blob_read_word(p,
                                                (uint32_t)k_ra8_npu_blob_word_total_bytes *
                                                  (uint32_t)k_ra8_npu_blob_word_bytes);
  if (!internal_npu_span_ok(0U, total, blob_bytes)) {
    ra8_log_error(s_tag, "load: total_bytes exceeds buffer");
    return k_ra8_err_out_of_range;
  }
  const uint32_t rcount = ra8_npu_blob_read_word(p,
                                                 (uint32_t)k_ra8_npu_blob_word_region_count *
                                                   (uint32_t)k_ra8_npu_blob_word_bytes);
  if (rcount > (uint32_t)k_ra8_npu_region_count) {
    ra8_log_error(s_tag, "load: too many regions");
    return k_ra8_err_invalid_arg;
  }
  const uint32_t coff   = ra8_npu_blob_read_word(p,
                                                 (uint32_t)k_ra8_npu_blob_word_cmd_offset *
                                                   (uint32_t)k_ra8_npu_blob_word_bytes);
  const uint32_t cbytes = ra8_npu_blob_read_word(p,
                                                 (uint32_t)k_ra8_npu_blob_word_cmd_bytes *
                                                   (uint32_t)k_ra8_npu_blob_word_bytes);
  if (cbytes == 0U) {
    ra8_log_error(s_tag, "load: empty command stream");
    return k_ra8_err_invalid_size;
  }
  if (!internal_npu_span_ok(coff, cbytes, total)) {
    ra8_log_error(s_tag, "load: command stream outside blob");
    return k_ra8_err_out_of_range;
  }
  *out_total  = total;
  *out_rcount = rcount;
  *out_coff   = coff;
  *out_cbytes = cbytes;
  return k_ra8_ok;
}

/**
 * @brief Fold the payload with FNV-1a and compare against the header checksum.
 *
 * @details Digests every byte from ::k_ra8_npu_blob_header_bytes to @p total,
 *          then compares against the stored `checksum` word. A single-condition
 *          mismatch check rejects a corrupted blob.
 *
 * @param[in] p     `.npub` byte buffer base.
 * @param[in] total Declared whole-blob length (already bounds-checked).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok The recomputed digest matches the stored checksum.
 * @retval k_ra8_err_checksum_mismatch The digest differs (blob corrupt).
 *
 * @pre @p total is within the buffer and >= the header size.
 * @pre @p p addresses at least @p total readable bytes.
 * @post No input is modified; reads only.
 * @post On success the payload integrity is proven for @p total bytes.
 *
 * @note Re-entrant; the loop is bounded by @p total (NASA Rule 2).
 * @since 0.1.0
 */
static ra8_err_t internal_npu_verify_checksum(const uint8_t* p, uint32_t total)
{
  uint32_t digest = (uint32_t)k_ra8_npu_blob_fnv_offset;
  for (uint32_t i = (uint32_t)k_ra8_npu_blob_header_bytes; i < total; i++) {
    digest = (digest ^ (uint32_t)p[i]) * (uint32_t)k_ra8_npu_blob_fnv_prime;
  }
  const uint32_t stored = ra8_npu_blob_read_word(p,
                                                 (uint32_t)k_ra8_npu_blob_word_checksum *
                                                   (uint32_t)k_ra8_npu_blob_word_bytes);
  if (digest != stored) {
    ra8_log_error(s_tag, "load: checksum mismatch");
    return k_ra8_err_checksum_mismatch;
  }
  return k_ra8_ok;
}

/**
 * @brief Resolve one region descriptor to its `BASEPn` base address.
 *
 * @details A BAKED region resolves to its bytes inside the blob; a RUNTIME region
 *          is carved from @p arena at the running, 16-byte-aligned offset
 *          @p arena_used, which is advanced by the region size. Every span is
 *          bounds-checked against the blob or the arena with single conditions.
 *
 * @param[in]     p          `.npub` byte buffer base.
 * @param[in]     total      Declared whole-blob length (bounds for baked data).
 * @param[in]     desc_off   Byte offset of this region descriptor.
 * @param[in]     arena      Runtime arena for RUNTIME regions.
 * @param[in,out] arena_used Bytes already claimed from @p arena; advanced here.
 * @param[out]    out_base   Resolved 64-bit AXI base for this region.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Region resolved; @p out_base and @p arena_used updated.
 * @retval k_ra8_err_out_of_range A baked region's bytes fall outside the blob.
 * @retval k_ra8_err_no_mem A runtime region does not fit in @p arena.
 *
 * @pre @p desc_off + one descriptor lies within the blob (caller-checked).
 * @pre Every out-pointer is non-NULL (caller-guaranteed).
 * @post On success @p out_base holds the region base and @p arena_used grows.
 * @post On failure @p arena_used and @p out_base are not relied upon.
 *
 * @note Re-entrant; reads the blob and the caller's running offset only.
 * @since 0.1.0
 */
static ra8_err_t internal_npu_place_region(const uint8_t*         p,
                                           uint32_t               total,
                                           uint32_t               desc_off,
                                           const ra8_npu_arena_t* arena,
                                           uint32_t*              arena_used,
                                           uint64_t*              out_base)
{
  const uint32_t flags = ra8_npu_blob_read_word(
    p,
    desc_off + ((uint32_t)k_ra8_npu_blob_rdesc_flags * (uint32_t)k_ra8_npu_blob_word_bytes));
  const uint32_t size = ra8_npu_blob_read_word(
    p,
    desc_off + ((uint32_t)k_ra8_npu_blob_rdesc_size * (uint32_t)k_ra8_npu_blob_word_bytes));
  if ((flags & (uint32_t)k_ra8_npu_blob_rflag_baked) != 0U) {
    const uint32_t data_off =
      ra8_npu_blob_read_word(p,
                             desc_off + ((uint32_t)k_ra8_npu_blob_rdesc_data_offset *
                                         (uint32_t)k_ra8_npu_blob_word_bytes));
    if (!internal_npu_span_ok(data_off, size, total)) {
      ra8_log_error(s_tag, "load: baked region outside blob");
      return k_ra8_err_out_of_range;
    }
    *out_base = (uint64_t)(uintptr_t)(p + data_off);
    return k_ra8_ok;
  }
  const uint32_t aligned =
    (*arena_used + (uint32_t)k_ra8_npu_loader_align_mask) & ~(uint32_t)k_ra8_npu_loader_align_mask;
  if (aligned > arena->bytes) {
    ra8_log_error(s_tag, "load: runtime arena exhausted");
    return k_ra8_err_no_mem;
  }
  if (size > (arena->bytes - aligned)) {
    ra8_log_error(s_tag, "load: runtime region does not fit");
    return k_ra8_err_no_mem;
  }
  *out_base   = (uint64_t)(uintptr_t)(arena->base + aligned);
  *arena_used = aligned + size;
  return k_ra8_ok;
}

/**
 * @brief Resolve every region descriptor into @p job->region_base[].
 *
 * @details Walks the @p rcount region descriptors that follow the header and
 *          resolves each through internal_npu_place_region(), advancing a
 *          private running arena offset. Extracted from ra8_npu_load() so the
 *          public entry stays within the NASA Rule 4 function-size budget.
 *
 * @param[in]  p      `.npub` byte buffer base.
 * @param[in]  total  Declared whole-blob length (bounds for baked data).
 * @param[in]  rcount Region descriptor count (already <= k_ra8_npu_region_count).
 * @param[in]  arena  Runtime arena for RUNTIME regions.
 * @param[out] job    Job whose `region_base[0..rcount)` are populated.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Every region resolved; @p job->region_base populated.
 * @retval k_ra8_err_out_of_range A baked region's bytes fall outside the blob.
 * @retval k_ra8_err_no_mem A runtime region does not fit in @p arena.
 *
 * @pre The region table lies within the blob (caller-checked).
 * @pre @p job and @p arena are non-NULL (caller-guaranteed).
 * @post On success @p job->region_base[0..rcount) hold resolved bases.
 * @post On failure @p job is not relied upon.
 *
 * @note Re-entrant; the loop is bounded by @p rcount (NASA Rule 2).
 * @since 0.1.0
 */
static ra8_err_t internal_npu_place_all_regions(const uint8_t*         p,
                                                uint32_t               total,
                                                uint32_t               rcount,
                                                const ra8_npu_arena_t* arena,
                                                ra8_npu_job_t*         job)
{
  uint32_t used = 0U;
  for (uint32_t r = 0U; r < rcount; r++) {
    const uint32_t desc_off =
      (uint32_t)k_ra8_npu_blob_header_bytes + (r * (uint32_t)k_ra8_npu_blob_region_desc_bytes);
    const ra8_err_t rgn =
      internal_npu_place_region(p, total, desc_off, arena, &used, &job->region_base[r]);
    RA8_RETURN_ON_ERROR(rgn, s_tag, "load: region");
  }
  return k_ra8_ok;
}

/**
 * @brief Parse, verify, and resolve a `.npub` blob into an ::ra8_npu_job_t.
 *
 * @details Runs the validated pipeline after the public entry has null-checked
 *          its pointers: header validation, region-table bounds, whole-payload
 *          checksum, then per-region base resolution and job finalisation.
 *          Split out of ra8_npu_load() so the public entry stays within the
 *          NASA Rule 4 function-size budget.
 *
 * @param[in]  p          `.npub` byte buffer base (non-NULL, caller-checked).
 * @param[in]  blob_bytes Buffer length in bytes.
 * @param[in]  arena      Runtime arena for RUNTIME regions (non-NULL).
 * @param[out] out_job    Populated job on success (non-NULL).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Blob valid; @p out_job populated.
 * @retval k_ra8_err_invalid_size Buffer below the header, or empty command stream.
 * @retval k_ra8_err_invalid_arg Bad magic, version, or region count.
 * @retval k_ra8_err_out_of_range A declared span falls outside the buffer.
 * @retval k_ra8_err_checksum_mismatch The payload digest differs (blob corrupt).
 * @retval k_ra8_err_no_mem A runtime region does not fit in @p arena.
 *
 * @pre @p p, @p arena and @p out_job are non-NULL (caller-guaranteed).
 * @pre @p p addresses at least @p blob_bytes readable bytes.
 * @post On success @p out_job locates the command stream and region bases.
 * @post On failure @p out_job is not modified past a partial write and is not
 *       relied upon.
 *
 * @note Re-entrant; reads the blob only.
 * @since 0.1.0
 */
static ra8_err_t internal_npu_build_job(const uint8_t*         p,
                                        uint32_t               blob_bytes,
                                        const ra8_npu_arena_t* arena,
                                        ra8_npu_job_t*         out_job)
{
  uint32_t total  = 0U;
  uint32_t rcount = 0U;
  uint32_t coff   = 0U;
  uint32_t cbytes = 0U;
  /* Each callee logs its own specific failure, so a plain early return here
   * propagates the code without a redundant second log line. */
  const ra8_err_t hdr = internal_npu_check_header(p, blob_bytes, &total, &rcount, &coff, &cbytes);
  if (hdr != k_ra8_ok) {
    return hdr;
  }
  if (!internal_npu_span_ok((uint32_t)k_ra8_npu_blob_header_bytes,
                            rcount * (uint32_t)k_ra8_npu_blob_region_desc_bytes,
                            total)) {
    ra8_log_error(s_tag, "load: region table outside blob");
    return k_ra8_err_out_of_range;
  }
  const ra8_err_t sum = internal_npu_verify_checksum(p, total);
  if (sum != k_ra8_ok) {
    return sum;
  }
  ra8_npu_job_t   job = {};
  const ra8_err_t rgn = internal_npu_place_all_regions(p, total, rcount, arena, &job);
  if (rgn != k_ra8_ok) {
    return rgn;
  }
  job.cmd_stream       = p + coff;
  job.cmd_stream_bytes = cbytes;
  job.region_count     = (uint8_t)rcount;
  *out_job             = job;
  return k_ra8_ok;
}

ra8_err_t ra8_npu_load(const void*            blob,
                       uint32_t               blob_bytes,
                       const ra8_npu_arena_t* arena,
                       ra8_npu_job_t*         out_job)
{
  RA8_CHECK_NULL_PTR(blob, s_tag, "blob must not be nullptr");
  RA8_CHECK_NULL_PTR(arena, s_tag, "arena must not be nullptr");
  RA8_CHECK_NULL_PTR(out_job, s_tag, "out_job must not be nullptr");
  return internal_npu_build_job((const uint8_t*)blob, blob_bytes, arena, out_job);
}

#else
/* RA8D2 (no NPU): this translation unit is intentionally empty. */
typedef int ra8_npu_loader_not_on_this_device_t;
#endif /* RA8_HAS_NPU */
