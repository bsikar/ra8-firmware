/**
 * @file ra8_jof_audit.c
 * @brief No-heap implementation of the backing-agnostic JOF audit.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * Parses JOF geometry through an injected positioned-read backend, derives
 * exact caller-buffer requirements, and audits every indexed tile for stored
 * coverage and decoded-size consistency. Reused caller scratch supports raw
 * and compressed payloads without allocation or a filesystem dependency.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_jof_audit.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief FNV-1a constants used for decoded tile evidence. */
typedef enum : uint32_t {
  k_jof_audit_fnv_basis    = 2166136261U, /**< FNV-1a 32-bit offset basis. */
  k_jof_audit_fnv_prime    = 16777619U,   /**< FNV-1a 32-bit prime.        */
  k_jof_audit_u32_b3_shift = 24U,         /**< LE32 high-byte shift.       */
} jof_audit_hash_t;

/** @brief Half-open address span used for caller-buffer separation checks. */
typedef struct internal_span_t {
  uintptr_t begin; /**< First byte address.         */
  uintptr_t end;   /**< One-past-last byte address. */
} internal_span_t;

/**
 * @brief Read exactly one bounded window from the injected backing
 * @details Converts a short successful callback read into a validation error.
 * @param[in]  pread   Read callback.
 * @param[in]  ctx     Callback context.
 * @param[in]  offset  Absolute offset.
 * @param[out] buf     Destination.
 * @param[in]  len     Exact byte count.
 * @return Exact-read status.
 * @retval k_ra8_ok Exactly @p len bytes were read.
 * @retval k_ra8_err_validation_failed The callback returned a short read.
 * @pre @p pread and @p buf are non-null.
 * @pre @p buf is writable for @p len bytes.
 * @post Success initializes exactly @p len bytes in @p buf.
 * @post Failure never claims a complete read.
 * @note Thread safety inherits the injected reader.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_read_exact(ra8_jof_pread_fn pread, void* ctx, uint64_t offset, uint8_t* buf, size_t len)
{
  size_t          got = 0U;
  const ra8_err_t rc  = pread(ctx, offset, buf, len, &got);
  if (rc != k_ra8_ok) {
    return rc;
  }
  return (got == len) ? k_ra8_ok : k_ra8_err_validation_failed;
}

/**
 * @brief Decode a little-endian u32 from an index entry
 * @details Combines four bytes explicitly so host endianness is irrelevant.
 * @param[in] p Four readable bytes.
 * @return Decoded value.
 * @retval 0 All four encoded bytes were zero.
 * @pre @p p addresses at least four readable bytes.
 * @pre The bytes use the canonical JOF little-endian order.
 * @post The source bytes are unchanged.
 * @post The return value is independent of native integer representation.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_rd_u32(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
         ((uint32_t)p[3] << k_jof_audit_u32_b3_shift);
}

/**
 * @brief Hash decoded bytes and report whether all bytes are equal
 * @details Produces bounded FNV-1a diagnostic evidence, not an identity proof.
 * @param[in]  bytes       Decoded tile.
 * @param[in]  len         Tile byte count.
 * @param[out] out_uniform Receives uniformity evidence.
 * @return FNV-1a hash.
 * @retval k_jof_audit_fnv_basis Hash returned for an empty span.
 * @pre @p bytes is readable for @p len bytes.
 * @pre @p out_uniform is writable.
 * @post @p out_uniform is true exactly when every byte equals the first.
 * @post The input bytes are unchanged.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_hash(const uint8_t* bytes, size_t len, bool* out_uniform)
{
  uint32_t h       = (uint32_t)k_jof_audit_fnv_basis;
  bool     uniform = true;
  for (size_t i = 0U; i < len; ++i) {
    if (i != 0U) {
      if (bytes[i] != bytes[0]) {
        uniform = false;
      }
    }
    h ^= (uint32_t)bytes[i];
    h *= (uint32_t)k_jof_audit_fnv_prime;
  }
  *out_uniform = uniform;
  return h;
}

/**
 * @brief Convert one caller region into an overflow-checked address span
 * @details Empty spans are represented by zero endpoints and never overlap.
 * @param[in]  ptr Region start, or null only when @p len is zero.
 * @param[in]  len Region byte count.
 * @param[out] out Receives the half-open address span.
 * @return Span-construction status.
 * @retval k_ra8_ok The span is representable.
 * @retval k_ra8_err_null_ptr A non-empty span has no storage.
 * @retval k_ra8_err_invalid_size The end address would wrap.
 * @pre @p out is non-null and writable.
 * @pre A non-empty @p ptr denotes at least @p len bytes of caller storage.
 * @post Success initializes @p out without touching caller storage.
 * @post Failure does not dereference @p ptr.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_make_span(const void* ptr, size_t len, internal_span_t* out)
{
  if (len == 0U) {
    *out = (internal_span_t){};
    return k_ra8_ok;
  }
  if (ptr == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const uintptr_t begin = (uintptr_t)ptr;
  if (len > (size_t)(UINTPTR_MAX - begin)) {
    return k_ra8_err_invalid_size;
  }
  *out = (internal_span_t){.begin = begin, .end = begin + (uintptr_t)len};
  return k_ra8_ok;
}

/**
 * @brief Report whether two non-wrapping half-open spans overlap
 * @details Uses strict half-open comparisons so adjacent and empty spans are
 * treated as disjoint while any shared byte is rejected.
 * @param[in] left  First checked span.
 * @param[in] right Second checked span.
 * @return True only when the spans share at least one byte.
 * @retval true The spans overlap.
 * @retval false The spans are disjoint or at least one is empty.
 * @pre Both spans came from ::internal_make_span.
 * @pre @p left and @p right are non-null readable objects.
 * @post Neither span is modified.
 * @post The result depends only on the two endpoint pairs.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_spans_overlap(const internal_span_t* left,
                                                const internal_span_t* right)
{
  if (left->begin >= right->end) {
    return false;
  }
  return right->begin < left->end;
}

/**
 * @brief Check every pair of writable audit spans for overlap.
 * @details Evaluates the ten unique pairs in a fixed order and stops at the
 *          first shared byte, including the result and workspace descriptors.
 * @param[in] workspace Workspace descriptor span.
 * @param[in] records Record-array span.
 * @param[in] tile Decoded-tile span.
 * @param[in] scratch Codec-scratch span, possibly empty.
 * @param[in] result Public result span.
 * @return Whether any supplied pair overlaps.
 * @retval true At least one pair shares a byte.
 * @retval false Every pair is disjoint.
 * @pre Every span came from ::internal_make_span.
 * @pre All span objects are non-NULL and readable.
 * @post No span or caller storage is modified.
 * @post False proves pairwise separation of all five spans.
 * @note Empty spans compare disjoint from every span.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_any_workspace_overlap(const internal_span_t* workspace,
                                                        const internal_span_t* records,
                                                        const internal_span_t* tile,
                                                        const internal_span_t* scratch,
                                                        const internal_span_t* result)
{
  if (internal_spans_overlap(workspace, records)) {
    return true;
  }
  if (internal_spans_overlap(workspace, tile)) {
    return true;
  }
  if (internal_spans_overlap(workspace, scratch)) {
    return true;
  }
  if (internal_spans_overlap(workspace, result)) {
    return true;
  }
  if (internal_spans_overlap(records, tile)) {
    return true;
  }
  if (internal_spans_overlap(records, scratch)) {
    return true;
  }
  if (internal_spans_overlap(records, result)) {
    return true;
  }
  if (internal_spans_overlap(tile, scratch)) {
    return true;
  }
  if (internal_spans_overlap(tile, result)) {
    return true;
  }
  return internal_spans_overlap(scratch, result);
}

ra8_err_t ra8_jof_audit_requirements(ra8_jof_pread_fn              pread,
                                     void*                         pread_ctx,
                                     uint64_t                      total_size,
                                     ra8_jof_audit_requirements_t* out)
{
  if (pread == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_jof_info_t  info = {};
  const ra8_err_t rc   = ra8_jof_parse(pread, pread_ctx, total_size, &info);
  if (rc != k_ra8_ok) {
    return rc;
  }
  const uint64_t tile64 = (uint64_t)info.tile_w * (uint64_t)info.tile_h * (uint64_t)info.bpp;
  if (tile64 > UINT32_MAX) {
    return k_ra8_err_invalid_size;
  }
  const uint32_t                     tile      = (uint32_t)tile64;
  const ra8_jof_audit_requirements_t candidate = {
    .record_count = info.tile_count,
    .tile_bytes   = tile,
    .scratch_bytes =
      (info.codec == (uint8_t)k_ra8_jof_codec_deflate) ? ra8_jof_stored_bound(tile) : 0U,
  };
  if (candidate.tile_bytes == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (info.codec == (uint8_t)k_ra8_jof_codec_deflate) {
    if (candidate.scratch_bytes == 0U) {
      return k_ra8_err_invalid_size;
    }
  }
  *out = candidate;
  return k_ra8_ok;
}

/**
 * @brief Validate the writable audit span layout.
 * @details Builds checked integer spans for every caller-owned destination and
 *          rejects arithmetic overflow or any pairwise overlap.
 * @param[in] ws Caller workspace.
 * @param[in] need Exact requirements.
 * @param[in] out Public result destination.
 * @param[in] record_bytes Size of the complete record array.
 * @return Span validation status.
 * @retval k_ra8_ok Every writable span is representable and disjoint.
 * @retval k_ra8_err_invalid_size A span end cannot be represented.
 * @retval k_ra8_err_invalid_arg At least two writable spans overlap.
 * @pre All required workspace pointers and capacities are already validated.
 * @pre @p record_bytes is the checked record-array byte count.
 * @post No caller buffer contents are modified.
 * @post Success proves pairwise separation of all writable destinations.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_check_workspace_spans(const ra8_jof_audit_workspace_t*    ws,
                               const ra8_jof_audit_requirements_t* need,
                               const ra8_jof_audit_result_t*       out,
                               size_t                              record_bytes)
{
  internal_span_t workspace_span = {};
  internal_span_t record_span    = {};
  internal_span_t tile_span      = {};
  internal_span_t scratch_span   = {};
  internal_span_t result_span    = {};
  ra8_err_t       rc             = internal_make_span(ws, sizeof(*ws), &workspace_span);
  if (rc == k_ra8_ok) {
    rc = internal_make_span(ws->records, record_bytes, &record_span);
  }
  if (rc == k_ra8_ok) {
    rc = internal_make_span(ws->tile, need->tile_bytes, &tile_span);
  }
  if (rc == k_ra8_ok) {
    rc = internal_make_span(ws->scratch, need->scratch_bytes, &scratch_span);
  }
  if (rc == k_ra8_ok) {
    rc = internal_make_span(out, sizeof(*out), &result_span);
  }
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (internal_any_workspace_overlap(&workspace_span,
                                     &record_span,
                                     &tile_span,
                                     &scratch_span,
                                     &result_span)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate caller workspace against parsed requirements
 * @details Checks every mandatory pointer and exact-or-larger capacity.
 * @param[in] ws   Caller workspace.
 * @param[in] need Exact requirements.
 * @param[in] out  Public result destination.
 * @return Workspace validation status.
 * @retval k_ra8_ok All required spans are present and large enough.
 * @retval k_ra8_err_null_ptr A mandatory span is absent.
 * @retval k_ra8_err_invalid_size At least one capacity is too small.
 * @retval k_ra8_err_invalid_arg Writable caller spans overlap.
 * @pre @p need points to successfully derived requirements.
 * @pre @p ws is either null or points to readable workspace metadata.
 * @post No caller buffer contents are modified.
 * @post Success authorizes exactly the capacities in @p need.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_check_workspace(const ra8_jof_audit_workspace_t*    ws,
                                                       const ra8_jof_audit_requirements_t* need,
                                                       const ra8_jof_audit_result_t*       out)
{
  if (ws == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (ws->records == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (ws->tile == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (need->scratch_bytes != 0U) {
    if (ws->scratch == nullptr) {
      return k_ra8_err_null_ptr;
    }
  }
  if (ws->record_cap < need->record_count) {
    return k_ra8_err_invalid_size;
  }
  if (ws->tile_cap < need->tile_bytes) {
    return k_ra8_err_invalid_size;
  }
  if (ws->scratch_cap < need->scratch_bytes) {
    return k_ra8_err_invalid_size;
  }
  const size_t record_bytes = (size_t)need->record_count * sizeof(*ws->records);
  if (need->record_count != 0U) {
    if ((record_bytes / sizeof(*ws->records)) != (size_t)need->record_count) {
      return k_ra8_err_invalid_size;
    }
  }
  return internal_check_workspace_spans(ws, need, out, record_bytes);
}

/**
 * @brief Count earlier records matching one non-uniform tile fingerprint
 * @details Uniform tiles are excluded so common blank regions do not inflate
 * duplicate diagnostics; matching hashes are evidence only.
 * @param[in] records Completed records.
 * @param[in] count   Earlier record count.
 * @param[in] item    Current record.
 * @return Number of matching earlier records, as diagnostic candidates only.
 * @retval 0 The item is uniform or no earlier fingerprint matches.
 * @pre @p records contains @p count completed entries.
 * @pre @p item points to one completed current entry.
 * @post The record array and item are unchanged.
 * @post The result never exceeds @p count.
 * @note A fingerprint match is not byte-equality proof and cannot invalidate
 *       an otherwise well-formed atlas.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_duplicate_count(const ra8_jof_audit_record_t* records,
                                                      uint32_t                      count,
                                                      const ra8_jof_audit_record_t* item)
{
  if (item->uniform) {
    return 0U;
  }
  uint32_t matches = 0U;
  for (uint32_t i = 0U; i < count; ++i) {
    if (records[i].uniform) {
      continue;
    }
    if (records[i].payload != item->payload) {
      continue;
    }
    if (records[i].content_hash != item->content_hash) {
      continue;
    }
    matches++;
  }
  return matches;
}

/** @brief Mutable state shared by the bounded per-tile audit operation. */
typedef struct internal_audit_context_t {
  ra8_jof_pread_fn           pread;           /**< Injected positioned reader.     */
  void*                      pread_ctx;       /**< Reader-specific context.        */
  ra8_jof_audit_workspace_t* workspace;       /**< Caller-owned reusable storage.  */
  ra8_jof_audit_result_t*    candidate;       /**< Transactional result candidate. */
  uint32_t                   expected_offset; /**< Next canonical payload offset.  */
} internal_audit_context_t;

/**
 * @brief Audit one index entry and its decoded tile evidence.
 * @details Reads the canonical record, checks stored coverage, decodes through
 * caller scratch, validates edge geometry, and accumulates diagnostic evidence.
 * @param[in,out] context Mutable bounded audit state and caller workspaces.
 * @param[in] index_number Zero-based index record to audit.
 * @return Canonical read, decode, or validation status.
 * @retval k_ra8_ok One record and its tile evidence were completed.
 * @retval k_ra8_err_validation_failed Stored offset arithmetic was invalid.
 * @retval other Injected read, tile decode, or dimension derivation failed.
 * @pre @p context is non-null and all workspace spans passed overlap checks.
 * @pre @p index_number is below the parsed tile count and record capacity.
 * @post Success initializes exactly the selected record and advances coverage.
 * @post Failure never increments the decoded-tile count for an incomplete tile.
 * @note Duplicate fingerprints are diagnostic candidates, not equality proof.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_audit_tile(internal_audit_context_t* context, uint32_t index_number)
{
  uint8_t        index[k_ra8_jof_index_entry] = {};
  const uint64_t index_at = (uint64_t)context->candidate->info.index_off +
                            ((uint64_t)index_number * (uint64_t)k_ra8_jof_index_entry);
  ra8_err_t      rc =
    internal_read_exact(context->pread, context->pread_ctx, index_at, index, sizeof(index));
  if (rc != k_ra8_ok) {
    return rc;
  }
  ra8_jof_audit_record_t* const record = &context->workspace->records[index_number];
  *record                              = (ra8_jof_audit_record_t){
    .offset = internal_rd_u32(&index[k_ra8_jof_idx_ofs_offset]),
    .length = internal_rd_u32(&index[k_ra8_jof_idx_ofs_length]),
  };
  if (record->offset != context->expected_offset) {
    context->candidate->coverage_errors++;
  }
  const uint64_t next = (uint64_t)record->offset + (uint64_t)record->length;
  if (next > UINT32_MAX) {
    return k_ra8_err_validation_failed;
  }
  context->expected_offset = (uint32_t)next;

  const uint16_t tx = (uint16_t)(index_number % (uint32_t)context->candidate->info.tile_cols);
  const uint16_t ty = (uint16_t)(index_number / (uint32_t)context->candidate->info.tile_cols);
  rc                = ra8_jof_read_tile(context->pread,
                                        context->pread_ctx,
                                        &context->candidate->info,
                                        tx,
                                        ty,
                                        context->workspace->scratch,
                                        context->workspace->scratch_cap,
                                        context->workspace->tile,
                                        context->workspace->tile_cap,
                                        &record->width,
                                        &record->height);
  if (rc != k_ra8_ok) {
    return rc;
  }
  record->payload =
    (uint32_t)record->width * (uint32_t)record->height * (uint32_t)context->candidate->info.bpp;
  uint16_t want_w = 0U;
  uint16_t want_h = 0U;
  rc              = ra8_jof_tile_dims(&context->candidate->info, tx, ty, &want_w, &want_h);
  if (rc != k_ra8_ok) {
    return rc;
  }
  bool geometry_error = record->width != want_w;
  if (record->height != want_h) {
    geometry_error = true;
  }
  if (geometry_error) {
    context->candidate->geometry_errors++;
  }
  record->content_hash = internal_hash(context->workspace->tile, record->payload, &record->uniform);
  context->candidate->duplicate_candidates +=
    internal_duplicate_count(context->workspace->records, index_number, record);
  context->candidate->decoded_tiles++;
  return k_ra8_ok;
}

ra8_err_t ra8_jof_audit(ra8_jof_pread_fn           pread,
                        void*                      pread_ctx,
                        uint64_t                   total_size,
                        ra8_jof_audit_workspace_t* workspace,
                        ra8_jof_audit_result_t*    out)
{
  if (pread == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_jof_audit_requirements_t need = {};
  ra8_err_t                    rc = ra8_jof_audit_requirements(pread, pread_ctx, total_size, &need);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_check_workspace(workspace, &need, out);
  if (rc != k_ra8_ok) {
    return rc;
  }
  ra8_jof_audit_result_t candidate = {};
  rc                               = ra8_jof_parse(pread, pread_ctx, total_size, &candidate.info);
  if (rc != k_ra8_ok) {
    return rc;
  }

  internal_audit_context_t context = {.pread           = pread,
                                      .pread_ctx       = pread_ctx,
                                      .workspace       = workspace,
                                      .candidate       = &candidate,
                                      .expected_offset = (uint32_t)k_ra8_jof_hdr_bytes};
  for (uint32_t i = 0U; i < candidate.info.tile_count; ++i) {
    rc = internal_audit_tile(&context, i);
    if (rc != k_ra8_ok) {
      return rc;
    }
  }
  if (context.expected_offset != candidate.info.index_off) {
    candidate.coverage_errors++;
  }
  *out = candidate;
  if (candidate.coverage_errors != 0U) {
    return k_ra8_err_validation_failed;
  }
  if (candidate.geometry_errors != 0U) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}
