/**
 * @file ra8_rabook_finalize.c
 * @brief Canonical RABOOK1 serialization and finalization.
 *
 * @details
 * Computes the wire layout over an immutable builder context, folds the body
 * CRC-32, and publishes the header plus body through an exact-write callback.
 * Image payloads may reside in the builder arena or behind a bounded external
 * reader. The memory finalizer is a bounded sink adapter over the same streaming
 * path, so both destinations produce identical bytes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "rabook_compile.h"

/** @brief Component tag for finalizer diagnostics. */
static const char* const s_tag_rabook = "ra8_rabook_finalize";

/**
 * @enum ra8_rabook_crc_t
 * @brief CRC-32/ISO-HDLC constants matching @ref book_validate.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_rabook_crc_init = 0xFFFFFFFFU, /**< CRC seed and final XOR mask. */
  k_rabook_crc_poly = 0xEDB88320U, /**< Reflected CRC-32 polynomial. */
} ra8_rabook_crc_t;

/**
 * @enum ra8_rabook_crc_bits_t
 * @brief Per-byte fold count for the bitwise CRC inner loop.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_rabook_crc_byte_bits = 8U, /**< Bit folds per input byte. */
} ra8_rabook_crc_bits_t;

/** @brief Image-pool backing recorded by the builder context. */
typedef enum : uint8_t {
  k_rabook_pool_external = 2U, /**< Bytes live behind the read callback. */
} ra8_rabook_pool_mode_t;

/**
 * @brief Continue a CRC-32/ISO-HDLC accumulator over one byte range.
 * @details Folds eight bits per byte with the reflected
 *          @ref k_rabook_crc_poly. The caller seeds with
 *          @ref k_rabook_crc_init and applies the final XOR after every segment,
 *          yielding the exact variant @ref book_validate expects.
 * @param[in] crc  Incoming, not-yet-finalized CRC accumulator.
 * @param[in] data Byte range to checksum (may be NULL iff @p len is 0).
 * @param[in] len  Number of bytes at @p data.
 * @return Updated, not-yet-finalized CRC accumulator.
 * @retval uint32_t Accumulator after all @p len bytes have been folded.
 * @pre @p data addresses at least @p len readable bytes (or @p len is 0).
 * @pre @p len is the true range length (no over-read).
 * @post A zero-length range returns @p crc unchanged.
 * @post @p data is not modified (read-only checksum).
 * @note Not thread-safe in the sense of shared state, but has none; pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_crc_update(uint32_t crc, const uint8_t* data, uint32_t len)
{
  uint32_t running_crc = crc;
  for (uint32_t i = 0U; i < len; i++) {
    running_crc ^= (uint32_t)data[i];
    for (uint32_t bit = 0U; bit < (uint32_t)k_rabook_crc_byte_bits; bit++) {
      const uint32_t lsb  = running_crc & 1U;
      const uint32_t mask = 0U - lsb;
      running_crc         = (running_crc >> 1U) ^ ((uint32_t)k_rabook_crc_poly & mask);
    }
  }
  return running_crc;
}

/**
 * @struct ra8_rabook_layout_t
 * @brief Computed byte offsets of every table / pool plus the total blob size.
 * @details All offsets are measured from the start of the output buffer; each
 *          table abuts the next so a table's byte length is the delta to the
 *          following offset.
 * @invariant `k_book_sizeof_header <= off_chap <= off_node <= ... <= total`.
 * @since 0.1.0
 */
typedef struct {
  uint32_t off_chap;   /**< Chapter table offset (== header size). */
  uint32_t off_node;   /**< Node table offset.                     */
  uint32_t off_attr;   /**< Attribute table offset.                */
  uint32_t off_style;  /**< Stylesheet table offset.               */
  uint32_t off_image;  /**< Image table offset.                    */
  uint32_t off_string; /**< String-pool offset.                    */
  uint32_t off_pool;   /**< Image-pool offset.                     */
  uint32_t total;      /**< Total blob size in bytes.              */
} ra8_rabook_layout_t;

/**
 * @brief Compute the table / pool offsets and total blob size, overflow-guarded.
 * @details Sums the fixed header, the five count*record-size tables, and the two
 *          pools in 64-bit arithmetic so no intermediate product or running offset
 *          can wrap a 32-bit value; the final total is rejected if it exceeds
 *          @c UINT32_MAX before being narrowed back into @p lay.
 * @param[in]  ctx Builder context with the running counts / sizes (non-NULL).
 * @param[out] lay Receives the contract offsets and total (non-NULL).
 * @return Error code.
 * @retval k_ra8_ok               Offsets computed and stored in @p lay.
 * @retval k_ra8_err_invalid_size The blob would exceed 32-bit addressing.
 * @pre @p ctx and @p lay are non-NULL (caller-validated).
 * @pre Each `*_count` / `*_size` reflects the bytes actually appended.
 * @post On k_ra8_ok, `lay` offsets are monotonically non-decreasing.
 * @post On error @p lay is left in an indeterminate state and must be ignored.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_compute_layout(const ra8_rabook_ctx_t* ctx, ra8_rabook_layout_t* lay)
{
  const uint64_t chap_bytes  = (uint64_t)ctx->chapter_count * (uint64_t)k_book_sizeof_chapter;
  const uint64_t node_bytes  = (uint64_t)ctx->node_count * (uint64_t)k_book_sizeof_node;
  const uint64_t attr_bytes  = (uint64_t)ctx->attr_count * (uint64_t)k_book_sizeof_attr;
  const uint64_t style_bytes = (uint64_t)ctx->stylesheet_count * (uint64_t)k_book_sizeof_stylesheet;
  const uint64_t image_bytes = (uint64_t)ctx->image_count * (uint64_t)k_book_sizeof_image;

  const uint64_t off_chap   = (uint64_t)k_book_sizeof_header;
  const uint64_t off_node   = off_chap + chap_bytes;
  const uint64_t off_attr   = off_node + node_bytes;
  const uint64_t off_style  = off_attr + attr_bytes;
  const uint64_t off_image  = off_style + style_bytes;
  const uint64_t off_string = off_image + image_bytes;
  const uint64_t off_pool   = off_string + (uint64_t)ctx->string_size;
  const uint64_t total      = off_pool + (uint64_t)ctx->image_pool_size;

  if (total > (uint64_t)UINT32_MAX) {
    ra8_log_error(s_tag_rabook, "finalize: layout overflows 32-bit offsets");
    return k_ra8_err_invalid_size;
  }

  lay->off_chap   = (uint32_t)off_chap;
  lay->off_node   = (uint32_t)off_node;
  lay->off_attr   = (uint32_t)off_attr;
  lay->off_style  = (uint32_t)off_style;
  lay->off_image  = (uint32_t)off_image;
  lay->off_string = (uint32_t)off_string;
  lay->off_pool   = (uint32_t)off_pool;
  lay->total      = (uint32_t)total;
  return k_ra8_ok;
}

/** @brief One resident body segment in canonical serialization order. */
typedef struct {
  const uint8_t* data; /**< First readable byte. */
  uint32_t       len;  /**< Segment byte count.  */
} ra8_rabook_segment_t;

/** @brief Number of resident body segments before the image-pool payload. */
typedef enum : uint8_t {
  k_rabook_segment_chapters = 0U, /**< Chapter table.                    */
  k_rabook_segment_nodes    = 1U, /**< DOM node table.                   */
  k_rabook_segment_attrs    = 2U, /**< DOM attribute table.              */
  k_rabook_segment_styles   = 3U, /**< Stylesheet table.                 */
  k_rabook_segment_images   = 4U, /**< Image descriptor table.           */
  k_rabook_segment_strings  = 5U, /**< Interned string pool.             */
  k_rabook_segment_count    = 6U, /**< Five tables plus the string pool. */
} ra8_rabook_segment_count_t;

/**
 * @brief Populate the canonical resident body-segment sequence.
 * @details Forms views over the five fixed tables and string pool; the external
 *          or internal image payload remains a separate final segment.
 * @param[in]  ctx Builder holding resident table and string storage.
 * @param[in]  lay Computed canonical section offsets.
 * @param[out] seg Six writable segment descriptors.
 * @pre All arguments are non-NULL.
 * @pre @p lay was computed from @p ctx.
 * @post Every segment points into a caller-owned builder arena.
 * @post Segment lengths exactly cover the body through the string pool.
 * @note The returned views remain valid while the builder arenas remain alive.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_segments(const ra8_rabook_ctx_t*    ctx,
                              const ra8_rabook_layout_t* lay,
                              ra8_rabook_segment_t       seg[k_rabook_segment_count])
{
  seg[k_rabook_segment_chapters] =
    (ra8_rabook_segment_t){(const uint8_t*)ctx->buf.chapters, lay->off_node - lay->off_chap};
  seg[k_rabook_segment_nodes] =
    (ra8_rabook_segment_t){(const uint8_t*)ctx->buf.nodes, lay->off_attr - lay->off_node};
  seg[k_rabook_segment_attrs] =
    (ra8_rabook_segment_t){(const uint8_t*)ctx->buf.attrs, lay->off_style - lay->off_attr};
  seg[k_rabook_segment_styles] =
    (ra8_rabook_segment_t){(const uint8_t*)ctx->buf.stylesheets, lay->off_image - lay->off_style};
  seg[k_rabook_segment_images] =
    (ra8_rabook_segment_t){(const uint8_t*)ctx->buf.images, lay->off_string - lay->off_image};
  seg[k_rabook_segment_strings] =
    (ra8_rabook_segment_t){(const uint8_t*)ctx->buf.string_pool, ctx->string_size};
}

/**
 * @brief Read one exact external-pool chunk, rejecting a short success.
 * @details Calls the supplied reader once and converts a successful short count
 *          into @ref k_ra8_err_invalid_size.
 * @param[in]  read Reader callback.
 * @param[in,out] read_ctx Reader context.
 * @param[in]  offset Logical pool offset.
 * @param[out] dst Destination buffer.
 * @param[in]  len Exact requested byte count.
 * @return Exact-transfer status.
 * @retval k_ra8_ok Exactly @p len bytes were read.
 * @retval k_ra8_err_invalid_size The callback reported a short success.
 * @pre Callback and destination pointers are non-NULL.
 * @pre @p dst spans @p len writable bytes.
 * @post Success initializes every requested destination byte.
 * @post A callback error is propagated unchanged.
 * @note Does not retry partial reads.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_read_exact(ra8_rabook_image_read_fn read,
                                     void*                    read_ctx,
                                     uint32_t                 offset,
                                     uint8_t*                 dst,
                                     uint32_t                 len)
{
  uint32_t        got = 0U;
  const ra8_err_t err = read(read_ctx, offset, dst, len, &got);
  if (err != k_ra8_ok) {
    return err;
  }
  return (got == len) ? k_ra8_ok : k_ra8_err_invalid_size;
}

/**
 * @brief Append one exact stream chunk, rejecting a short success.
 * @details Zero-length segments are skipped; non-empty segments are submitted
 *          once and must be accepted completely.
 * @param[in] write Writer callback.
 * @param[in,out] write_ctx Writer context.
 * @param[in] src Source bytes.
 * @param[in] len Exact requested byte count.
 * @return Exact-transfer status.
 * @retval k_ra8_ok The complete segment was appended, or @p len was zero.
 * @retval k_ra8_err_invalid_size The callback reported a short success.
 * @pre @p write is non-NULL.
 * @pre @p src spans @p len readable bytes when @p len is nonzero.
 * @post Success advances the logical destination by @p len bytes.
 * @post A callback error is propagated unchanged.
 * @note Does not retry partial writes.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_write_exact(ra8_rabook_write_fn write, void* write_ctx, const uint8_t* src, uint32_t len)
{
  if (len == 0U) {
    return k_ra8_ok;
  }
  uint32_t        wrote = 0U;
  const ra8_err_t err   = write(write_ctx, src, len, &wrote);
  if (err != k_ra8_ok) {
    return err;
  }
  return (wrote == len) ? k_ra8_ok : k_ra8_err_invalid_size;
}

/**
 * @brief Fold the logical image pool into an in-progress CRC.
 * @details Reads internal bytes directly or walks an external pool through
 *          bounded caller scratch, preserving one CRC accumulator across chunks.
 * @param[in] ctx Builder describing the logical image pool.
 * @param[in] read External reader, when selected.
 * @param[in,out] read_ctx External reader context.
 * @param[in,out] scratch External transfer scratch.
 * @param[in] scratch_cap Scratch capacity in bytes.
 * @param[in,out] crc Running CRC accumulator.
 * @return Pool-read status.
 * @retval k_ra8_ok Every pool byte was folded exactly once.
 * @retval k_ra8_err_invalid_size An external read was short.
 * @pre @p ctx and @p crc are non-NULL.
 * @pre External mode supplies a non-NULL reader and nonzero scratch capacity.
 * @post Success updates @p crc through the whole logical pool.
 * @post Failure leaves the partially updated accumulator unpublished.
 * @note External reads are bounded by @p scratch_cap.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_crc_image_pool(const ra8_rabook_ctx_t*  ctx,
                                         ra8_rabook_image_read_fn read,
                                         void*                    read_ctx,
                                         uint8_t*                 scratch,
                                         uint32_t                 scratch_cap,
                                         uint32_t*                crc)
{
  if (ctx->image_pool_size == 0U) {
    return k_ra8_ok;
  }
  if (ctx->image_pool_mode != (uint8_t)k_rabook_pool_external) {
    *crc = internal_crc_update(*crc, ctx->buf.image_pool, ctx->image_pool_size);
    return k_ra8_ok;
  }
  uint32_t offset = 0U;
  while (offset < ctx->image_pool_size) {
    uint32_t span = ctx->image_pool_size - offset;
    if (span > scratch_cap) {
      span = scratch_cap;
    }
    const ra8_err_t err = internal_read_exact(read, read_ctx, offset, scratch, span);
    if (err != k_ra8_ok) {
      return err;
    }
    *crc = internal_crc_update(*crc, scratch, span);
    offset += span;
  }
  return k_ra8_ok;
}

/**
 * @brief Fill the fixed RABOOK1 header from one layout and CRC.
 * @details Copies builder counts, metadata, flags, pool sizes, canonical offsets,
 *          magic, version, and finalized body CRC into the pinned wire struct.
 * @param[in] ctx Builder state to serialize.
 * @param[in] lay Canonical layout computed from @p ctx.
 * @param[in] crc Finalized body CRC-32.
 * @return Complete RABOOK1 header value.
 * @retval book_header_t Header ready for byte emission.
 * @pre @p ctx and @p lay are non-NULL.
 * @pre @p lay was computed successfully from @p ctx.
 * @post Every header count and offset matches the builder and layout.
 * @post The returned header owns no pointers into builder storage.
 * @note Pure construction over caller-owned state.
 * @since 0.1.0
 */
RA8_INTERNAL
static book_header_t
internal_make_header(const ra8_rabook_ctx_t* ctx, const ra8_rabook_layout_t* lay, uint32_t crc)
{
  book_header_t hdr = {};
  (void)memcpy(hdr.magic, "RABOOK1", sizeof(hdr.magic));
  hdr.format_version    = (uint32_t)k_book_format_version;
  hdr.total_size        = lay->total;
  hdr.flags             = ctx->flags;
  hdr.title_off         = ctx->title_off;
  hdr.author_off        = ctx->author_off;
  hdr.language_off      = ctx->language_off;
  hdr.identifier_off    = ctx->identifier_off;
  hdr.cover_image_index = ctx->cover_image_index;
  hdr.chapter_count     = ctx->chapter_count;
  hdr.chapter_off       = lay->off_chap;
  hdr.node_count        = ctx->node_count;
  hdr.node_off          = lay->off_node;
  hdr.attr_count        = ctx->attr_count;
  hdr.attr_off          = lay->off_attr;
  hdr.stylesheet_count  = ctx->stylesheet_count;
  hdr.stylesheet_off    = lay->off_style;
  hdr.image_count       = ctx->image_count;
  hdr.image_off         = lay->off_image;
  hdr.string_off        = lay->off_string;
  hdr.string_size       = ctx->string_size;
  hdr.image_pool_off    = lay->off_pool;
  hdr.image_pool_size   = ctx->image_pool_size;
  hdr.crc32_val         = crc;
  return hdr;
}

/**
 * @brief Emit the logical image pool through the exact-write callback.
 * @details Writes an internal pool directly or rereads an external pool in
 *          caller-scratch-sized chunks and appends each chunk exactly once.
 * @param[in] ctx Builder describing the logical pool.
 * @param[in] read External reader, when selected.
 * @param[in,out] read_ctx External reader context.
 * @param[in] write Destination writer.
 * @param[in,out] write_ctx Destination writer context.
 * @param[in,out] scratch External transfer scratch.
 * @param[in] scratch_cap Scratch capacity in bytes.
 * @return Pool emission status.
 * @retval k_ra8_ok The entire pool was appended.
 * @retval k_ra8_err_invalid_size A read or write callback was short.
 * @pre @p ctx and @p write are non-NULL.
 * @pre External mode supplies a reader and nonzero scratch capacity.
 * @post Success appends exactly `ctx->image_pool_size` bytes.
 * @post Callback failures are propagated unchanged.
 * @note External reads never exceed @p scratch_cap.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_write_image_pool(const ra8_rabook_ctx_t*  ctx,
                                           ra8_rabook_image_read_fn read,
                                           void*                    read_ctx,
                                           ra8_rabook_write_fn      write,
                                           void*                    write_ctx,
                                           uint8_t*                 scratch,
                                           uint32_t                 scratch_cap)
{
  if (ctx->image_pool_size == 0U) {
    return k_ra8_ok;
  }
  if (ctx->image_pool_mode != (uint8_t)k_rabook_pool_external) {
    return internal_write_exact(write, write_ctx, ctx->buf.image_pool, ctx->image_pool_size);
  }
  uint32_t offset = 0U;
  while (offset < ctx->image_pool_size) {
    uint32_t span = ctx->image_pool_size - offset;
    if (span > scratch_cap) {
      span = scratch_cap;
    }
    ra8_err_t err = internal_read_exact(read, read_ctx, offset, scratch, span);
    if (err == k_ra8_ok) {
      err = internal_write_exact(write, write_ctx, scratch, span);
    }
    if (err != k_ra8_ok) {
      return err;
    }
    offset += span;
  }
  return k_ra8_ok;
}

/**
 * @brief Compute the finalized CRC for all canonical body segments.
 * @details Folds resident segments in layout order, then reads and folds the
 *          logical image pool before applying the CRC final XOR.
 * @param[in] ctx Builder describing the logical body.
 * @param[in] seg Canonical resident segment sequence.
 * @param[in] read External image-pool reader, when selected.
 * @param[in,out] read_ctx External reader context.
 * @param[in,out] scratch External transfer scratch.
 * @param[in] scratch_cap Scratch capacity in bytes.
 * @param[out] out_crc Finalized body CRC.
 * @return CRC pass status.
 * @retval k_ra8_ok Every logical body byte was folded.
 * @retval k_ra8_err_invalid_size An external read was short.
 * @pre @p ctx, @p seg, and @p out_crc are non-NULL.
 * @pre External mode supplies a reader and nonzero scratch capacity.
 * @post Success initializes @p out_crc with the RABOOK1 body CRC.
 * @post Callback failures are propagated unchanged.
 * @note External reads never exceed @p scratch_cap.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_crc_body(const ra8_rabook_ctx_t*    ctx,
                                   const ra8_rabook_segment_t seg[k_rabook_segment_count],
                                   ra8_rabook_image_read_fn   read,
                                   void*                      read_ctx,
                                   uint8_t*                   scratch,
                                   uint32_t                   scratch_cap,
                                   uint32_t*                  out_crc)
{
  uint32_t crc = (uint32_t)k_rabook_crc_init;
  for (uint8_t i = 0U; i < (uint8_t)k_rabook_segment_count; ++i) {
    crc = internal_crc_update(crc, seg[i].data, seg[i].len);
  }
  const ra8_err_t err = internal_crc_image_pool(ctx, read, read_ctx, scratch, scratch_cap, &crc);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_crc = crc ^ (uint32_t)k_rabook_crc_init;
  return k_ra8_ok;
}

/**
 * @brief Emit the fixed header followed by the canonical logical body.
 * @details Appends each resident segment exactly, then emits the internal or
 *          externally reread image pool.
 * @param[in] ctx Builder describing the logical image pool.
 * @param[in] hdr Finalized RABOOK1 header.
 * @param[in] seg Canonical resident segment sequence.
 * @param[in] read External image-pool reader, when selected.
 * @param[in,out] read_ctx External reader context.
 * @param[in] write Destination writer.
 * @param[in,out] write_ctx Destination writer context.
 * @param[in,out] scratch External transfer scratch.
 * @param[in] scratch_cap Scratch capacity in bytes.
 * @return Emission status.
 * @retval k_ra8_ok Header and complete body were appended.
 * @retval k_ra8_err_invalid_size A callback reported a short success.
 * @pre @p ctx, @p hdr, @p seg, and @p write are non-NULL.
 * @pre @p hdr describes the supplied segment sequence and image pool.
 * @post Success appends exactly the header total-size byte count.
 * @post Callback failures are propagated unchanged.
 * @note External image bytes are reread after the CRC pass.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_write_book(const ra8_rabook_ctx_t*    ctx,
                                     const book_header_t*       hdr,
                                     const ra8_rabook_segment_t seg[k_rabook_segment_count],
                                     ra8_rabook_image_read_fn   read,
                                     void*                      read_ctx,
                                     ra8_rabook_write_fn        write,
                                     void*                      write_ctx,
                                     uint8_t*                   scratch,
                                     uint32_t                   scratch_cap)
{
  ra8_err_t err =
    internal_write_exact(write, write_ctx, (const uint8_t*)hdr, (uint32_t)sizeof(*hdr));
  for (uint8_t i = 0U; i < (uint8_t)k_rabook_segment_count; ++i) {
    if (err != k_ra8_ok) {
      break;
    }
    err = internal_write_exact(write, write_ctx, seg[i].data, seg[i].len);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_write_image_pool(ctx, read, read_ctx, write, write_ctx, scratch, scratch_cap);
}

/**
 * @brief Validate streaming-finalizer arguments and builder state.
 * @details Enforces common pointer requirements, sticky builder failure, and
 *          the reader/scratch contract selected by a non-empty external pool.
 * @param[in] ctx Builder to finalize.
 * @param[in] image_read External image-pool reader, when selected.
 * @param[in] write Destination writer.
 * @param[in] scratch External transfer scratch pointer.
 * @param[in] scratch_cap Scratch capacity in bytes.
 * @param[out] out_len Final serialized length destination.
 * @return Validation status.
 * @retval k_ra8_ok Arguments satisfy the selected pool mode.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_size External mode has zero scratch capacity.
 * @retval k_ra8_err_no_mem Builder failure was already latched.
 * @pre Pointer values may be NULL for validation.
 * @pre @p ctx, when non-NULL, addresses an initialized builder.
 * @post Success permits layout, CRC, and emission passes.
 * @post No builder or output state is modified.
 * @note Internal-pool mode ignores @p image_read and scratch arguments.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_stream_args(const ra8_rabook_ctx_t*  ctx,
                                               ra8_rabook_image_read_fn image_read,
                                               ra8_rabook_write_fn      write,
                                               const uint8_t*           scratch,
                                               uint32_t                 scratch_cap,
                                               const uint32_t*          out_len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  if (write == nullptr) {
    ra8_log_error(s_tag_rabook, "write is NULL");
    return k_ra8_err_null_ptr;
  }
  RA8_CHECK_NULL_PTR(out_len, s_tag_rabook, "out_len");
  if (ctx->failed) {
    return k_ra8_err_no_mem;
  }
  bool external = false;
  if (ctx->image_pool_size != 0U) {
    external = ctx->image_pool_mode == (uint8_t)k_rabook_pool_external;
  }
  if (external) {
    if (image_read == nullptr) {
      return k_ra8_err_null_ptr;
    }
    if (scratch == nullptr) {
      return k_ra8_err_null_ptr;
    }
    if (scratch_cap == 0U) {
      return k_ra8_err_invalid_size;
    }
  }
  return k_ra8_ok;
}

ra8_err_t ra8_rabook_finalize_stream(const ra8_rabook_ctx_t*  ctx,
                                     ra8_rabook_image_read_fn image_read,
                                     void*                    image_ctx,
                                     ra8_rabook_write_fn      write,
                                     void*                    write_ctx,
                                     uint8_t*                 scratch,
                                     uint32_t                 scratch_cap,
                                     uint32_t*                out_len)
{
  const ra8_err_t arg_err =
    internal_validate_stream_args(ctx, image_read, write, scratch, scratch_cap, out_len);
  if (arg_err != k_ra8_ok) {
    return arg_err;
  }

  ra8_rabook_layout_t lay     = {};
  const ra8_err_t     lay_err = internal_compute_layout(ctx, &lay);
  if (lay_err != k_ra8_ok) {
    return lay_err;
  }
  ra8_rabook_segment_t seg[k_rabook_segment_count] = {};
  internal_segments(ctx, &lay, seg);
  uint32_t  crc = 0U;
  ra8_err_t err = internal_crc_body(ctx, seg, image_read, image_ctx, scratch, scratch_cap, &crc);
  if (err != k_ra8_ok) {
    return err;
  }
  const book_header_t hdr = internal_make_header(ctx, &lay, crc);
  err                     = internal_write_book(ctx,
                                                &hdr,
                                                seg,
                                                image_read,
                                                image_ctx,
                                                write,
                                                write_ctx,
                                                scratch,
                                                scratch_cap);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_len = lay.total;
  return k_ra8_ok;
}

/** @brief Bounded in-memory sink used by the legacy finalizer wrapper. */
typedef struct {
  uint8_t* data; /**< Output arena base.  */
  uint32_t cap;  /**< Output arena bytes. */
  uint32_t used; /**< Bytes appended.     */
} ra8_rabook_memory_sink_t;

/**
 * @brief Append one chunk to a bounded memory sink.
 * @details Checks remaining capacity before copying and reports zero written on
 *          exhaustion, preserving the exact-write callback contract.
 * @param[in,out] opaque Memory-sink context.
 * @param[in] src Source bytes.
 * @param[in] requested Requested append length.
 * @param[out] out_written Actual appended length.
 * @return Sink status.
 * @retval k_ra8_ok The full request was copied.
 * @retval k_ra8_err_invalid_size The destination lacks capacity.
 * @pre Context, source, and count pointers are non-NULL.
 * @pre The sink's used count does not exceed its capacity.
 * @post Success advances used and reports @p requested.
 * @post Exhaustion reports zero and leaves the sink unchanged.
 * @note Used only by the legacy in-memory finalizer wrapper.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_memory_write(void* opaque, const uint8_t* src, uint32_t requested, uint32_t* out_written)
{
  ra8_rabook_memory_sink_t* sink = (ra8_rabook_memory_sink_t*)opaque;
  *out_written                   = 0U;
  if (requested > (sink->cap - sink->used)) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(&sink->data[sink->used], src, (size_t)requested);
  sink->used += requested;
  *out_written = requested;
  return k_ra8_ok;
}

/**
 * @brief Validate the legacy in-memory finalizer and output capacity.
 * @details Checks result pointers and sticky builder failure, then computes the
 *          canonical layout without publishing bytes to prove the sink fits.
 * @param[in] ctx Builder to finalize.
 * @param[out] out_blob Final output pointer destination.
 * @param[out] out_len Final serialized length destination.
 * @return Validation status.
 * @retval k_ra8_ok Builder layout fits the configured output arena.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_no_mem Builder failure was already latched.
 * @retval k_ra8_err_invalid_size Layout overflows or exceeds output capacity.
 * @pre Pointer values may be NULL for validation.
 * @pre @p ctx, when non-NULL, addresses an initialized builder.
 * @post Success permits the streaming wrapper to use the configured sink.
 * @post No builder, output bytes, or result pointers are modified.
 * @note The streaming finalizer recomputes the same layout before emission.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_memory_finalize(ra8_rabook_ctx_t* ctx,
                                                   const void**      out_blob,
                                                   const uint32_t*   out_len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  RA8_CHECK_NULL_PTR(out_blob, s_tag_rabook, "out_blob");
  RA8_CHECK_NULL_PTR(out_len, s_tag_rabook, "out_len");
  if (ctx->failed) {
    ra8_log_error(s_tag_rabook, "finalize after a builder overflow");
    return k_ra8_err_no_mem;
  }
  ra8_rabook_layout_t lay = {};
  RA8_RETURN_ON_ERROR(internal_compute_layout(ctx, &lay), s_tag_rabook, "layout");
  return (lay.total <= ctx->buf.out_cap) ? k_ra8_ok : k_ra8_err_invalid_size;
}

ra8_err_t ra8_rabook_finalize(ra8_rabook_ctx_t* ctx, const void** out_blob, uint32_t* out_len)
{
  const ra8_err_t arg_err = internal_validate_memory_finalize(ctx, out_blob, out_len);
  if (arg_err != k_ra8_ok) {
    return arg_err;
  }
  ra8_rabook_memory_sink_t sink = {.data = ctx->buf.out, .cap = ctx->buf.out_cap};
  uint32_t                 len  = 0U;
  const ra8_err_t          err  = ra8_rabook_finalize_stream(ctx,
                                                             nullptr,
                                                             nullptr,
                                                             internal_memory_write,
                                                             &sink,
                                                             nullptr,
                                                             0U,
                                                             &len);
  if (err != k_ra8_ok) {
    return err;
  }

  *out_blob = ctx->buf.out;
  *out_len  = len;
  return k_ra8_ok;
}
