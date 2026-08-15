/**
 * @file ra8_decomp_limits.c
 * @brief Unified decompression-limits policy -- budget charging and checks.
 *
 * @details
 * Implementation of the one enforcement seam every archive / stream decoder
 * shares (see `ra8_decomp_limits.h`). Charging arithmetic is saturating and
 * overflow-checked so hostile 64-bit header values cannot wrap a bound: the
 * ratio limit `in * max_ratio + grace` is computed with an explicit
 * divide-guard and saturates to UINT64_MAX, at which point only the absolute
 * output cap governs (which is exactly the intent: a huge honest input is
 * bounded by the cap, not the quotient).
 *
 * Every decision in this file is a single condition on purpose: the policy
 * is the most security-load-bearing code in the content path, so each bound
 * is tested independently (and carries no compound-decision MC/DC burden).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#include "ra8_decomp_limits.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"

/** @brief Log tag for decompression-policy diagnostics. */
static const char* const s_tag_decomp = "ra8_decomp";

/**
 * @enum priv_zip_preflight_t
 * @brief Fixed classic-ZIP EOCD geometry used by the entry-cap preflight.
 */
typedef enum : uint32_t {
  k_priv_zip_eocd_bytes     = 22U,    /**< Fixed EOCD bytes before its comment.  */
  k_priv_zip_comment_max    = 65535U, /**< Maximum classic ZIP comment bytes.    */
  k_priv_zip_scan_chunk     = 512U,   /**< Candidate offsets inspected per read. */
  k_priv_zip_sig_bytes      = 4U,     /**< Bytes in the EOCD signature.          */
  k_priv_zip_entries_offset = 10U,    /**< Total-entry field offset in EOCD.     */
  k_priv_zip_comment_offset = 20U,    /**< Comment-length field offset in EOCD.  */
} priv_zip_preflight_t;

/** @brief Classic ZIP end-of-central-directory signature bytes. */
static const uint8_t s_zip_eocd_signature[k_priv_zip_sig_bytes] = {0x50U, 0x4BU, 0x05U, 0x06U};

ra8_decomp_limits_t ra8_decomp_limits_default(void)
{
  ra8_decomp_limits_t lim = {};
  lim.max_output_bytes    = (uint64_t)k_ra8_decomp_def_output_bytes;
  lim.max_ratio           = (uint32_t)k_ra8_decomp_def_max_ratio;
  lim.ratio_grace_bytes   = (uint32_t)k_ra8_decomp_def_ratio_grace;
  lim.max_entries         = (uint32_t)k_ra8_decomp_def_max_entries;
  lim.max_iterations      = (uint32_t)k_ra8_decomp_def_max_iters;
  lim.max_depth           = (uint8_t)k_ra8_decomp_def_max_depth;
  return lim;
}

/**
 * @brief Whether every field of a policy is non-zero (usable as a bound).
 * @details A zero cap is always a configuration bug: it would reject all
 *          input (caps) or create ambiguity (grace), so binding such a
 *          policy is refused outright.
 * @param[in] lim Policy to inspect (non-NULL).
 * @return Whether all six fields are non-zero.
 * @retval true  The policy is usable.
 * @retval false At least one field is zero.
 * @pre @p lim is non-NULL (caller-guarded).
 * @pre @p lim is fully initialised (no indeterminate fields).
 * @post No state is modified (pure read).
 * @post The result depends only on @p lim.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_limits_usable(const ra8_decomp_limits_t* lim)
{
  if (lim->max_output_bytes == 0U) {
    return false;
  }
  if (lim->max_ratio == 0U) {
    return false;
  }
  if (lim->ratio_grace_bytes == 0U) {
    return false;
  }
  if (lim->max_entries == 0U) {
    return false;
  }
  if (lim->max_iterations == 0U) {
    return false;
  }
  if (lim->max_depth == 0U) {
    return false;
  }
  return true;
}

/**
 * @brief The saturating ratio bound `in * max_ratio + grace` for a policy.
 * @details Guards both the multiply and the add against 64-bit wrap: a
 *          product or sum that would overflow saturates to UINT64_MAX,
 *          deliberately disabling the ratio test for astronomically large
 *          honest inputs (the absolute output cap still governs those).
 * @param[in] lim      Policy supplying `max_ratio` / `ratio_grace_bytes`.
 * @param[in] in_total Compressed bytes consumed (or declared).
 * @return The largest output byte count the ratio bound admits.
 * @retval UINT64_MAX When the bound saturates (ratio test disabled).
 * @pre @p lim is non-NULL with non-zero `max_ratio` (caller-guarded).
 * @pre @p in_total is the untrusted input size; any value is safe.
 * @post No state is modified (pure computation).
 * @post The result never wraps below its true mathematical value.
 * @note Thread-safe: pure computation.
 * @since Version 0.1.0
 */
RA8_INTERNAL static uint64_t internal_ratio_bound(const ra8_decomp_limits_t* lim, uint64_t in_total)
{
  const uint64_t ratio = (uint64_t)lim->max_ratio;
  if (in_total > (UINT64_MAX / ratio)) {
    return UINT64_MAX; /* product would wrap: saturate (output cap governs) */
  }
  const uint64_t product = in_total * ratio;
  const uint64_t grace   = (uint64_t)lim->ratio_grace_bytes;
  if (product > (UINT64_MAX - grace)) {
    return UINT64_MAX; /* sum would wrap: saturate (output cap governs) */
  }
  return product + grace;
}

ra8_err_t ra8_decomp_budget_init(ra8_decomp_budget_t* b, const ra8_decomp_limits_t* limits)
{
  RA8_CHECK_NULL_PTR(b, s_tag_decomp, "budget init: null budget");
  *b = (ra8_decomp_budget_t){};
  if (limits == nullptr) {
    b->limits = ra8_decomp_limits_default();
    return k_ra8_ok;
  }
  if (!internal_limits_usable(limits)) {
    return k_ra8_err_invalid_arg;
  }
  b->limits = *limits;
  return k_ra8_ok;
}

ra8_err_t
ra8_decomp_budget_charge_output(ra8_decomp_budget_t* b, uint64_t in_total, uint64_t out_delta)
{
  RA8_CHECK_NULL_PTR(b, s_tag_decomp, "charge output: null budget");
  if (out_delta > (UINT64_MAX - b->out_bytes)) {
    b->out_bytes = UINT64_MAX; /* saturate: any wrap-sized delta is a breach */
  } else {
    b->out_bytes += out_delta;
  }
  b->in_bytes = in_total;
  if (b->out_bytes > b->limits.max_output_bytes) {
    return k_ra8_err_decomp_output_cap;
  }
  if (b->out_bytes > internal_ratio_bound(&b->limits, in_total)) {
    return k_ra8_err_decomp_ratio;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_decomp_budget_charge_entry(ra8_decomp_budget_t* b)
{
  RA8_CHECK_NULL_PTR(b, s_tag_decomp, "charge entry: null budget");
  if (b->entries >= b->limits.max_entries) {
    return k_ra8_err_decomp_entries;
  }
  b->entries += 1U;
  return k_ra8_ok;
}

ra8_err_t ra8_decomp_budget_charge_iter(ra8_decomp_budget_t* b)
{
  RA8_CHECK_NULL_PTR(b, s_tag_decomp, "charge iter: null budget");
  if (b->iters >= b->limits.max_iterations) {
    return k_ra8_err_decomp_iterations;
  }
  b->iters += 1U;
  return k_ra8_ok;
}

ra8_err_t ra8_decomp_budget_enter(ra8_decomp_budget_t* b)
{
  RA8_CHECK_NULL_PTR(b, s_tag_decomp, "enter: null budget");
  if (b->depth >= b->limits.max_depth) {
    return k_ra8_err_decomp_depth;
  }
  b->depth = (uint8_t)(b->depth + 1U);
  return k_ra8_ok;
}

void ra8_decomp_budget_leave(ra8_decomp_budget_t* b)
{
  if (b == nullptr) {
    return; /* teardown paths call unconditionally */
  }
  if (b->depth == 0U) {
    return; /* unbalanced leave: ignore rather than wrap */
  }
  b->depth = (uint8_t)(b->depth - 1U);
}

ra8_err_t
ra8_decomp_check_declared(const ra8_decomp_limits_t* limits, uint64_t comp_size, uint64_t out_size)
{
  RA8_CHECK_NULL_PTR(limits, s_tag_decomp, "check declared: null limits");
  if (out_size > limits->max_output_bytes) {
    return k_ra8_err_decomp_output_cap;
  }
  if (out_size > internal_ratio_bound(limits, comp_size)) {
    return k_ra8_err_decomp_ratio;
  }
  return k_ra8_ok;
}

/**
 * @brief Decode one little-endian 16-bit ZIP field.
 * @details Combines two already-bounds-checked bytes without an unaligned load.
 * @param[in] bytes Address of two readable bytes.
 * @return Decoded unsigned value.
 * @retval 0 The encoded field is zero.
 * @pre @p bytes is non-NULL.
 * @pre At least two bytes are readable at @p bytes.
 * @post No source byte is modified.
 * @post The result depends only on the two source bytes.
 * @note ZIP metadata is always little-endian.
 * @since Version 0.1.0
 */
RA8_INTERNAL static uint16_t internal_zip_u16(const uint8_t* bytes)
{
  return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

ra8_err_t ra8_decomp_zip_entry_preflight(ra8_decomp_read_fn read, void* ctx, uint64_t archive_size)
{
  RA8_CHECK_NULL_PTR(read, s_tag_decomp, "zip preflight: null reader");
  if (archive_size < (uint64_t)k_priv_zip_eocd_bytes) {
    return k_ra8_ok;
  }
  const uint64_t candidates = archive_size - (uint64_t)k_priv_zip_eocd_bytes + 1U;
  const uint64_t window     = (uint64_t)k_priv_zip_comment_max + 1U;
  const uint64_t lower      = (candidates > window) ? (candidates - window) : 0U;
  uint64_t       end        = candidates;
  uint8_t        chunk[k_priv_zip_scan_chunk + k_priv_zip_sig_bytes - 1U];
  uint8_t        eocd[k_priv_zip_eocd_bytes];
  while (end > lower) {
    const uint64_t start = ((end - lower) > (uint64_t)k_priv_zip_scan_chunk)
                             ? (end - (uint64_t)k_priv_zip_scan_chunk)
                             : lower;
    const size_t   count = (size_t)(end - start);
    if (read(ctx, start, chunk, count + k_priv_zip_sig_bytes - 1U) !=
        count + k_priv_zip_sig_bytes - 1U) {
      return k_ra8_ok;
    }
    for (size_t i = count; i > 0U; --i) {
      const size_t at = i - 1U;
      if (memcmp(&chunk[at], s_zip_eocd_signature, k_priv_zip_sig_bytes) != 0) {
        continue;
      }
      const uint64_t position = start + (uint64_t)at;
      if (read(ctx, position, eocd, sizeof(eocd)) != sizeof(eocd)) {
        continue;
      }
      const uint16_t comment = internal_zip_u16(&eocd[k_priv_zip_comment_offset]);
      if ((position + sizeof(eocd) + (uint64_t)comment) != archive_size) {
        continue;
      }
      const uint16_t entries = internal_zip_u16(&eocd[k_priv_zip_entries_offset]);
      return ((uint32_t)entries > (uint32_t)k_ra8_decomp_def_max_entries) ? k_ra8_err_decomp_entries
                                                                          : k_ra8_ok;
    }
    end = start;
  }
  return k_ra8_ok;
}
