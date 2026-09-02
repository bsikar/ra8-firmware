/**
 * @file book.c
 * @brief Implementation of the `.rabook` blob validator.
 *
 * @details
 * The only non-inline part of `book` is integrity/bounds validation. Walking
 * a validated blob is pure offset arithmetic and lives entirely in the header.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#include "book.h"

#include <string.h>

#include "book_internal.h"
#include "ra8_attributes.h"
#include "ra8_check.h"

/** @brief Log tag for `book` validation diagnostics. */
static const char* const s_tag_book = "book";

/**
 * @brief Copy an object representation through compatible byte-pointer types.
 * @details Centralizes the checked project's permitted bytewise object copy.
 * @param[out] dst Destination spanning at least @p len writable bytes.
 * @param[in] src Source spanning at least @p len readable bytes.
 * @param[in] len Number of bytes to copy.
 * @pre @p dst is writable for @p len bytes.
 * @pre @p src is readable for @p len bytes and does not overlap @p dst.
 * @post The first @p len destination bytes equal the source bytes on entry.
 * @post No bytes outside the destination span are modified.
 * @note Thread-safe when callers provide disjoint storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_copy_object(void* dst, const void* src, size_t len)
{
  (void)memcpy(dst, src, len);
}

/**
 * @enum book_crc_const_t
 * @brief Constants for the reflected CRC-32/ISO-HDLC used in the trailer.
 * @details Matches Python `zlib.crc32_val`, so a blob produced by
 *          `tools/epub_compile` verifies bit-for-bit on device.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_book_crc_init = 0xFFFFFFFFU, /**< CRC seed and final XOR mask. */
  k_book_crc_poly = 0xEDB88320U, /**< Reflected polynomial.        */
} book_crc_const_t;

typedef enum : uint8_t {
  k_book_crc_bits_per_byte = 8U, /**< Reflected updates per input byte. */
} book_crc_byte_t;

/**
 * @brief Extend a reflected CRC-32 over one byte span.
 *
 * @details
 * Computes a CRC-32/ISO-HDLC (reflected polynomial 0xEDB88320) over the byte
 * array [@p data, @p data + @p len). The algorithm seeds the accumulator with
 * @ref k_book_crc_init, folds each byte through the reflected polynomial,
 * then XORs the final value with @ref k_book_crc_init again. The
 * check value over "123456789" is 0xCBF43926, matching Python `zlib.crc32_val`.
 *
 * @param[in] crc Previous finalized CRC value; use zero for the first span.
 * @param[in] data Pointer to the byte array to checksum; must not be NULL.
 * @param[in] len Number of bytes to process; zero preserves @p crc.
 *
 * @return CRC-32 after extending @p crc with @p data.
 * @retval 0x00000000 Returned for an empty first span.
 * @retval 0xCBF43926 Check value for the ASCII string "123456789".
 *
 * @pre @p data is not NULL when @p len is greater than 0.
 * @pre @p len does not exceed the size of the allocation pointed to by @p data.
 * @post The returned value equals the CRC-32/ISO-HDLC of the input bytes.
 * @post Neither @p data nor any external state is modified.
 *
 * @note Not thread-safe if the read range overlaps a concurrent write.
 *
 * @since Version 0.1.0
 */
RA8_PRIV uint32_t priv_book_crc32_extend(uint32_t crc, const uint8_t* data, size_t len)
{
  uint32_t state = crc ^ k_book_crc_init;
  for (size_t i = 0U; i < len; ++i) {
    state ^= (uint32_t)data[i];
    for (uint8_t bit = 0U; bit < k_book_crc_bits_per_byte; ++bit) {
      const uint32_t mask = ((state & 1U) != 0U) ? k_book_crc_init : 0U;
      state               = (state >> 1U) ^ (k_book_crc_poly & mask);
    }
  }
  return state ^ k_book_crc_init;
}

/**
 * @brief Compute CRC-32/ISO-HDLC over one resident byte span.
 * @details Seeds the incremental implementation with zero so the result uses
 *          the same finalized wire convention as the streaming validator.
 * @param[in] data Readable bytes.
 * @param[in] len Number of bytes in @p data.
 * @return Finalized CRC for the span.
 * @retval UINT32_C(0) The span's CRC happens to be zero.
 * @retval UINT32_MAX The span's CRC happens to have every bit set.
 * @pre @p data addresses @p len bytes when @p len is non-zero.
 * @pre The immutable CRC lookup table is fully initialized at compile time.
 * @post No state is modified.
 * @post The result is identical to ::priv_book_crc32_extend called with a zero seed.
 * @note Thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
RA8_NO_RECURSION
RA8_INTERNAL static uint32_t internal_crc32(const uint8_t* data, size_t len)
{
  return priv_book_crc32_extend(0U, data, len);
}

/**
 * @brief Implementation of `internal_table_fits()` -- overflow-safe extent check.
 *
 * @details
 * Returns true when the half-open byte range [@p off, @p off + @p count *
 * @p elem) lies entirely within a blob of @p total bytes. Both the start
 * offset and the computed end are promoted to 64-bit before comparison so that
 * no 32-bit arithmetic can wrap on adversarially crafted blob fields, even when
 * @p count and @p elem together would overflow a 32-bit product.
 *
 * @param[in] off    Byte offset of the table's first element within the blob.
 * @param[in] count  Number of elements in the table.
 * @param[in] elem   Size in bytes of one table element.
 * @param[in] total  Total byte length of the blob (value from the header).
 *
 * @return bool Whether the described table fits inside the blob.
 * @retval true   The range [@p off, @p off + @p count * @p elem) is within @p total.
 * @retval false  The range overflows or exceeds @p total bytes.
 *
 * @pre @p total reflects the actual allocation backing the blob pointer.
 * @pre @p elem is non-zero; passing zero causes the range to collapse to @p off.
 * @post No memory is read or written; the result is a pure arithmetic predicate.
 * @post Returns false for any input combination that would overflow a 32-bit sum.
 *
 * @note Pure function with no shared state; thread-safe.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool
internal_table_fits(uint32_t off, uint32_t count, uint32_t elem, uint32_t total)
{
  uint64_t end = (uint64_t)off + ((uint64_t)count * (uint64_t)elem);
  return (off <= total) && (end <= (uint64_t)total);
}

/**
 * @brief Whether every image descriptor declares a pixel format this build knows.
 *
 * @details
 * Rejects a blob carrying a @ref book_image_t::pixel_format newer than
 * @ref k_book_pixfmt_gray8 -- the same fail-closed stance book_validate()
 * takes on an unknown header feature bit: a depth this firmware cannot unpack must
 * be refused, not fed to the wrong blit and mis-rendered. Every pre-field blob
 * zero-filled the byte (== @ref k_book_pixfmt_gray4), so this never rejects an
 * older gray4 book. Called only after the image table's bounds are validated, so
 * book_images() is in range for all @p hdr->image_count entries.
 *
 * @param[in] base Blob base already bounds-checked by the caller (non-NULL).
 * @param[in] hdr  Header view of @p base whose image table was already validated.
 *
 * @return Whether every image's declared pixel format is known to this build.
 * @retval true  Every descriptor's `pixel_format` is <= k_book_pixfmt_gray8.
 * @retval false At least one descriptor names a depth this firmware cannot decode.
 *
 * @pre The image table [image_off, image_off + image_count * sizeof(image)) fits.
 * @pre @p base and @p hdr are non-NULL and describe the same blob.
 * @post No memory is written (pure read over the immutable image table).
 * @post The result depends only on the descriptors, not on any pool bytes.
 *
 * @note Thread-safe: read-only over immutable data.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_image_pixfmts_known(const void* base, const book_header_t* hdr)
{
  const book_image_t* imgs  = book_images(base);
  bool                known = true;
  for (uint32_t i = 0U; i < hdr->image_count; ++i) { /* bound: validated image_count */
    if (imgs[i].pixel_format > (uint8_t)k_book_pixfmt_gray8) {
      known = false;
    }
  }
  return known;
}

/**
 * @brief Whether the header's magic field equals the "RABOOK1" tag.
 *
 * @details
 * Compares all 8 magic bytes (7 chars + NUL) against the fixed "RABOOK1"
 * signature. Factored out of book_validate() so that function stays within
 * the readability-function-size / NASA Rule 4 statement budget; the check is a
 * pure read over the caller-provided header.
 *
 * @param[in] hdr Header view of a blob whose size was already bounds-checked.
 *
 * @return Whether every magic byte matches the "RABOOK1" signature.
 * @retval true  All 8 magic bytes match.
 * @retval false At least one magic byte differs.
 *
 * @pre @p hdr is non-NULL and points at a blob of at least sizeof(header).
 * @pre The magic[] array is fully in range (guaranteed by the size precheck).
 * @post No memory is written (pure read over the header).
 * @post The result depends only on hdr->magic, not on any body bytes.
 *
 * @note Thread-safe: read-only over immutable data.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_magic_ok(const book_header_t* hdr)
{
  const char expect[8] = {'R', 'A', 'B', 'O', 'O', 'K', '1', '\0'};
  bool       ok        = true;
  for (uint8_t i = 0U; i < (uint8_t)sizeof(expect); ++i) {
    if (hdr->magic[i] != expect[i]) {
      ok = false;
    }
  }
  return ok;
}

ra8_err_t book_validate(const void* base, size_t size)
{
  RA8_CHECK_NULL_PTR(base, s_tag_book, "validate: null base");

  if (size < sizeof(book_header_t)) {
    return k_ra8_err_invalid_size;
  }

  const book_header_t* hdr = (const book_header_t*)base;

  if (!internal_magic_ok(hdr)) {
    return k_ra8_err_invalid_arg;
  }
  if (hdr->format_version != k_book_format_version) {
    return k_ra8_err_invalid_arg;
  }
  /* Unknown feature bits mean the blob relies on a presentation semantic this
   * firmware does not implement (e.g. a reading-order mode newer than
   * book_flag_t) -- refuse it rather than silently mis-render it. */
  if ((hdr->flags & ~(uint32_t)k_book_flag_mask_known) != 0U) {
    return k_ra8_err_invalid_arg;
  }

  uint32_t total = hdr->total_size;
  if ((total < sizeof(book_header_t)) || ((size_t)total > size)) {
    return k_ra8_err_invalid_size;
  }

  const bool tables_ok =
    internal_table_fits(hdr->chapter_off, hdr->chapter_count, sizeof(book_chapter_t), total) &&
    internal_table_fits(hdr->node_off, hdr->node_count, sizeof(book_node_t), total) &&
    internal_table_fits(hdr->attr_off, hdr->attr_count, sizeof(book_attr_t), total) &&
    internal_table_fits(hdr->stylesheet_off,
                        hdr->stylesheet_count,
                        sizeof(book_stylesheet_t),
                        total) &&
    internal_table_fits(hdr->image_off, hdr->image_count, sizeof(book_image_t), total) &&
    internal_table_fits(hdr->string_off, hdr->string_size, 1U, total) &&
    internal_table_fits(hdr->image_pool_off, hdr->image_pool_size, 1U, total);
  if (!tables_ok) {
    return k_ra8_err_invalid_size;
  }

  /* Every image descriptor must name a pixel depth this build can unpack; an
   * unknown depth is refused (fail-closed) rather than fed to the wrong blit. */
  if (!internal_image_pixfmts_known(base, hdr)) {
    return k_ra8_err_invalid_arg;
  }

  const uint8_t* body     = &((const uint8_t*)base)[sizeof(book_header_t)];
  uint32_t       body_len = total - (uint32_t)sizeof(book_header_t);
  if (internal_crc32(body, body_len) != hdr->crc32_val) {
    return k_ra8_err_range_check_failed;
  }

  return k_ra8_ok;
}

/** @brief Implementation of `priv_book_container_header_fields()` -- memcpy field decode. */
RA8_PRIV ra8_err_t priv_book_container_header_fields(const uint8_t* hdr,
                                                     uint32_t*      out_chunk_bytes,
                                                     uint64_t*      out_total,
                                                     uint32_t*      out_count)
{
  RA8_CHECK_NULL_PTR(hdr, s_tag_book, "hdr fields: null hdr");
  RA8_CHECK_NULL_PTR(out_chunk_bytes, s_tag_book, "hdr fields: null out");
  const char cmagic[k_book_container_magic_len] = {'R', 'B', 'K', 'C'};
  for (uint8_t i = 0U; i < k_book_container_magic_len; ++i) {
    if ((char)hdr[i] != cmagic[i]) {
      return k_ra8_err_invalid_arg;
    }
  }
  uint32_t chunk_bytes = 0U;
  uint64_t total       = 0U;
  uint32_t count       = 0U;
  uint32_t reserved    = 0U;
  internal_copy_object(&chunk_bytes, &hdr[k_book_cont_off_chunk_bytes], sizeof(chunk_bytes));
  internal_copy_object(&total, &hdr[k_book_cont_off_total], sizeof(total));
  internal_copy_object(&count, &hdr[k_book_cont_off_count], sizeof(count));
  internal_copy_object(&reserved, &hdr[k_book_cont_off_reserved], sizeof(reserved));
  if ((chunk_bytes == 0U) || (total == 0U) || (reserved != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  const uint64_t want_count = (total + (uint64_t)chunk_bytes - 1U) / (uint64_t)chunk_bytes;
  if ((uint64_t)count != want_count) {
    return k_ra8_err_invalid_arg;
  }
  *out_chunk_bytes = chunk_bytes;
  *out_total       = total;
  *out_count       = count;
  return k_ra8_ok;
}

/** @brief Implementation of `priv_book_container_table_entry()` -- unaligned-safe memcpy load. */
RA8_PRIV uint64_t priv_book_container_table_entry(const uint8_t* table, uint32_t idx)
{
  uint64_t entry = 0U;
  internal_copy_object(&entry, &table[(size_t)idx * k_book_container_entry_len], sizeof(entry));
  return entry;
}

/**
 * @struct book_container_view_t
 * @brief Parsed, bounds-checked view of a resident "RBKC" container file.
 * @details Populated by `internal_container_view()`; every pointer aims into the
 *          caller's file buffer and every extent was verified against
 *          @c file_len, so the inflate loop can trust it without re-checking.
 * @invariant `table` covers `chunk_count + 1` entries; `payload` covers
 *            `payload_len` bytes; both lie within the original file buffer.
 * @since Version 0.1.0
 */
typedef struct {
  const uint8_t* table;       /**< First chunk-table byte (unaligned).    */
  const uint8_t* payload;     /**< First byte of the chunk zlib streams.  */
  uint64_t       total;       /**< Flat-blob inflated total in bytes.     */
  uint64_t       payload_len; /**< Concatenated stream bytes in the file. */
  uint32_t       chunk_bytes; /**< Inflated bytes per chunk (last short). */
  uint32_t       chunk_count; /**< Number of chunks.                      */
} book_container_view_t;

/**
 * @brief Parse + bounds-check a resident "RBKC" container against its file.
 *
 * @details
 * Decodes the fixed header via `priv_book_container_header_fields()`, verifies
 * the header plus chunk table fit inside @p file_len, verifies the inflated
 * total fits @p scratch_cap, then walks the chunk table once to require
 * `offset[0] == 0`, strict monotonic growth, and
 * `offset[chunk_count] == payload_len` (the streams exactly tile the rest of
 * the file). On success @p out_view carries pointers the inflate loop can
 * trust without further checks.
 *
 * @param[in]  bytes       First byte of the container file.
 * @param[in]  file_len    Readable length of @p bytes.
 * @param[in]  scratch_cap Caller's inflate scratch capacity in bytes.
 * @param[out] out_view    Receives the validated view.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               View populated; geometry fully validated.
 * @retval k_ra8_err_invalid_arg  Bad magic / header fields / chunk-table shape.
 * @retval k_ra8_err_invalid_size File shorter than header + table, or the
 *                               inflated total exceeds @p scratch_cap.
 *
 * @pre @p bytes is non-NULL and points at @p file_len readable bytes.
 * @pre @p out_view is non-NULL and writable.
 * @post On k_ra8_ok every @p out_view extent lies inside the file buffer.
 * @post On any error @p out_view is not fully populated and must not be used.
 *
 * @note Thread-safe: reads only caller memory; no global state.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_container_view(const uint8_t*         bytes,
                                                      size_t                 file_len,
                                                      size_t                 scratch_cap,
                                                      book_container_view_t* out_view)
{
  if (file_len < k_book_container_header_len) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t err = priv_book_container_header_fields(bytes,
                                                    &out_view->chunk_bytes,
                                                    &out_view->total,
                                                    &out_view->chunk_count);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint64_t entries     = (uint64_t)out_view->chunk_count + 1U;
  const uint64_t table_bytes = entries * k_book_container_entry_len;
  if ((uint64_t)file_len < ((uint64_t)k_book_container_header_len + table_bytes)) {
    return k_ra8_err_invalid_size;
  }
  if ((uint64_t)scratch_cap < out_view->total) {
    return k_ra8_err_invalid_size;
  }
  out_view->table       = &bytes[k_book_container_header_len];
  out_view->payload     = &out_view->table[table_bytes];
  out_view->payload_len = (uint64_t)file_len - k_book_container_header_len - table_bytes;
  uint64_t prev         = priv_book_container_table_entry(out_view->table, 0U);
  if (prev != 0U) {
    return k_ra8_err_invalid_arg;
  }
  for (uint32_t i = 1U; i <= out_view->chunk_count; ++i) { /* bound: validated chunk_count */
    const uint64_t cur = priv_book_container_table_entry(out_view->table, i);
    if (cur <= prev) {
      return k_ra8_err_invalid_arg;
    }
    prev = cur;
  }
  if (prev != out_view->payload_len) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Inflate every chunk of a validated container view into @p scratch.
 *
 * @details
 * Walks the chunk table in order; chunk `i`'s zlib stream occupies
 * `[payload + offset[i], payload + offset[i+1])` and must inflate to exactly
 * `min(chunk_bytes, total - i * chunk_bytes)` bytes, written at
 * `scratch + i * chunk_bytes`. A produced-length mismatch on any chunk aborts
 * (truncated / corrupt stream). On success @p scratch holds the reassembled
 * flat blob of exactly `view->total` bytes.
 *
 * @param[in]  view    Validated view from `internal_container_view()`.
 * @param[in]  inflate Caller decompressor (see @ref book_inflate_fn).
 * @param[out] scratch Destination for the reassembled blob.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Every chunk inflated to its exact span.
 * @retval k_ra8_err_invalid_size A chunk inflated to the wrong length.
 * @retval k_ra8_err_*            The inflater's own error, returned verbatim.
 *
 * @pre @p view was accepted by `internal_container_view()` for this scratch capacity.
 * @pre @p inflate and @p scratch are non-NULL (checked by the caller).
 * @post On k_ra8_ok, `scratch[0..view->total)` is the flat blob.
 * @post On any error @p scratch contents are unspecified.
 *
 * @note Not thread-safe: writes @p scratch.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_inflate_chunks(const book_container_view_t* view,
                                                      book_inflate_fn              inflate,
                                                      uint8_t*                     scratch)
{
  for (uint32_t i = 0U; i < view->chunk_count; ++i) { /* bound: validated chunk_count */
    const uint64_t off      = priv_book_container_table_entry(view->table, i);
    const uint64_t next     = priv_book_container_table_entry(view->table, i + 1U);
    const uint64_t dst_off  = (uint64_t)i * view->chunk_bytes;
    uint64_t       expected = view->total - dst_off;
    if (expected > (uint64_t)view->chunk_bytes) {
      expected = view->chunk_bytes;
    }
    size_t          produced = 0U;
    const ra8_err_t err      = inflate(&view->payload[(size_t)off],
                                       (size_t)(next - off),
                                       &scratch[(size_t)dst_off],
                                       (size_t)expected,
                                       &produced);
    if (err != k_ra8_ok) {
      return err;
    }
    if (produced != (size_t)expected) {
      return k_ra8_err_invalid_size;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Parse, inflate, validate, and publish an already-guarded open.
 *
 * @details
 * The argument-checked body of book_open(): builds the container view,
 * inflates every chunk into @p scratch, validates the reassembled blob, and
 * publishes the base/size outputs. Split from the entry point so the
 * null-guard macros and the staged error chain each stay within the
 * function-size budget.
 *
 * @param[in]  bytes       Container file bytes (non-NULL, caller-checked).
 * @param[in]  file_len    Readable length of @p bytes.
 * @param[in]  inflate     Caller decompressor (non-NULL, caller-checked).
 * @param[out] scratch     Destination for the reassembled blob.
 * @param[in]  scratch_cap Capacity of @p scratch in bytes.
 * @param[out] out_base    Receives the validated blob base.
 * @param[out] out_size    Receives the inflated blob length.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Blob inflated, validated, and published.
 * @retval k_ra8_err_invalid_arg  Malformed container header / chunk table.
 * @retval k_ra8_err_invalid_size Short file, scratch too small, or a chunk
 *                               inflated to the wrong span.
 * @retval k_ra8_err_range_check_failed Blob CRC mismatch.
 *
 * @pre Every pointer argument was null-checked by the caller.
 * @pre @p scratch addresses at least @p scratch_cap writable bytes.
 * @post On k_ra8_ok, `*out_base == scratch` and `*out_size` is the blob length.
 * @post On any error the outputs are not modified.
 *
 * @note Not thread-safe: writes @p scratch.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_open_body(const uint8_t*  bytes,
                                                 size_t          file_len,
                                                 book_inflate_fn inflate,
                                                 void*           scratch,
                                                 size_t          scratch_cap,
                                                 const void**    out_base,
                                                 size_t*         out_size)
{
  book_container_view_t view = {};
  ra8_err_t             err  = internal_container_view(bytes, file_len, scratch_cap, &view);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_inflate_chunks(&view, inflate, (uint8_t*)scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  err = book_validate(scratch, (size_t)view.total);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_base = scratch;
  *out_size = (size_t)view.total;
  return k_ra8_ok;
}

ra8_err_t book_open(const void*     file,
                    size_t          file_len,
                    book_inflate_fn inflate,
                    void*           scratch,
                    size_t          scratch_cap,
                    const void**    out_base,
                    size_t*         out_size)
{
  RA8_CHECK_NULL_PTR(file, s_tag_book, "open: null file");
  RA8_CHECK_NULL_PTR(inflate, s_tag_book, "open: null inflate");
  RA8_CHECK_NULL_PTR(scratch, s_tag_book, "open: null scratch");
  RA8_CHECK_NULL_PTR(out_base, s_tag_book, "open: null out_base");
  RA8_CHECK_NULL_PTR(out_size, s_tag_book, "open: null out_size");

  return internal_open_body((const uint8_t*)file,
                            file_len,
                            inflate,
                            scratch,
                            scratch_cap,
                            out_base,
                            out_size);
}
