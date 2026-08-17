/**
 * @file ra8_rabook_container.c
 * @brief Zero-heap streaming RBKC container writer.
 *
 * @details Serializes the documented RBKC header and offset table around
 * independently zlib-compressed flat-blob chunks. All storage and I/O arrive
 * through the public contract in ra8_rabook_container.h.
 *
 * [Ring 4 / EPUB Compiler] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_rabook_container.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_book.h"
#include "ra8_io_compress.h"

/** @brief RBKC fixed-field sizes and offsets. */
typedef enum : uint8_t {
  k_rbkc_chunk_bytes_off = 4U,  /**< uint32 chunk size.     */
  k_rbkc_total_off       = 8U,  /**< uint64 inflated total. */
  k_rbkc_count_off       = 16U, /**< uint32 chunk count.    */
  k_rbkc_reserved_off    = 20U, /**< uint32 reserved zero.  */
  k_rbkc_u32_bytes       = 4U,  /**< Little-endian uint32.  */
  k_rbkc_u64_bytes       = 8U,  /**< Little-endian uint64.  */
} rbkc_layout_t;

/** @brief Number of bits shifted per little-endian output byte. */
typedef enum : uint8_t {
  k_rbkc_bits_per_byte = 8U, /**< Binary bits represented by one output byte. */
} rbkc_bits_t;

/** @brief Canonical RBKC four-byte magic. */
static const uint8_t s_rbkc_magic[k_ra8_book_container_magic_len] = {'R', 'B', 'K', 'C'};

/**
 * @brief Encode one uint32 in little-endian order.
 * @details Writes all four bytes explicitly so output is host-endian independent.
 * @param[out] out Destination spanning four writable bytes.
 * @param[in] value Integer to serialize.
 * @pre @p out is non-NULL and has four writable bytes.
 * @pre @p value is the complete field value, without prior byte swapping.
 * @post `out[0..4)` contains the little-endian representation of @p value.
 * @post Memory outside the four-byte destination is unchanged.
 * @note Thread-safe for distinct output buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put_u32(uint8_t* out, uint32_t value)
{
  for (uint8_t i = 0U; i < (uint8_t)k_rbkc_u32_bytes; i++) {
    out[i] = (uint8_t)(value >> ((uint32_t)i * (uint32_t)k_rbkc_bits_per_byte));
  }
}

/**
 * @brief Encode one uint64 in little-endian order.
 * @details Writes all eight bytes explicitly so output is host-endian independent.
 * @param[out] out Destination spanning eight writable bytes.
 * @param[in] value Integer to serialize.
 * @pre @p out is non-NULL and has eight writable bytes.
 * @pre @p value is the complete field value, without prior byte swapping.
 * @post `out[0..8)` contains the little-endian representation of @p value.
 * @post Memory outside the eight-byte destination is unchanged.
 * @note Thread-safe for distinct output buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put_u64(uint8_t* out, uint64_t value)
{
  for (uint8_t i = 0U; i < (uint8_t)k_rbkc_u64_bytes; i++) {
    out[i] = (uint8_t)(value >> ((uint32_t)i * (uint32_t)k_rbkc_bits_per_byte));
  }
}

/**
 * @brief Call the destination once and require an exact successful write.
 * @details Preserves backend errors and converts a successful short write into
 *          ::k_ra8_err_invalid_size, preventing a torn container from succeeding.
 * @param[in] write_at Destination callback.
 * @param[in,out] ctx Destination callback context.
 * @param[in] offset Absolute staging-object byte offset.
 * @param[in] src Source spanning @p len readable bytes.
 * @param[in] len Exact number of bytes to write.
 * @return Exact-write or backend status.
 * @retval k_ra8_ok The callback stored exactly @p len bytes.
 * @retval k_ra8_err_invalid_size The callback reported a successful short write.
 * @pre @p write_at, @p ctx, and @p src satisfy the public callback contract.
 * @pre The destination range `[offset, offset + len)` is representable.
 * @post Success means the complete requested range reached the backend.
 * @post A backend failure is returned unchanged.
 * @note Not thread-safe unless the supplied backend context is synchronized.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_write_exact(ra8_rabook_write_at_fn write_at,
                                                   void*                  ctx,
                                                   uint64_t               offset,
                                                   const uint8_t*         src,
                                                   uint32_t               len)
{
  uint32_t        written = 0U;
  const ra8_err_t err     = write_at(ctx, offset, src, len, &written);
  if (err != k_ra8_ok) {
    return err;
  }
  return (written == len) ? k_ra8_ok : k_ra8_err_invalid_size;
}

/**
 * @brief Reject a missing callback, result, workspace, or workspace member.
 * @details Isolates the pointer-only precondition chain so the caller's own
 *          size and capacity checks stay readable as a short, flat sequence.
 * @param[in] read Source callback.
 * @param[in] write_at Destination callback.
 * @param[in] ws Caller-owned workspace to validate.
 * @param[in] out_len Required public result pointer.
 * @param[in] out_count Required chunk-count result pointer.
 * @return Pointer validation status.
 * @retval k_ra8_ok Every required pointer is non-NULL.
 * @retval k_ra8_err_null_ptr A callback, result, workspace, or member is NULL.
 * @pre None of the supplied pointers have been dereferenced yet.
 * @pre @p ws, when non-NULL, has not been mutated by this call.
 * @post No pointer is dereferenced.
 * @post The return value fully identifies whether validation may continue.
 * @note Thread-safe for distinct argument objects.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_pointers(ra8_rabook_flat_read_fn read,
                                                         ra8_rabook_write_at_fn  write_at,
                                                         const ra8_rabook_container_workspace_t* ws,
                                                         const uint64_t* out_len,
                                                         uint32_t*       out_count)
{
  if (read == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (write_at == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (ws == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_len == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_count == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (ws->input == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (ws->compressed == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (ws->compressor == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (ws->offsets == nullptr) {
    return k_ra8_err_null_ptr;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate pointers and workspace capacities before output mutation.
 * @details Derives the bounded chunk count only after required callback and
 *          workspace members pass their preconditions.
 * @param[in] read Source callback.
 * @param[in] flat_len Declared flat-blob length.
 * @param[in] chunk_bytes Requested inflated chunk size.
 * @param[in] write_at Destination callback.
 * @param[in] ws Caller-owned workspace to validate.
 * @param[in] out_len Required public result pointer.
 * @param[out] out_count Receives the derived chunk count.
 * @return Argument/workspace validation status.
 * @retval k_ra8_ok All arguments fit and @p out_count is set.
 * @retval k_ra8_err_null_ptr A callback, result, workspace, or member is NULL.
 * @retval k_ra8_err_invalid_arg A required size is zero.
 * @retval k_ra8_err_invalid_size A scratch or offset-table capacity is insufficient.
 * @pre The caller has not mutated the staging destination yet.
 * @pre Non-NULL @p ws members describe their true capacities.
 * @post Success produces a nonzero @p out_count fitting @p ws.
 * @post Failure does not call either callback or mutate workspace bytes.
 * @note Thread-safe for distinct argument objects.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate(ra8_rabook_flat_read_fn                 read,
                                                uint32_t                                flat_len,
                                                uint32_t                                chunk_bytes,
                                                ra8_rabook_write_at_fn                  write_at,
                                                const ra8_rabook_container_workspace_t* ws,
                                                const uint64_t*                         out_len,
                                                uint32_t*                               out_count)
{
  const ra8_err_t ptr_err = internal_validate_pointers(read, write_at, ws, out_len, out_count);
  if (ptr_err != k_ra8_ok) {
    return ptr_err;
  }
  if (flat_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (chunk_bytes == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (chunk_bytes > ws->input_cap) {
    return k_ra8_err_invalid_size;
  }
  if (ws->compressor_cap < (uint32_t)k_ra8_io_compress_scratch_bytes) {
    return k_ra8_err_invalid_size;
  }
  const uint32_t count = ((flat_len - 1U) / chunk_bytes) + 1U;
  if (count == UINT32_MAX) {
    return k_ra8_err_invalid_size;
  }
  if (ws->offset_cap < (count + 1U)) {
    return k_ra8_err_invalid_size;
  }
  *out_count = count;
  return k_ra8_ok;
}

/**
 * @brief Write the fixed header and reserve a zero-filled offset table.
 * @details Serializes canonical little-endian geometry, then writes exactly
 *          `count + 1` zero entries so later payload writes never depend on
 *          sparse-file behavior.
 * @param[in] write_at Destination callback.
 * @param[in,out] ctx Destination callback context.
 * @param[in] flat_len Declared inflated RABOOK1 length.
 * @param[in] chunk_bytes Inflated bytes per chunk.
 * @param[in] count Derived nonzero chunk count.
 * @return Prefix-write status.
 * @retval k_ra8_ok Header and complete table reservation were written.
 * @retval k_ra8_err_invalid_size A successful callback was short.
 * @pre Arguments were accepted by ::internal_validate.
 * @pre The destination is a private empty or overwrite-safe staging object.
 * @post Success reserves bytes through the future payload offset.
 * @post Failure is explicit and the caller will abort the staging object.
 * @note Not thread-safe with respect to @p ctx.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_write_prefix(ra8_rabook_write_at_fn write_at,
                                                    void*                  ctx,
                                                    uint32_t               flat_len,
                                                    uint32_t               chunk_bytes,
                                                    uint32_t               count)
{
  uint8_t header[k_ra8_book_container_header_len] = {};
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_book_container_magic_len; i++) {
    header[i] = s_rbkc_magic[i];
  }
  internal_put_u32(&header[k_rbkc_chunk_bytes_off], chunk_bytes);
  internal_put_u64(&header[k_rbkc_total_off], (uint64_t)flat_len);
  internal_put_u32(&header[k_rbkc_count_off], count);
  internal_put_u32(&header[k_rbkc_reserved_off], 0U);
  ra8_err_t err = internal_write_exact(write_at, ctx, 0U, header, (uint32_t)sizeof(header));
  if (err != k_ra8_ok) {
    return err;
  }
  const uint8_t zero[k_ra8_book_container_entry_len] = {};
  for (uint32_t i = 0U; i <= count; i++) {
    const uint64_t at = (uint64_t)k_ra8_book_container_header_len +
                        ((uint64_t)i * (uint64_t)k_ra8_book_container_entry_len);
    err               = internal_write_exact(write_at, ctx, at, zero, (uint32_t)sizeof(zero));
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Read, compress, and append every independent chunk payload.
 * @details Each exact flat range becomes one RFC 1950 stream; payload-relative
 *          start offsets are recorded for the later table back-fill.
 * @param[in] read Source callback.
 * @param[in,out] read_ctx Source callback context.
 * @param[in] flat_len Declared flat-blob length.
 * @param[in] chunk_bytes Inflated bytes per chunk.
 * @param[in] write_at Destination callback.
 * @param[in,out] write_ctx Destination callback context.
 * @param[in,out] ws Exclusive input/compression/table workspace.
 * @param[in] count Derived chunk count.
 * @param[in] payload_off Absolute first-payload byte offset.
 * @return Source, compression, or destination status.
 * @retval k_ra8_ok Every stream and the terminal table offset were produced.
 * @retval k_ra8_err_invalid_size A callback was short or an offset overflowed.
 * @pre Prefix/table reservation succeeded and all arguments passed ::internal_validate.
 * @pre @p ws buffers do not overlap and remain live throughout the loop.
 * @post Success sets offsets `[0..count]` monotonically with offset zero first.
 * @post Failure leaves no successful final-length claim.
 * @note Not thread-safe with respect to callback contexts or @p ws.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_write_chunks(ra8_rabook_flat_read_fn           read,
                                                    void*                             read_ctx,
                                                    uint32_t                          flat_len,
                                                    uint32_t                          chunk_bytes,
                                                    ra8_rabook_write_at_fn            write_at,
                                                    void*                             write_ctx,
                                                    ra8_rabook_container_workspace_t* ws,
                                                    uint32_t                          count,
                                                    uint64_t                          payload_off)
{
  uint32_t flat_off    = 0U;
  uint64_t payload_len = 0U;
  for (uint32_t i = 0U; i < count; i++) {
    const uint32_t remaining = flat_len - flat_off;
    const uint32_t want      = (remaining < chunk_bytes) ? remaining : chunk_bytes;
    uint32_t       got       = 0U;
    ra8_err_t      err       = read(read_ctx, flat_off, ws->input, want, &got);
    if (err != k_ra8_ok) {
      return err;
    }
    if (got != want) {
      return k_ra8_err_invalid_size;
    }
    uint32_t packed = 0U;
    err             = ra8_io_compress_zlib(ws->input,
                                           want,
                                           ws->compressed,
                                           ws->compressed_cap,
                                           ws->compressor,
                                           ws->compressor_cap,
                                           &packed);
    if (err != k_ra8_ok) {
      return err;
    }
    if ((uint64_t)packed > (UINT64_MAX - payload_len)) {
      return k_ra8_err_invalid_size;
    }
    if (payload_len > (UINT64_MAX - payload_off)) {
      return k_ra8_err_invalid_size;
    }
    ws->offsets[i] = payload_len;
    err =
      internal_write_exact(write_at, write_ctx, payload_off + payload_len, ws->compressed, packed);
    if (err != k_ra8_ok) {
      return err;
    }
    payload_len += packed;
    flat_off += want;
  }
  ws->offsets[count] = payload_len;
  return k_ra8_ok;
}

/**
 * @brief Back-fill the little-endian table after all payload streams exist.
 * @details Serializes every caller-workspace offset independently, including
 *          the terminal payload length at entry @p count.
 * @param[in] write_at Destination callback.
 * @param[in,out] ctx Destination callback context.
 * @param[in] ws Workspace holding `count + 1` finalized offsets.
 * @param[in] count Number of compressed payload streams.
 * @return Table-write status.
 * @retval k_ra8_ok Every offset entry was stored exactly.
 * @retval k_ra8_err_invalid_size A successful callback was short.
 * @pre ::internal_write_chunks succeeded and populated all entries.
 * @pre The table reservation from ::internal_write_prefix still exists.
 * @post Success makes the complete payload independently addressable.
 * @post Backend errors are propagated unchanged.
 * @note Not thread-safe with respect to @p ctx.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_write_table(ra8_rabook_write_at_fn                  write_at,
                                                   void*                                   ctx,
                                                   const ra8_rabook_container_workspace_t* ws,
                                                   uint32_t                                count)
{
  uint8_t encoded[k_ra8_book_container_entry_len];
  for (uint32_t i = 0U; i <= count; i++) {
    internal_put_u64(encoded, ws->offsets[i]);
    const uint64_t  at = (uint64_t)k_ra8_book_container_header_len +
                         ((uint64_t)i * (uint64_t)k_ra8_book_container_entry_len);
    const ra8_err_t err =
      internal_write_exact(write_at, ctx, at, encoded, (uint32_t)sizeof(encoded));
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

ra8_err_t ra8_rabook_container_write(ra8_rabook_flat_read_fn           read,
                                     void*                             read_ctx,
                                     uint32_t                          flat_len,
                                     uint32_t                          chunk_bytes,
                                     ra8_rabook_write_at_fn            write_at,
                                     void*                             write_ctx,
                                     ra8_rabook_container_workspace_t* ws,
                                     uint64_t*                         out_len)
{
  if (out_len != nullptr) {
    *out_len = 0U;
  }
  uint32_t        count = 0U;
  const ra8_err_t valid =
    internal_validate(read, flat_len, chunk_bytes, write_at, ws, out_len, &count);
  if (valid != k_ra8_ok) {
    return valid;
  }
  const uint64_t table_bytes = ((uint64_t)count + 1U) * (uint64_t)k_ra8_book_container_entry_len;
  const uint64_t payload_off = (uint64_t)k_ra8_book_container_header_len + table_bytes;
  ra8_err_t      err = internal_write_prefix(write_at, write_ctx, flat_len, chunk_bytes, count);
  if (err == k_ra8_ok) {
    err = internal_write_chunks(read,
                                read_ctx,
                                flat_len,
                                chunk_bytes,
                                write_at,
                                write_ctx,
                                ws,
                                count,
                                payload_off);
  }
  if (err == k_ra8_ok) {
    err = internal_write_table(write_at, write_ctx, ws, count);
  }
  if (err == k_ra8_ok) {
    if (ws->offsets[count] > (UINT64_MAX - payload_off)) {
      return k_ra8_err_invalid_size;
    }
    *out_len = payload_off + ws->offsets[count];
  }
  return err;
}
