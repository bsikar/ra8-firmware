/**
 * @file ra8_rabook_comic.c
 * @brief Streaming image-sequence builder for reader-native RABOOK comics.
 * @details Normalizes one encoded page at a time, stores its gray payload in a
 * caller spool, constructs a minimal `body/img` DOM chapter, and streams the
 * canonical flat RABOOK1 layout without resident image or output pools.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_rabook_comic.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_book.h"

/**
 * @brief Return whether one bounded string has a NUL terminator.
 * @details Scans at most the public comic string limit so untrusted metadata
 *          cannot cause an unbounded read before interning.
 * @param[in] text Candidate string.
 * @return Whether a terminator occurs within the comic string capacity.
 * @retval true The complete bounded string is safe for builder interning.
 * @retval false The pointer is NULL or the bounded window lacks a terminator.
 * @pre Non-NULL @p text spans ::k_ra8_rabook_comic_string_max readable bytes.
 * @pre The string remains immutable for this call.
 * @post No input byte or builder state is modified.
 * @post At most the fixed bounded window is inspected.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_string_is_bounded(const char* text)
{
  if (text == nullptr) {
    return false;
  }
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_rabook_comic_string_max; ++i) {
    if (text[i] == '\0') {
      return true;
    }
  }
  return false;
}

/**
 * @brief Intern one string or classify the builder failure.
 * @details Translates the builder's nil-offset sentinel into the public
 *          no-memory status while preserving valid offset zero for empty text.
 * @param[in,out] builder Active RABOOK table builder.
 * @param[in] text Bounded NUL-terminated string.
 * @param[out] out_off Resulting string-pool offset.
 * @return Intern status.
 * @retval k_ra8_ok The string was interned or deduplicated.
 * @retval k_ra8_err_no_mem The string pool could not hold the value.
 * @pre Pointers are non-NULL and @p text passed the bounded-string check.
 * @pre @p builder was initialized and remains active.
 * @post Success initializes @p out_off to a non-nil offset or zero for empty.
 * @post Failure leaves the builder's sticky failure latched.
 * @note Not thread-safe through @p builder.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_intern(ra8_rabook_ctx_t* builder, const char* text, uint32_t* out_off)
{
  const uint32_t off = ra8_rabook_intern(builder, text);
  if (off == (uint32_t)k_ra8_book_nil) {
    return k_ra8_err_no_mem;
  }
  *out_off = off;
  return k_ra8_ok;
}

/**
 * @brief Append the minimal body/img DOM and one chapter row.
 * @details Interns the required XML names, creates a body with one img child,
 *          binds its src to the page identifier, and publishes the matching
 *          chapter only after every preceding builder step succeeds.
 * @param[in,out] comic Active comic build.
 * @param[in] page_id_off Interned page identifier and image href.
 * @return Builder/link status.
 * @retval k_ra8_ok One chapter rooted at a body/img tree was appended.
 * @retval k_ra8_err_no_mem A string, node, attribute, or chapter arena filled.
 * @return Link validation failures propagate unchanged.
 * @pre The page image descriptor was appended successfully.
 * @pre @p page_id_off addresses the image id in the builder string pool.
 * @post Success adds exactly two nodes, one attribute, and one chapter.
 * @post Failure leaves the builder sticky failure latched when capacity caused it.
 * @note Not thread-safe through @p comic.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_append_chapter(ra8_rabook_comic_t* comic,
                                                      uint32_t            page_id_off)
{
  uint32_t  body_off = 0U;
  uint32_t  img_off  = 0U;
  uint32_t  src_off  = 0U;
  ra8_err_t result   = internal_intern(&comic->builder, "body", &body_off);
  if (result == k_ra8_ok) {
    result = internal_intern(&comic->builder, "img", &img_off);
  }
  if (result == k_ra8_ok) {
    result = internal_intern(&comic->builder, "src", &src_off);
  }
  const ra8_book_attr_t source = {.name_off = src_off, .value_off = page_id_off};
  const uint32_t        body   = (result == k_ra8_ok)
                                   ? ra8_rabook_add_element(&comic->builder, body_off, nullptr, 0U)
                                   : (uint32_t)k_ra8_book_nil;
  const uint32_t        image  = (body != (uint32_t)k_ra8_book_nil)
                                   ? ra8_rabook_add_element(&comic->builder, img_off, &source, 1U)
                                   : (uint32_t)k_ra8_book_nil;
  if ((body == (uint32_t)k_ra8_book_nil) || (image == (uint32_t)k_ra8_book_nil)) {
    return (result == k_ra8_ok) ? k_ra8_err_no_mem : result;
  }
  result = ra8_rabook_link_child(&comic->builder, body, image);
  if (result != k_ra8_ok) {
    return result;
  }
  if (ra8_rabook_add_chapter(&comic->builder, page_id_off, page_id_off, body) ==
      (uint32_t)k_ra8_book_nil) {
    return k_ra8_err_no_mem;
  }
  return k_ra8_ok;
}

/**
 * @brief Store one normalized raster and append its image descriptor.
 * @details Writes at the builder's current logical image-pool end, requires an
 *          exact write, then records an external descriptor whose returned
 *          offset must agree with the write offset.
 * @param[in,out] comic Active comic build.
 * @param[in] page_id_off Interned image identifier.
 * @param[in] raster Normalized raster geometry and extent.
 * @return Spool and builder status.
 * @retval k_ra8_ok Exact bytes were spooled and one descriptor was appended.
 * @retval k_ra8_err_invalid_size The spool callback reported a short write.
 * @retval k_ra8_err_no_mem The descriptor or logical image pool overflowed.
 * @return Backend failures propagate unchanged.
 * @pre @p comic->normalized holds `raster->encoded_size` readable bytes.
 * @pre The image spool is private and supports this logical offset.
 * @post Success advances the logical image pool by exactly the encoded size.
 * @post Failure must be followed by discarding the entire private spool.
 * @note Not thread-safe through @p comic or its spool.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_store_image(ra8_rabook_comic_t*               comic,
                                                   uint32_t                          page_id_off,
                                                   const ra8_rabook_raster_result_t* raster)
{
  const uint32_t offset  = comic->builder.image_pool_size;
  uint32_t       written = 0U;
  ra8_err_t      result =
    comic->image_write(comic->image_ctx, offset, comic->normalized, raster->encoded_size, &written);
  if (result != k_ra8_ok) {
    return result;
  }
  if (written != raster->encoded_size) {
    return k_ra8_err_invalid_size;
  }
  uint32_t       recorded_offset = 0U;
  const uint32_t image           = ra8_rabook_add_image_external(&comic->builder,
                                                                 page_id_off,
                                                                 raster->width,
                                                                 raster->height,
                                                                 (uint8_t)k_ra8_book_image_gray4,
                                                                 raster->pixel_format,
                                                                 raster->encoded_size,
                                                                 &recorded_offset);
  if (image == (uint32_t)k_ra8_book_nil) {
    return k_ra8_err_no_mem;
  }
  if (recorded_offset != offset) {
    comic->builder.failed = true;
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate and intern all final metadata fields.
 * @details Bounded-validates the four public strings before mutating the
 *          header, interns them in declared order, and selects the first page
 *          as the cover image.
 * @param[in,out] comic Active populated comic build.
 * @param[in] metadata Caller metadata strings.
 * @return Metadata validation and builder status.
 * @retval k_ra8_ok Header offsets and first-page cover were recorded.
 * @retval k_ra8_err_null_ptr @p metadata is NULL.
 * @retval k_ra8_err_invalid_size A string lacks a bounded terminator.
 * @retval k_ra8_err_no_mem The string arena filled.
 * @pre The comic contains at least one image descriptor.
 * @pre Metadata strings remain immutable for the call.
 * @post Success binds every header string and cover image index zero.
 * @post Failure does not invoke the streaming finalizer.
 * @note Not thread-safe through @p comic.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_set_metadata(ra8_rabook_comic_t*                comic,
                                                    const ra8_rabook_comic_metadata_t* metadata)
{
  if (metadata == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!internal_string_is_bounded(metadata->title) ||
      !internal_string_is_bounded(metadata->author) ||
      !internal_string_is_bounded(metadata->language) ||
      !internal_string_is_bounded(metadata->identifier)) {
    return k_ra8_err_invalid_size;
  }
  uint32_t          fields[] = {0U, 0U, 0U, 0U};
  const char* const values[] = {metadata->title,
                                metadata->author,
                                metadata->language,
                                metadata->identifier};
  for (size_t i = 0U; i < (sizeof values / sizeof values[0]); ++i) {
    const ra8_err_t result = internal_intern(&comic->builder, values[i], &fields[i]);
    if (result != k_ra8_ok) {
      return result;
    }
  }
  return ra8_rabook_set_metadata(&comic->builder, fields[0], fields[1], fields[2], fields[3], 0U);
}

ra8_err_t ra8_rabook_comic_init(ra8_rabook_comic_t*                 comic,
                                const ra8_rabook_comic_workspace_t* workspace,
                                ra8_rabook_image_read_fn            image_read,
                                ra8_rabook_comic_write_at_fn        image_write,
                                void*                               image_ctx,
                                uint16_t                            max_image_edge,
                                uint8_t                             pixel_format)
{
  if (comic == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *comic = (ra8_rabook_comic_t){};
  if ((workspace == nullptr) || (image_read == nullptr) || (image_write == nullptr) ||
      (image_ctx == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((workspace->normalized == nullptr) || (workspace->stream_scratch == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((workspace->normalized_cap == 0U) || (workspace->stream_scratch_cap == 0U)) {
    return k_ra8_err_invalid_size;
  }
  if ((pixel_format != (uint8_t)k_ra8_book_pixfmt_gray4) &&
      (pixel_format != (uint8_t)k_ra8_book_pixfmt_gray8)) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t result = ra8_rabook_compile_init(&comic->builder, &workspace->builder);
  if (result == k_ra8_ok) {
    comic->raster             = workspace->raster;
    comic->image_read         = image_read;
    comic->image_write        = image_write;
    comic->image_ctx          = image_ctx;
    comic->normalized         = workspace->normalized;
    comic->normalized_cap     = workspace->normalized_cap;
    comic->stream_scratch     = workspace->stream_scratch;
    comic->stream_scratch_cap = workspace->stream_scratch_cap;
    comic->max_image_edge     = max_image_edge;
    comic->pixel_format       = pixel_format;
    comic->active             = true;
  }
  return result;
}

ra8_err_t ra8_rabook_comic_add_page(ra8_rabook_comic_t* comic,
                                    const char*         page_id,
                                    const uint8_t*      source,
                                    size_t              source_size)
{
  if (comic == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!comic->active) {
    return k_ra8_err_invalid_state;
  }
  if ((page_id == nullptr) || (source == nullptr) || (source_size == 0U)) {
    comic->active = false;
    return k_ra8_err_invalid_arg;
  }
  if (!internal_string_is_bounded(page_id)) {
    comic->active = false;
    return k_ra8_err_invalid_size;
  }
  ra8_rabook_raster_result_t raster      = {};
  ra8_err_t                  result      = ra8_rabook_raster_encode(source,
                                                                    source_size,
                                                                    comic->max_image_edge,
                                                                    comic->pixel_format,
                                                                    &comic->raster,
                                                                    comic->normalized,
                                                                    comic->normalized_cap,
                                                                    &raster);
  uint32_t                   page_id_off = 0U;
  if (result == k_ra8_ok) {
    result = internal_intern(&comic->builder, page_id, &page_id_off);
  }
  if (result == k_ra8_ok) {
    result = internal_store_image(comic, page_id_off, &raster);
  }
  if (result == k_ra8_ok) {
    result = internal_append_chapter(comic, page_id_off);
  }
  if (result == k_ra8_ok) {
    comic->page_count++;
  } else {
    comic->active = false;
  }
  return result;
}

ra8_err_t ra8_rabook_comic_finish(ra8_rabook_comic_t*                comic,
                                  const ra8_rabook_comic_metadata_t* metadata,
                                  ra8_rabook_write_fn                write,
                                  void*                              write_ctx,
                                  uint32_t*                          out_len)
{
  if (comic == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!comic->active) {
    return k_ra8_err_invalid_state;
  }
  comic->active = false;
  if ((write == nullptr) || (write_ctx == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (comic->page_count == 0U) {
    return k_ra8_err_empty;
  }
  ra8_err_t result = internal_set_metadata(comic, metadata);
  if (result == k_ra8_ok) {
    result = ra8_rabook_finalize_stream(&comic->builder,
                                        comic->image_read,
                                        comic->image_ctx,
                                        write,
                                        write_ctx,
                                        comic->stream_scratch,
                                        comic->stream_scratch_cap,
                                        out_len);
  }
  return result;
}
