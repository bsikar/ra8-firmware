/**
 * @file ra8_book_chunked_validate.c
 * @brief Strict streaming-validation adapter for an open RBKC chunk reader.
 *
 * @details
 * Presents an opened chunk-compressed RBKC source as exact flat reads for the
 * strict RABOOK1 validator. It rechecks reader geometry, rejects overlapping
 * caller workspaces, inflates one chunk at a time, and retains no storage
 * beyond the caller-owned reader, chunk, and validation scratch buffers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_book_chunked.h"

/** @brief Cached inflated-chunk view used by arbitrary flat-source reads. */
typedef struct {
  ra8_book_chunked_t* rd;         /**< Open compressed reader.          */
  uint8_t*            chunk;      /**< Caller-owned inflated chunk.     */
  uint32_t            loaded_idx; /**< Cached chunk index.              */
  uint32_t            loaded_len; /**< Exact cached inflated byte span. */
  bool                loaded;     /**< Whether the cache contains data. */
} chunk_validate_t;

/** @brief Fixed span count used by strict alias validation. */
typedef enum : uint8_t {
  k_chunk_validate_span_count = 5U, /**< Reader, chunk, scratch, staging, and table. */
} chunk_validate_constant_t;

/**
 * @brief Decide whether two non-empty caller-memory spans overlap.
 * @details Converts bases to integer addresses, proves each exclusive end is
 *          representable, then applies the standard half-open overlap test.
 * @param[in] a First span base.
 * @param[in] a_len First span length in bytes.
 * @param[in] b Second span base.
 * @param[in] b_len Second span length in bytes.
 * @param[out] invalid Receives true when either end address would overflow.
 * @return Whether the two representable spans overlap.
 * @retval true The spans share at least one byte.
 * @retval false The spans are disjoint or an address range is invalid.
 * @pre Both span bases are non-NULL and lengths are non-zero.
 * @pre @p invalid is non-NULL and writable.
 * @post @p invalid reports address-arithmetic overflow independently of
 * overlap.
 * @post Input storage is not read or modified.
 * @note Pure and thread-safe; comparisons use integer addresses to avoid
 *       undefined relational comparisons between unrelated C objects.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool
internal_spans_overlap(const void* a, uint64_t a_len, const void* b, uint64_t b_len, bool* invalid)
{
  const uintptr_t a_begin = (uintptr_t)a;
  const uintptr_t b_begin = (uintptr_t)b;
  *invalid                = (a_len > (uint64_t)UINTPTR_MAX) || (b_len > (uint64_t)UINTPTR_MAX) ||
                            (a_begin > (UINTPTR_MAX - (uintptr_t)a_len)) ||
                            (b_begin > (UINTPTR_MAX - (uintptr_t)b_len));
  if (*invalid) {
    return false;
  }
  const uintptr_t a_end = a_begin + (uintptr_t)a_len;
  const uintptr_t b_end = b_begin + (uintptr_t)b_len;
  return (a_begin < b_end) && (b_begin < a_end);
}

/**
 * @brief Revalidate the open-reader invariants needed by strict validation.
 * @details Recomputes chunk geometry, table bounds, monotonic compressed
 *          extents, and payload addition before any cache read is attempted.
 * @param[in] rd Candidate open reader.
 * @return Reader-state validation status.
 * @retval k_ra8_ok Geometry, callbacks, table, and staging are usable.
 * @retval k_ra8_err_invalid_state The reader is partial or internally
 * inconsistent.
 * @pre @p rd is non-NULL.
 * @pre The table remains readable for `chunk_count + 1` entries.
 * @post No reader or caller storage is modified.
 * @post Success proves every later table index and staging write is bounded.
 * @note Pure and thread-safe for an immutable reader.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_reader(const ra8_book_chunked_t* rd)
{
  if ((rd->file_read == nullptr) || (rd->inflate == nullptr) || (rd->table == nullptr) ||
      (rd->staging == nullptr) || (rd->chunk_bytes == 0U) || (rd->chunk_count == 0U) ||
      (rd->inflated_total == 0U) || (rd->staging_cap == 0U)) {
    return k_ra8_err_invalid_state;
  }
  const uint64_t expected_count =
    (rd->inflated_total / (uint64_t)rd->chunk_bytes) +
    ((rd->inflated_total % (uint64_t)rd->chunk_bytes) != 0U ? 1U : 0U);
  const uint64_t table_entries = (uint64_t)rd->chunk_count + 1U;
  if ((expected_count > (uint64_t)UINT32_MAX) || ((uint64_t)rd->chunk_count != expected_count) ||
      (table_entries > (uint64_t)rd->table_cap_entries)) {
    return k_ra8_err_invalid_state;
  }
  if (rd->table[0] != 0U) {
    return k_ra8_err_invalid_state;
  }
  for (uint32_t i = 0U; i < rd->chunk_count; ++i) {
    if ((rd->table[i + 1U] <= rd->table[i]) ||
        ((rd->table[i + 1U] - rd->table[i]) > (uint64_t)rd->staging_cap)) {
      return k_ra8_err_invalid_state;
    }
  }
  if (rd->table[rd->chunk_count] > (UINT64_MAX - rd->payload_off)) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Reject output aliases before the output is zeroed or published.
 * @details Compares the result object with the reader, inflated chunk, strict
 *          scratch, compressed staging, and offset-table spans fail-closed.
 * @param[in] rd Candidate reader descriptor.
 * @param[in] chunk Inflated-chunk workspace, possibly NULL.
 * @param[in] chunk_cap Advertised @p chunk capacity.
 * @param[in] scratch Strict-validation workspace, possibly NULL.
 * @param[in] scratch_cap Advertised @p scratch capacity.
 * @param[out] out_header Candidate result storage.
 * @return Whether @p out_header overlaps a known reader or workspace span.
 * @retval true An overlap or unrepresentable span was found.
 * @retval false The result object is disjoint from every sized known span.
 * @pre @p rd and @p out_header are non-NULL and readable as their declared
 * types.
 * @pre Advertised capacities truthfully describe every non-NULL workspace.
 * @post No caller storage is read through an aliased pointer or modified.
 * @post A false result permits output clearing without corrupting known input.
 * @note The opaque file callback context cannot be sized here and remains a
 *       documented caller no-alias precondition.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool internal_output_is_aliased(const ra8_book_chunked_t* rd,
                                       const uint8_t*            chunk,
                                       uint32_t                  chunk_cap,
                                       const uint8_t*            scratch,
                                       uint32_t                  scratch_cap,
                                       const ra8_book_header_t*  out_header)
{
  const uint64_t table_len = (uint64_t)rd->table_cap_entries * sizeof(rd->table[0]);
  const void*    bases[k_chunk_validate_span_count] = {rd, chunk, scratch, rd->staging, rd->table};
  const uint64_t lens[k_chunk_validate_span_count]  = {sizeof(*rd),
                                                       chunk_cap,
                                                       scratch_cap,
                                                       rd->staging_cap,
                                                       table_len};
  for (uint8_t i = 0U; i < k_chunk_validate_span_count; ++i) {
    if ((bases[i] != nullptr) && (lens[i] != 0U)) {
      bool invalid = false;
      if (internal_spans_overlap(out_header, sizeof(*out_header), bases[i], lens[i], &invalid) ||
          invalid) {
        return true;
      }
    }
  }
  return false;
}

/**
 * @brief Require strict-validation workspaces to be pairwise disjoint.
 * @details Checks every pair among the reader, inflated chunk, semantic
 *          scratch, compressed staging, and exact validated table spans.
 * @param[in] rd Validated open reader.
 * @param[out] chunk Inflated-chunk workspace.
 * @param[in] chunk_len Bytes of @p chunk the adapter will access.
 * @param[out] scratch Semantic and CRC workspace.
 * @param[in] scratch_cap Writable bytes at @p scratch.
 * @return Workspace validation status.
 * @retval k_ra8_ok All five mutable/read-only workspace spans are disjoint.
 * @retval k_ra8_err_invalid_arg Spans overlap or address arithmetic overflows.
 * @pre All bases are non-NULL and lengths are non-zero.
 * @pre Reader geometry and table bounds passed ::internal_validate_reader.
 * @post No workspace bytes are modified.
 * @post Success prevents cache corruption through caller-buffer aliasing.
 * @note Pure and thread-safe for immutable span descriptors.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_workspaces(const ra8_book_chunked_t* rd,
                                              uint8_t*                  chunk,
                                              uint32_t                  chunk_len,
                                              uint8_t*                  scratch,
                                              uint32_t                  scratch_cap)
{
  const uint64_t table_len = ((uint64_t)rd->chunk_count + 1U) * sizeof(rd->table[0]);
  const void*    bases[k_chunk_validate_span_count] = {rd, chunk, scratch, rd->staging, rd->table};
  const uint64_t lens[k_chunk_validate_span_count]  = {sizeof(*rd),
                                                       chunk_len,
                                                       scratch_cap,
                                                       rd->staging_cap,
                                                       table_len};
  for (uint8_t i = 0U; i < k_chunk_validate_span_count; ++i) {
    for (uint8_t j = (uint8_t)(i + 1U); j < k_chunk_validate_span_count; ++j) {
      bool invalid = false;
      if (internal_spans_overlap(bases[i], lens[i], bases[j], lens[j], &invalid) || invalid) {
        return k_ra8_err_invalid_arg;
      }
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Inflate one requested chunk into the caller cache when not resident.
 * @details Reuses an already resident matching chunk; otherwise derives the
 *          exact final-chunk length and delegates decompression to the reader.
 * @param[in,out] ctx Adapter state.
 * @param[in] idx Chunk index to make resident.
 * @return Chunk-reader status.
 * @retval k_ra8_ok The requested chunk is resident in the caller cache.
 * @retval k_ra8_err_out_of_range The reader rejected the derived chunk range.
 * @pre @p idx is less than the open reader's chunk count.
 * @pre ctx->chunk has capacity for the configured inflated chunk size.
 * @post Success makes @p idx and its exact span resident in ctx->chunk.
 * @post Failure does not mark the requested chunk as newly resident.
 * @note Not thread-safe; reuses both reader staging and caller chunk storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_load_chunk(chunk_validate_t* ctx, uint32_t idx)
{
  if (ctx->loaded && (ctx->loaded_idx == idx)) {
    return k_ra8_ok;
  }
  const uint64_t offset = (uint64_t)idx * (uint64_t)ctx->rd->chunk_bytes;
  uint64_t       remain = ctx->rd->inflated_total - offset;
  uint32_t       span   = ctx->rd->chunk_bytes;
  if (remain < (uint64_t)span) {
    span = (uint32_t)remain;
  }
  const ra8_err_t err = ra8_book_chunked_read(ctx->rd, offset, ctx->chunk, span);
  if (err == k_ra8_ok) {
    ctx->loaded     = true;
    ctx->loaded_idx = idx;
    ctx->loaded_len = span;
  }
  return err;
}

/**
 * @brief Serve an exact arbitrary flat-blob read through the one-chunk cache.
 * @details Splits a flat range at chunk boundaries, inflates each chunk at
 *          most once while resident, and copies until the exact request is met.
 * @param[in,out] opaque Adapter context.
 * @param[in] offset Flat-blob byte offset.
 * @param[out] dst Destination for exactly @p len bytes.
 * @param[in] len Exact byte count.
 * @return Adapter, chunk-reader, or range status.
 * @retval k_ra8_ok Exactly @p len bytes were copied.
 * @retval k_ra8_err_out_of_range The requested flat range exceeds the source.
 * @pre Public validation established every pointer and workspace capacity.
 * @pre @p opaque points to an initialized chunk-validation context.
 * @post Success fills all @p len destination bytes.
 * @post No bytes beyond the requested destination range are modified.
 * @note Reads spanning chunk boundaries are copied iteratively without
 * recursion.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_chunk_flat_read(void* opaque, uint64_t offset, uint8_t* dst, uint32_t len)
{
  chunk_validate_t* ctx = (chunk_validate_t*)opaque;
  if ((offset > ctx->rd->inflated_total) || ((uint64_t)len > (ctx->rd->inflated_total - offset))) {
    return k_ra8_err_out_of_range;
  }
  uint32_t copied = 0U;
  while (copied < len) {
    const uint32_t idx = (uint32_t)(offset / (uint64_t)ctx->rd->chunk_bytes);
    ra8_err_t      err = internal_load_chunk(ctx, idx);
    if (err != k_ra8_ok) {
      return err;
    }
    const uint32_t in_chunk = (uint32_t)(offset % (uint64_t)ctx->rd->chunk_bytes);
    uint32_t       span     = ctx->loaded_len - in_chunk;
    if (span > (len - copied)) {
      span = len - copied;
    }
    (void)memmove(&dst[copied], &ctx->chunk[in_chunk], span);
    copied += span;
    offset += span;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_book_chunked_validate_strict(ra8_book_chunked_t* rd,
                                           uint8_t*            chunk,
                                           uint32_t            chunk_cap,
                                           uint8_t*            scratch,
                                           uint32_t            scratch_cap,
                                           ra8_book_header_t*  out_header)
{
  if (out_header == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (rd == nullptr) {
    *out_header = (ra8_book_header_t){};
    return k_ra8_err_null_ptr;
  }
  if (internal_output_is_aliased(rd, chunk, chunk_cap, scratch, scratch_cap, out_header)) {
    return k_ra8_err_invalid_arg;
  }
  *out_header = (ra8_book_header_t){};
  if ((chunk == nullptr) || (scratch == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((chunk_cap == 0U) || (scratch_cap == 0U)) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t err = internal_validate_reader(rd);
  if (err != k_ra8_ok) {
    return err;
  }
  if (chunk_cap < rd->chunk_bytes) {
    return k_ra8_err_invalid_size;
  }
  err = internal_validate_workspaces(rd, chunk, rd->chunk_bytes, scratch, scratch_cap);
  if (err != k_ra8_ok) {
    return err;
  }
  chunk_validate_t ctx = {
    .rd         = rd,
    .chunk      = chunk,
    .loaded_idx = 0U,
    .loaded_len = 0U,
    .loaded     = false,
  };
  ra8_book_header_t validated = {};
  err                         = ra8_book_validate_stream_strict(internal_chunk_flat_read,
                                                                &ctx,
                                                                rd->inflated_total,
                                                                scratch,
                                                                scratch_cap,
                                                                &validated);
  if (err == k_ra8_ok) {
    *out_header = validated;
  }
  return err;
}
