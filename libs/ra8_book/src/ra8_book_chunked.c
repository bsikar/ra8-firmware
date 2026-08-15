/**
 * @file ra8_book_chunked.c
 * @brief Implementation of the demand-paged "RBKC" chunk reader.
 *
 * @details
 * Geometry parsing is shared with the resident open through
 * `priv_book_container_header_fields()` / `priv_book_container_table_entry()`
 * (ra8_book_internal.h), so the container format has exactly one in-firmware
 * definition. This TU adds the callback-driven half: the header and chunk
 * table are pulled through the caller's file reader at open, and each
 * ::ra8_book_chunked_read stages one compressed stream and inflates it into
 * the caller's frame.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#include "ra8_book_chunked.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_book_internal.h"
#include "ra8_check.h"

/** @brief Log tag for chunk-reader diagnostics. */
static const char* const s_tag_chunked = "ra8_book_chunked";

/**
 * @brief Validate a loaded chunk table's shape against the payload extent.
 *
 * @details
 * Requires `table[0] == 0`, strictly increasing entries, an end offset equal
 * to @p payload_len (the streams exactly tile the rest of the file), and
 * every chunk's compressed length within the staging budget. Split from
 * `internal_load_table` so each half stays within the function-size budget.
 *
 * @param[in] rd          Reader carrying the parsed header fields.
 * @param[in] table_buf   Loaded table (`chunk_count + 1` entries).
 * @param[in] payload_len Stream bytes remaining in the file after the table.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Table well-formed; every stream fits staging.
 * @retval k_ra8_err_invalid_arg  Bad start / non-monotonic / bad end offset.
 * @retval k_ra8_err_invalid_size A compressed chunk exceeds the staging budget.
 *
 * @pre `rd->chunk_count` and `rd->staging_cap` were populated by the caller.
 * @pre @p table_buf holds `rd->chunk_count + 1` decoded entries.
 * @post No state is modified (pure validation).
 * @post On k_ra8_ok every stream span is loadable through the staging buffer.
 *
 * @note Thread-safe: reads only its arguments.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_table_check(const ra8_book_chunked_t* rd, const uint64_t* table_buf, uint64_t payload_len)
{
  if (table_buf[0] != 0U) {
    return k_ra8_err_invalid_arg;
  }
  uint64_t max_clen = 0U;
  for (uint32_t i = 0U; i < rd->chunk_count; ++i) { /* bound: validated chunk_count */
    if (table_buf[i + 1U] <= table_buf[i]) {
      return k_ra8_err_invalid_arg;
    }
    const uint64_t clen = table_buf[i + 1U] - table_buf[i];
    if (clen > max_clen) {
      max_clen = clen;
    }
  }
  if (table_buf[rd->chunk_count] != payload_len) {
    return k_ra8_err_invalid_arg;
  }
  if (max_clen > (uint64_t)rd->staging_cap) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Load + validate the chunk table through the file reader.
 *
 * @details
 * Sizes the table from the already-parsed header fields in @p rd, bounds it
 * against @p table_cap_entries, the file length, and the one-call uint32 read
 * limit, reads it into @p table_buf in one burst, then walks it once to
 * require `offset[0] == 0`, strict monotonic growth, an end offset equal to
 * the payload length implied by @p file_len, and every compressed chunk
 * length within @p rd->staging_cap. On success `rd->table` and
 * `rd->payload_off` are bound.
 *
 * @param[in,out] rd                Reader carrying parsed header fields.
 * @param[in]     file_len          Container file length in bytes.
 * @param[out]    table_buf         Caller buffer receiving the table.
 * @param[in]     table_cap_entries Capacity of @p table_buf in uint64 entries.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Table loaded and validated; @p rd bound.
 * @retval k_ra8_err_invalid_arg  Table shape invalid (start, monotonicity,
 * end).
 * @retval k_ra8_err_invalid_size Table exceeds the caller buffer / file bounds
 * / one-call read limit, or a chunk exceeds staging.
 * @retval k_ra8_err_*            A file-read error, returned verbatim.
 *
 * @pre @p rd's header fields were populated by the caller.
 * @pre @p table_buf covers @p table_cap_entries uint64 slots.
 * @post On k_ra8_ok, `rd->table == table_buf` and `rd->payload_off` is set.
 * @post On any error `rd->table` stays NULL (reader unbound).
 *
 * @note Not thread-safe.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_load_table(ra8_book_chunked_t* rd,
                                                  uint64_t            file_len,
                                                  uint64_t*           table_buf,
                                                  uint32_t            table_cap_entries)
{
  const uint64_t entries     = (uint64_t)rd->chunk_count + 1U;
  const uint64_t table_bytes = entries * k_ra8_book_container_entry_len;
  if (entries > (uint64_t)table_cap_entries) {
    return k_ra8_err_invalid_size;
  }
  if (table_bytes > (uint64_t)UINT32_MAX) {
    return k_ra8_err_invalid_size;
  }
  if (file_len < ((uint64_t)k_ra8_book_container_header_len + table_bytes)) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t err = rd->file_read(rd->file_ctx,
                                      (uint64_t)k_ra8_book_container_header_len,
                                      (uint8_t*)table_buf,
                                      (uint32_t)table_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint64_t  payload_off = (uint64_t)k_ra8_book_container_header_len + table_bytes;
  const ra8_err_t shape       = internal_table_check(rd, table_buf, file_len - payload_off);
  if (shape != k_ra8_ok) {
    return shape;
  }
  rd->table             = table_buf;
  rd->table_cap_entries = table_cap_entries;
  rd->payload_off       = payload_off;
  return k_ra8_ok;
}

/**
 * @brief The argument-checked body of ::ra8_book_chunked_open.
 *
 * @details
 * Reads and parses the fixed header, then loads + validates the chunk table.
 * The caller has already null-checked every argument and pre-bound the
 * reader's callbacks and buffers. Split from the entry point so the
 * null-guard macros and the staged open logic each stay within the
 * function-size budget.
 *
 * @param[in,out] rd                Reader with callbacks/buffers pre-bound.
 * @param[in]     file_len          Container file length in bytes.
 * @param[in]     table_buf         Caller buffer for the chunk table.
 * @param[in]     table_cap_entries Capacity of @p table_buf in uint64 entries.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Reader bound; geometry fully validated.
 * @retval k_ra8_err_invalid_arg  Bad magic / header fields / table shape.
 * @retval k_ra8_err_invalid_size Short file or a caller buffer too small.
 * @retval k_ra8_err_*            A file-read error, returned verbatim.
 *
 * @pre Every pointer argument was null-checked by the caller.
 * @pre `rd->file_read` / `rd->staging` / `rd->staging_cap` are pre-bound.
 * @post On k_ra8_ok, `rd->table` is bound and reads may be served.
 * @post On any error `rd->table` stays NULL (reader unbound).
 *
 * @note Not thread-safe.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_chunked_open_body(ra8_book_chunked_t* rd,
                                                         uint64_t            file_len,
                                                         uint64_t*           table_buf,
                                                         uint32_t            table_cap_entries)
{
  if (file_len < (uint64_t)k_ra8_book_container_header_len) {
    return k_ra8_err_invalid_size;
  }
  uint8_t   hdr[k_ra8_book_container_header_len] = {};
  ra8_err_t err = rd->file_read(rd->file_ctx, 0U, hdr, (uint32_t)sizeof(hdr));
  if (err != k_ra8_ok) {
    return err;
  }
  err =
    priv_book_container_header_fields(hdr, &rd->chunk_bytes, &rd->inflated_total, &rd->chunk_count);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_load_table(rd, file_len, table_buf, table_cap_entries);
}

ra8_err_t ra8_book_chunked_open(ra8_book_chunked_t* rd,
                                ra8_vsource_read_fn file_read,
                                void*               file_ctx,
                                uint64_t            file_len,
                                ra8_book_inflate_fn inflate,
                                uint64_t*           table_buf,
                                uint32_t            table_cap_entries,
                                uint8_t*            staging,
                                uint32_t            staging_cap)
{
  RA8_CHECK_NULL_PTR(rd, s_tag_chunked, "open: null rd");
  RA8_CHECK_NULL_PTR((const void*)file_read, s_tag_chunked, "open: null file_read");
  RA8_CHECK_NULL_PTR((const void*)inflate, s_tag_chunked, "open: null inflate");
  RA8_CHECK_NULL_PTR(table_buf, s_tag_chunked, "open: null table_buf");
  RA8_CHECK_NULL_PTR(staging, s_tag_chunked, "open: null staging");

  *rd             = (ra8_book_chunked_t){};
  rd->file_read   = file_read;
  rd->file_ctx    = file_ctx;
  rd->inflate     = inflate;
  rd->staging     = staging;
  rd->staging_cap = staging_cap;
  return internal_chunked_open_body(rd, file_len, table_buf, table_cap_entries);
}

/**
 * @brief Stage one chunk's compressed stream and inflate it into @p buf.
 *
 * @details
 * The I/O tail of ::ra8_book_chunked_read after the geometry checks: reads
 * chunk @p idx's zlib stream into the staging buffer through the file
 * reader, inflates it into @p buf, and requires the produced length to equal
 * the requested span. Split out so the contract checks and the staged I/O
 * each stay within the function-size budget.
 *
 * @param[in]  rd  Bound reader (table validated at open).
 * @param[in]  idx Chunk index (`< chunk_count`, caller-bounded).
 * @param[out] buf Destination for exactly @p len inflated bytes.
 * @param[in]  len The chunk's exact inflated span.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Chunk inflated into @p buf.
 * @retval k_ra8_err_invalid_size The stream inflated to the wrong length.
 * @retval k_ra8_err_*            A file-read or inflater error, verbatim.
 *
 * @pre @p idx was bounds-checked against the validated chunk count.
 * @pre @p buf addresses at least @p len writable bytes.
 * @post On k_ra8_ok, `buf[0..len)` holds the chunk's inflated bytes.
 * @post On any error @p buf contents are unspecified.
 *
 * @note Not thread-safe: the staging buffer is reused across calls.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_stage_and_inflate(const ra8_book_chunked_t* rd, uint32_t idx, uint8_t* buf, uint32_t len)
{
  const uint64_t clen = rd->table[idx + 1U] - rd->table[idx];
  ra8_err_t      err =
    rd->file_read(rd->file_ctx, rd->payload_off + rd->table[idx], rd->staging, (uint32_t)clen);
  if (err != k_ra8_ok) {
    return err;
  }
  size_t produced = 0U;
  err             = rd->inflate(rd->staging, (size_t)clen, buf, (size_t)len, &produced);
  if (err != k_ra8_ok) {
    return err;
  }
  if (produced != (size_t)len) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_book_chunked_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag_chunked, "read: null ctx");
  RA8_CHECK_NULL_PTR(buf, s_tag_chunked, "read: null buf");
  const ra8_book_chunked_t* rd = (const ra8_book_chunked_t*)ctx;
  if (rd->table == nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (offset >= rd->inflated_total) {
    return k_ra8_err_out_of_range;
  }
  if ((offset % (uint64_t)rd->chunk_bytes) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  uint64_t expected = rd->inflated_total - offset;
  if (expected > (uint64_t)rd->chunk_bytes) {
    expected = rd->chunk_bytes;
  }
  if ((uint64_t)len != expected) {
    return k_ra8_err_invalid_arg;
  }
  return internal_stage_and_inflate(rd, (uint32_t)(offset / (uint64_t)rd->chunk_bytes), buf, len);
}
