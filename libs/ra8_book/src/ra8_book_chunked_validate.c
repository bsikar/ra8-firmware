/**
 * @file ra8_book_chunked_validate.c
 * @brief Strict streaming-validation adapter for an open RBKC chunk reader.
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
  uint8_t*            chunk;      /**< Caller-owned inflated chunk.      */
  uint32_t            loaded_idx; /**< Cached chunk index.               */
  uint32_t            loaded_len; /**< Exact cached inflated byte span.  */
  bool                loaded;     /**< Whether the cache contains data.  */
} chunk_validate_t;

/**
 * @brief Inflate one requested chunk into the caller cache when not resident.
 * @param[in,out] ctx Adapter state.
 * @param[in] idx Chunk index to make resident.
 * @return Chunk-reader status.
 * @pre @p idx is less than the open reader's chunk count.
 * @post Success makes @p idx and its exact span resident in ctx->chunk.
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
 * @param[in,out] opaque Adapter context.
 * @param[in] offset Flat-blob byte offset.
 * @param[out] dst Destination for exactly @p len bytes.
 * @param[in] len Exact byte count.
 * @return Adapter, chunk-reader, or range status.
 * @pre Public validation established every pointer and workspace capacity.
 * @post Success fills all @p len destination bytes.
 * @note Reads spanning chunk boundaries are copied iteratively without recursion.
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
  *out_header = (ra8_book_header_t){};
  if ((rd == nullptr) || (chunk == nullptr) || (scratch == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((rd->table == nullptr) || (rd->chunk_bytes == 0U) || (rd->chunk_count == 0U)) {
    return k_ra8_err_invalid_state;
  }
  if ((chunk_cap < rd->chunk_bytes) || (scratch_cap == 0U)) {
    return k_ra8_err_invalid_size;
  }
  chunk_validate_t ctx = {
    .rd         = rd,
    .chunk      = chunk,
    .loaded_idx = 0U,
    .loaded_len = 0U,
    .loaded     = false,
  };
  return ra8_book_validate_stream_strict(internal_chunk_flat_read,
                                         &ctx,
                                         rd->inflated_total,
                                         scratch,
                                         scratch_cap,
                                         out_header);
}
