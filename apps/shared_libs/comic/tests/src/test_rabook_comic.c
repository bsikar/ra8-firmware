/**
 * @file test_rabook_comic.c
 * @brief End-to-end tests for streamed comic-page RABOOK production.
 * @details Converts real BMP pages through the production raster path, streams
 * a canonical flat book, wraps it in RBKC, and strictly consumes every chunk.
 * Fault cases prove short spool writes fail closed and poison the private build.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "book.h"
#include "book_chunked.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_compress.h"
#include "ra8_err.h"
#include "ra8_rabook_comic.h"
#include "ra8_rabook_container.h"
#include "unity_minimal.h"

/** @brief Fixed page, builder, codec, and container fixture capacities. */
typedef enum : uint32_t {
  k_comic_pages            = 2U,            /**< Real pages in the success book. */
  k_comic_chapters         = 3U,            /**< Builder chapter capacity.       */
  k_comic_nodes            = 6U,            /**< Two DOM nodes per page.         */
  k_comic_attrs            = 3U,            /**< One src attribute per page.     */
  k_comic_images           = 3U,            /**< Builder image capacity.         */
  k_comic_strings          = 2048U,         /**< Interned string capacity.       */
  k_comic_codec_bytes      = 1024U * 1024U, /**< One MiB per decoder arena.      */
  k_comic_pixel_bytes      = 256U,          /**< WebP frame scratch.             */
  k_comic_gray_bytes       = 64U,           /**< Downscale scratch.              */
  k_comic_normalized_bytes = 64U,           /**< One normalized page.            */
  k_comic_image_spool      = 256U,          /**< All normalized pages.           */
  k_comic_flat_bytes       = 4096U,         /**< Complete flat RABOOK1.          */
  k_comic_packed_bytes     = 8192U,         /**< Complete RBKC artifact.         */
  k_comic_chunk_bytes      = 256U,          /**< Independent RBKC chunk size.    */
  k_comic_deflate_bytes    = 512U,          /**< Worst-case fixture zlib stream. */
  k_comic_offsets          = 32U,           /**< RBKC offset-table entries.      */
  k_comic_validate_bytes   = 512U,          /**< Strict validator scratch.       */
} comic_fixture_limit_t;

/** @brief Builder capacities a fault fixture deliberately constrains. */
typedef enum : uint32_t {
  k_comic_cap_none      = 0U,  /**< Table that cannot hold one entry.            */
  k_comic_strings_min   = 1U,  /**< Pool holding only the empty-string sentinel. */
  k_comic_strings_tight = 32U, /**< Pool that fills while interning metadata.    */
} comic_fault_cap_t;

/** @brief Argument and postcondition constants used by the fault fixtures. */
typedef enum : uint32_t {
  k_comic_edge_unclamped = 0U, /**< Longer-edge clamp disabled.        */
  k_comic_no_pages       = 0U, /**< Pages retained after a fault.      */
  k_comic_no_bytes       = 0U, /**< Byte extent produced by a fault.   */
  k_comic_one_page       = 1U, /**< Pages in a one-page fault fixture. */
} comic_fault_expect_t;

/** @brief One bounded memory source/destination used by all callbacks. */
typedef struct {
  uint8_t* bytes;       /**< Mutable backing bytes.              */
  uint32_t capacity;    /**< Accessible backing extent.          */
  uint32_t length;      /**< Highest initialized byte plus one.  */
  bool     short_write; /**< Inject a successful one-byte short. */
} comic_memory_t;

/** @brief Builder arena capacities for one deliberately constrained build. */
typedef struct {
  uint32_t chapter_cap; /**< Chapter-table entry capacity. */
  uint32_t image_cap;   /**< Image-table entry capacity.   */
  uint32_t string_cap;  /**< String-pool byte capacity.    */
} comic_caps_t;

/** @brief Complete caller state retained through one constrained comic build. */
typedef struct {
  ra8_rabook_comic_t           comic;     /**< Production facade state.  */
  ra8_rabook_comic_workspace_t workspace; /**< Bound caller workspace.   */
  ra8_img_arena_t              stb;       /**< stb decoder descriptor.   */
  ra8_webp_arena_t             webp;      /**< WebP decoder descriptor.  */
  comic_memory_t               image;     /**< Normalized image spool.   */
  comic_memory_t               flat;      /**< Flat RABOOK1 destination. */
} comic_fault_fixture_t;

/** @brief Compressor scratch with portable alignment. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[k_ra8_compress_scratch_bytes]; /**< Codec workspace. */
} comic_compressor_t;

/** @brief 2x2 24-bit BMP with four distinct source colors. */
static const uint8_t s_bmp[] = {
  0x42, 0x4D, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
  0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00,
  0x13, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00,
  0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00,
};

static uint8_t s_image_bytes[k_comic_image_spool];
static uint8_t s_flat_bytes[k_comic_flat_bytes];
static uint8_t s_packed_bytes[k_comic_packed_bytes];

/**
 * @brief Write one bounded span into a memory object.
 * @details Models random-access storage with exact capacity accounting and an
 *          injectable one-byte short-write response for transaction faults.
 * @param[in,out] memory Destination object.
 * @param[in] offset Destination byte offset.
 * @param[in] source Source bytes.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_written Actual accepted bytes.
 * @return Memory-backend status.
 * @retval k_ra8_ok The full span, or injected short span, was written.
 * @retval k_ra8_err_invalid_size The requested range exceeds capacity.
 * @pre Pointers are non-NULL and source spans requested bytes.
 * @pre Offset and requested are represented as uint64 for overflow checking.
 * @post Success advances length to the highest initialized byte.
 * @post Capacity failure writes no bytes and reports zero progress.
 * @note Test-only and not thread-safe through @p memory.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_memory_write(comic_memory_t* memory,
                                                    uint64_t        offset,
                                                    const uint8_t*  source,
                                                    uint32_t        requested,
                                                    uint32_t*       out_written)
{
  *out_written = 0U;
  if ((offset > memory->capacity) ||
      ((uint64_t)requested > ((uint64_t)memory->capacity - offset))) {
    return k_ra8_err_invalid_size;
  }
  uint32_t accepted = requested;
  if (memory->short_write && (accepted != 0U)) {
    accepted--;
  }
  (void)memcpy(&memory->bytes[offset], source, accepted);
  const uint32_t end = (uint32_t)offset + accepted;
  if (end > memory->length) {
    memory->length = end;
  }
  *out_written = accepted;
  return k_ra8_ok;
}

/**
 * @brief Adapt the memory writer to the comic image-spool callback.
 * @details Narrows the comic's uint32 offset contract into the common bounded
 *          memory writer used by the fixture.
 * @param[in,out] ctx Bound ::comic_memory_t.
 * @param[in] offset Logical image offset.
 * @param[in] source Normalized image bytes.
 * @param[in] requested Exact requested bytes.
 * @param[out] out_written Actual accepted bytes.
 * @return Memory writer status.
 * @retval k_ra8_ok The complete page span was stored.
 * @retval k_ra8_err_invalid_size The destination was too small or injected a
 *                                successful short write.
 * @pre Callback arguments satisfy ::ra8_rabook_comic_write_at_fn.
 * @pre @p ctx remains exclusively mutable.
 * @post Output and memory state match ::internal_memory_write.
 * @post No allocation or hidden fixture state is used.
 * @note Test-only callback.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_image_write(void*          ctx,
                                                   uint32_t       offset,
                                                   const uint8_t* source,
                                                   uint32_t       requested,
                                                   uint32_t*      out_written)
{
  return internal_memory_write(ctx, offset, source, requested, out_written);
}

/** @brief Fail every image-spool write with a distinctive backend status. @details Models a storage backend that rejects the transfer outright rather than accepting a short span, so the facade's propagation of a callback error is observable separately from its short-write guard. @param[in,out] ctx Ignored spool context. @param[in] offset Ignored logical image-pool offset. @param[in] source Ignored normalized image bytes. @param[in] requested Ignored requested byte count. @param[out] out_written Set to zero because nothing was accepted. @return Fixed backend failure. @retval k_ra8_err_hw_error The scripted backend refused the write. @pre Callback arguments satisfy ::ra8_rabook_comic_write_at_fn. @pre @p out_written addresses writable storage. @post No destination byte is modified. @post Reported progress is exactly zero. @note Test-only callback; holds no state and is thread-safe. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_image_write_fault(void*          ctx,
                                                         uint32_t       offset,
                                                         const uint8_t* source,
                                                         uint32_t       requested,
                                                         uint32_t*      out_written)
{
  (void)ctx;
  (void)offset;
  (void)source;
  (void)requested;
  *out_written = 0U;
  return k_ra8_err_hw_error;
}

/**
 * @brief Read one exact span and report explicit progress.
 * @details Models the repeatable external-image reader used by the production
 *          streaming finalizer and rejects every out-of-range request.
 * @param[in] ctx Bound ::comic_memory_t source.
 * @param[in] offset Source byte offset.
 * @param[out] destination Writable destination bytes.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_read Actual copied bytes.
 * @return Source status.
 * @retval k_ra8_ok Exact bytes were copied.
 * @retval k_ra8_err_invalid_size The range exceeds initialized bytes.
 * @pre Pointer arguments are non-NULL.
 * @pre Source memory remains immutable during the call.
 * @post Success reports requested bytes; failure reports zero.
 * @post No source byte is modified.
 * @note Test-only callback.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_memory_read_progress(void*     ctx,
                                                            uint32_t  offset,
                                                            uint8_t*  destination,
                                                            uint32_t  requested,
                                                            uint32_t* out_read)
{
  const comic_memory_t* memory = ctx;
  *out_read                    = 0U;
  if ((offset > memory->length) || (requested > (memory->length - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(destination, &memory->bytes[offset], requested);
  *out_read = requested;
  return k_ra8_ok;
}

/**
 * @brief Append one flat-book span to a memory object.
 * @details Uses the destination's initialized length as the append offset so
 *          successive finalizer callbacks form one contiguous flat book.
 * @param[in,out] ctx Bound ::comic_memory_t destination.
 * @param[in] source Flat RABOOK1 bytes.
 * @param[in] requested Exact append byte count.
 * @param[out] out_written Actual accepted bytes.
 * @return Memory writer status.
 * @retval k_ra8_ok The complete span was appended.
 * @retval k_ra8_err_invalid_size The destination lacked capacity.
 * @pre Callback arguments satisfy ::ra8_rabook_write_fn.
 * @pre The current length is the required append offset.
 * @post Success extends the object by the reported count.
 * @post Failure preserves the previously initialized prefix.
 * @note Test-only callback.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_flat_append(void* ctx, const uint8_t* source, uint32_t requested, uint32_t* out_written)
{
  comic_memory_t* memory = ctx;
  return internal_memory_write(memory, memory->length, source, requested, out_written);
}

/**
 * @brief Adapt a memory source to the flat-container reader callback.
 * @details Delegates exact positioned reads to the same bounded reader used by
 *          the external image spool.
 * @param[in] ctx Bound ::comic_memory_t source.
 * @param[in] offset Flat-source byte offset.
 * @param[out] destination Writable destination.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_read Exact copied byte count.
 * @return Source status.
 * @retval k_ra8_ok The complete flat-book span was copied.
 * @retval k_ra8_err_invalid_size The request exceeded the flat object.
 * @pre Callback arguments satisfy ::ra8_rabook_flat_read_fn.
 * @pre The flat memory remains immutable.
 * @post Results match ::internal_memory_read_progress.
 * @post Source storage is not modified.
 * @note Test-only callback.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_flat_read(void*     ctx,
                                                 uint32_t  offset,
                                                 uint8_t*  destination,
                                                 uint32_t  requested,
                                                 uint32_t* out_read)
{
  return internal_memory_read_progress(ctx, offset, destination, requested, out_read);
}

/**
 * @brief Adapt the memory writer to the RBKC random-write callback.
 * @details Preserves the writer's 64-bit offset contract so the container may
 *          back-fill its chunk table after streaming compressed chunks.
 * @param[in,out] ctx Bound ::comic_memory_t destination.
 * @param[in] offset RBKC byte offset.
 * @param[in] source Container bytes.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_written Actual accepted bytes.
 * @return Memory writer status.
 * @retval k_ra8_ok The complete container span was stored.
 * @retval k_ra8_err_invalid_size The offset or requested extent was invalid.
 * @pre Callback arguments satisfy ::ra8_rabook_write_at_fn.
 * @pre The destination remains exclusively mutable.
 * @post Results match ::internal_memory_write.
 * @post Back-filled table writes preserve the final extent.
 * @note Test-only callback.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_packed_write(void*          ctx,
                                                    uint64_t       offset,
                                                    const uint8_t* source,
                                                    uint32_t       requested,
                                                    uint32_t*      out_written)
{
  return internal_memory_write(ctx, offset, source, requested, out_written);
}

/**
 * @brief Read exact RBKC bytes for the strict chunk reader.
 * @details Validates the 64-bit request against the initialized container
 *          extent before copying into the strict reader's caller scratch.
 * @param[in] ctx Bound ::comic_memory_t source.
 * @param[in] offset Container byte offset.
 * @param[out] destination Exact read destination.
 * @param[in] length Exact requested byte count.
 * @return Source status.
 * @retval k_ra8_ok Exact initialized bytes were copied.
 * @retval k_ra8_err_invalid_size The request is outside the object.
 * @pre Callback arguments satisfy ::ra8_vsource_read_fn.
 * @pre Source bytes remain immutable.
 * @post Success initializes the complete destination span.
 * @post No source or unrelated destination byte is modified.
 * @note Test-only callback.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_packed_read(void* ctx, uint64_t offset, uint8_t* destination, uint32_t length)
{
  const comic_memory_t* memory = ctx;
  if ((offset > memory->length) || ((uint64_t)length > ((uint64_t)memory->length - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(destination, &memory->bytes[offset], length);
  return k_ra8_ok;
}

/**
 * @brief Inflate one complete RFC 1950 stream through miniz.
 * @details Uses the allocation-free one-shot decoder with explicit zlib-header
 *          parsing and a non-wrapping caller destination.
 * @param[in] source Compressed stream bytes.
 * @param[in] source_size Compressed extent.
 * @param[out] destination Inflated destination.
 * @param[in] destination_cap Destination capacity.
 * @param[out] out_size Actual inflated byte count.
 * @return Inflate status.
 * @retval k_ra8_ok One complete stream inflated.
 * @retval k_ra8_err_validation_failed Miniz rejected the stream.
 * @pre Pointers are non-NULL and spans match their capacities.
 * @pre The stream carries an RFC 1950 header.
 * @post Success initializes @p out_size within destination capacity.
 * @post Failure does not claim an output size.
 * @note Allocation-free and thread-safe for distinct spans.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_inflate(const void* source,
                                               size_t      source_size,
                                               void*       destination,
                                               size_t      destination_cap,
                                               size_t*     out_size)
{
  const size_t result = tinfl_decompress_mem_to_mem(
    destination,
    destination_cap,
    source,
    source_size,
    (uint32_t)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  if (result == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
    return k_ra8_err_validation_failed;
  }
  *out_size = result;
  return k_ra8_ok;
}

/**
 * @brief Create a fresh comic workspace over static fixture storage.
 * @details Binds every builder, codec, normalized-page, and stream arena while
 *          intentionally leaving resident image/output pools as one-byte
 *          sentinels to prove the external streaming path.
 * @param[out] stb Caller descriptor for the stb arena.
 * @param[out] webp Caller descriptor for the WebP arena.
 * @return Complete comic workspace by value.
 * @retval ra8_rabook_comic_workspace_t Every fixed arena is bound.
 * @pre Static fixture storage is exclusively owned.
 * @pre Output descriptors are non-NULL.
 * @post Both codec arenas are empty and every capacity is exact.
 * @post Resident output/image arenas are one-byte dummies by design.
 * @note Not thread-safe because the fixture uses shared statics.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_rabook_comic_workspace_t internal_workspace(ra8_img_arena_t*  stb,
                                                                    ra8_webp_arena_t* webp)
{
  static book_chapter_t      s_chapters[k_comic_chapters];
  static book_node_t         s_nodes[k_comic_nodes];
  static book_attr_t         s_attrs[k_comic_attrs];
  static book_stylesheet_t   s_styles[1];
  static book_image_t        s_images[k_comic_images];
  static char                s_strings[k_comic_strings];
  static uint8_t             s_dummy_image_pool[1];
  static uint8_t             s_dummy_output[1];
  alignas(16) static uint8_t s_stb_arena[k_comic_codec_bytes];
  alignas(16) static uint8_t s_webp_arena[k_comic_codec_bytes];
  static uint8_t             s_rgba[k_comic_pixel_bytes];
  static uint8_t             s_gray[k_comic_gray_bytes];
  static uint8_t             s_normalized[k_comic_normalized_bytes];
  static uint8_t             s_stream_scratch[k_comic_normalized_bytes];
  *stb  = (ra8_img_arena_t){.base = s_stb_arena, .cap = sizeof s_stb_arena};
  *webp = (ra8_webp_arena_t){.base = s_webp_arena, .cap = sizeof s_webp_arena};
  const ra8_rabook_buffers_t          builder = {.chapters       = s_chapters,
                                                 .nodes          = s_nodes,
                                                 .attrs          = s_attrs,
                                                 .stylesheets    = s_styles,
                                                 .images         = s_images,
                                                 .string_pool    = s_strings,
                                                 .image_pool     = s_dummy_image_pool,
                                                 .out            = s_dummy_output,
                                                 .chapter_cap    = k_comic_chapters,
                                                 .node_cap       = k_comic_nodes,
                                                 .attr_cap       = k_comic_attrs,
                                                 .stylesheet_cap = 1U,
                                                 .image_cap      = k_comic_images,
                                                 .string_cap     = k_comic_strings,
                                                 .image_pool_cap = sizeof s_dummy_image_pool,
                                                 .out_cap        = sizeof s_dummy_output};
  const ra8_rabook_raster_workspace_t raster  = {.stb_arena  = stb,
                                                 .webp_arena = webp,
                                                 .rgba       = s_rgba,
                                                 .rgba_cap   = sizeof s_rgba,
                                                 .gray       = s_gray,
                                                 .gray_cap   = sizeof s_gray};
  return (ra8_rabook_comic_workspace_t){.builder            = builder,
                                        .raster             = raster,
                                        .normalized         = s_normalized,
                                        .normalized_cap     = sizeof s_normalized,
                                        .stream_scratch     = s_stream_scratch,
                                        .stream_scratch_cap = sizeof s_stream_scratch};
}

/**
 * @brief Build two real pages into a canonical flat RABOOK1 memory object.
 * @details Initializes the production comic facade, decodes two BMP pages,
 *          attaches complete metadata, and finalizes through the append seam.
 * @param[out] image Receives normalized external image-spool bytes.
 * @param[out] flat Receives the complete flat book bytes.
 * @return Production build status.
 * @retval k_ra8_ok Two chapters were normalized and finalized.
 * @return Initialization, raster, spool, or finalization errors propagate.
 * @pre Output memory objects bind cleared fixture storage.
 * @pre Static builder/codec arenas are exclusively owned.
 * @post Success publishes a strict flat book with two images and chapters.
 * @post Failure leaves only private discardable fixture prefixes.
 * @note Test-only orchestration helper.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_build_flat(comic_memory_t* image, comic_memory_t* flat)
{
  ra8_img_arena_t              stb       = {};
  ra8_webp_arena_t             webp      = {};
  ra8_rabook_comic_workspace_t workspace = internal_workspace(&stb, &webp);
  ra8_rabook_comic_t           comic     = {};
  ra8_err_t                    result    = ra8_rabook_comic_init(&comic,
                                                                 &workspace,
                                                                 internal_memory_read_progress,
                                                                 internal_image_write,
                                                                 image,
                                                                 0U,
                                                                 (uint8_t)k_book_pixfmt_gray4);
  if (result == k_ra8_ok) {
    result = ra8_rabook_comic_add_page(&comic, "page-001.bmp", s_bmp, sizeof s_bmp);
  }
  if (result == k_ra8_ok) {
    result = ra8_rabook_comic_add_page(&comic, "page-002.bmp", s_bmp, sizeof s_bmp);
  }
  const ra8_rabook_comic_metadata_t metadata = {.title      = "Fetched Comic",
                                                .author     = "C6 source",
                                                .language   = "en",
                                                .identifier = "urn:ra8:test:comic"};
  if (result == k_ra8_ok) {
    uint32_t flat_length = 0U;
    result = ra8_rabook_comic_finish(&comic, &metadata, internal_flat_append, flat, &flat_length);
    if ((result == k_ra8_ok) && (flat_length != flat->length)) {
      result = k_ra8_err_validation_failed;
    }
  }
  return result;
}

/**
 * @brief Wrap a flat fixture in production RBKC and strictly reopen it.
 * @details Runs the actual chunk compressor and random-write table back-fill,
 *          opens the resulting container through the chunk reader, then
 *          validates all compressed streams and inner RABOOK1 structure.
 * @param[in] flat Valid flat RABOOK1 memory source.
 * @param[out] packed Receives the chunked container.
 * @param[out] out_header Strictly validated inner header.
 * @return Container, reader, or strict validation status.
 * @retval k_ra8_ok Every compressed stream and inner field validated.
 * @return Writer, reader, inflater, or validator failures propagate.
 * @pre Memory objects and static container workspaces are exclusive.
 * @pre @p flat contains one strict nonempty flat book.
 * @post Success initializes @p packed and @p out_header.
 * @post Failure never reports a valid header.
 * @note Test-only end-to-end reader proof.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_wrap_and_validate(const comic_memory_t* flat,
                                                         comic_memory_t*       packed,
                                                         book_header_t*        out_header)
{
  static uint8_t                   s_chunk_input[k_comic_chunk_bytes];
  static uint8_t                   s_deflate[k_comic_deflate_bytes];
  static uint8_t                   s_reader_staging[k_comic_deflate_bytes];
  static uint8_t                   s_reader_chunk[k_comic_chunk_bytes];
  static uint8_t                   s_validate[k_comic_validate_bytes];
  static uint64_t                  s_offsets[k_comic_offsets];
  static uint64_t                  s_reader_offsets[k_comic_offsets];
  static comic_compressor_t        s_compressor;
  comic_memory_t                   flat_reader   = *flat;
  ra8_rabook_container_workspace_t writer        = {.input          = s_chunk_input,
                                                    .compressed     = s_deflate,
                                                    .compressor     = s_compressor.bytes,
                                                    .offsets        = s_offsets,
                                                    .input_cap      = sizeof s_chunk_input,
                                                    .compressed_cap = sizeof s_deflate,
                                                    .compressor_cap = sizeof s_compressor.bytes,
                                                    .offset_cap     = k_comic_offsets};
  uint64_t                         packed_length = 0U;
  ra8_err_t                        result        = ra8_rabook_container_write(internal_flat_read,
                                                                              &flat_reader,
                                                                              flat_reader.length,
                                                                              k_comic_chunk_bytes,
                                                                              internal_packed_write,
                                                                              packed,
                                                                              &writer,
                                                                              &packed_length);
  if ((result == k_ra8_ok) && (packed_length != packed->length)) {
    result = k_ra8_err_validation_failed;
  }
  book_chunked_t reader = {};
  if (result == k_ra8_ok) {
    result = book_chunked_open(&reader,
                               internal_packed_read,
                               packed,
                               packed_length,
                               internal_inflate,
                               s_reader_offsets,
                               k_comic_offsets,
                               s_reader_staging,
                               sizeof s_reader_staging);
  }
  if (result == k_ra8_ok) {
    result = book_chunked_validate_strict(&reader,
                                          s_reader_chunk,
                                          sizeof s_reader_chunk,
                                          s_validate,
                                          sizeof s_validate,
                                          out_header);
  }
  return result;
}

/** @brief Bind one comic fixture whose builder arenas carry chosen capacities. @details Reuses the shared workspace constructor, then narrows only the chapter, image, and string capacities so each fault vector starves exactly one arena while every other input stays valid. @param[out] fixture Complete caller state for one constrained build. @param[in] caps Chapter, image, and string capacities for this vector. @return Production initialization status. @retval k_ra8_ok The empty comic is active and ready for one page. @return Constructor validation or builder errors propagate unchanged. @pre Static fixture storage is exclusively owned by the caller. @pre @p fixture and @p caps are non-NULL and writable/readable. @post Success leaves an active zero-page comic bound to @p fixture storage. @post Both fixture memory extents start at zero initialized bytes. @note Not thread-safe because the fixture shares static backing arrays. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_begin_capped(comic_fault_fixture_t* fixture,
                                                    const comic_caps_t*    caps)
{
  *fixture           = (comic_fault_fixture_t){};
  fixture->image     = (comic_memory_t){.bytes = s_image_bytes, .capacity = sizeof s_image_bytes};
  fixture->flat      = (comic_memory_t){.bytes = s_flat_bytes, .capacity = sizeof s_flat_bytes};
  fixture->workspace = internal_workspace(&fixture->stb, &fixture->webp);
  fixture->workspace.builder.chapter_cap = caps->chapter_cap;
  fixture->workspace.builder.image_cap   = caps->image_cap;
  fixture->workspace.builder.string_cap  = caps->string_cap;
  return ra8_rabook_comic_init(&fixture->comic,
                               &fixture->workspace,
                               internal_memory_read_progress,
                               internal_image_write,
                               &fixture->image,
                               (uint16_t)k_comic_edge_unclamped,
                               (uint8_t)k_book_pixfmt_gray4);
}

/** @brief Run one add-page vector against a freshly constrained fixture. @details Normalizes the real BMP fixture so the failure originates in the builder arena under test, then proves the build is poisoned by rejecting a second page without touching the spool again. @param[in] caps Chapter, image, and string capacities for this vector. @param[in] expected Exact expected add-page status. @pre Static fixture storage is exclusively owned by the caller. @pre @p caps leaves every arena except the one under test sufficient. @post The observed add-page result equals @p expected. @post A failed vector reports zero pages and refuses further pages. @note Test-only orchestration helper; not thread-safe. @since 0.1.0 */
RA8_INTERNAL static void internal_expect_capped_add(const comic_caps_t* caps, ra8_err_t expected)
{
  comic_fault_fixture_t fixture = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_begin_capped(&fixture, caps));
  TEST_ASSERT_EQ(expected, ra8_rabook_comic_add_page(&fixture.comic, "pg", s_bmp, sizeof s_bmp));
  TEST_ASSERT_EQ(k_comic_no_pages, fixture.comic.page_count);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rabook_comic_add_page(&fixture.comic, "pg2", s_bmp, sizeof s_bmp));
}

/** @brief Run one finish vector against a freshly built one-page comic. @details Requires the page to land before finish runs, so a metadata or string-arena failure cannot be confused with an earlier fault. @param[in] caps Chapter, image, and string capacities for this vector. @param[in] metadata Candidate final metadata, or NULL for the null guard. @param[in] expected Exact expected finish status. @pre Static fixture storage is exclusively owned by the caller. @pre Non-NULL metadata members remain readable for the call. @post The observed finish result equals @p expected. @post Exactly one page was appended before finish was invoked. @note Test-only orchestration helper; not thread-safe. @since 0.1.0 */
RA8_INTERNAL static void internal_expect_capped_finish(const comic_caps_t*                caps,
                                                       const ra8_rabook_comic_metadata_t* metadata,
                                                       ra8_err_t                          expected)
{
  comic_fault_fixture_t fixture = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_begin_capped(&fixture, caps));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_comic_add_page(&fixture.comic, "pg", s_bmp, sizeof s_bmp));
  TEST_ASSERT_EQ(k_comic_one_page, fixture.comic.page_count);
  uint32_t flat_length = UINT32_MAX;
  TEST_ASSERT_EQ(expected,
                 ra8_rabook_comic_finish(&fixture.comic,
                                         metadata,
                                         internal_flat_append,
                                         &fixture.flat,
                                         &flat_length));
}

/**
 * @brief Comic pages become a strict reader-consumable RBKC artifact.
 * @test The production image-sequence facade emits a usable `.rabook` file.
 * @details Uses the real raster normalizer, external image spool, flat
 * finalizer, chunk compressor, inflater, and strict reader in one path.
 * @pre Static fixture state is exclusively owned.
 * @pre Embedded BMP bytes remain unchanged.
 * @post The artifact contains two page chapters and two normalized images.
 * @post Metadata, cover selection, DOM src values, and image offsets are exact.
 * @note This is the non-vacuous source-image-to-rabook acceptance proof.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_image_sequence_to_rbkc(void)
{
  TEST_BEGIN("rabook comic: source images to strict RBKC");
  comic_memory_t image  = {.bytes = s_image_bytes, .capacity = sizeof s_image_bytes};
  comic_memory_t flat   = {.bytes = s_flat_bytes, .capacity = sizeof s_flat_bytes};
  comic_memory_t packed = {.bytes = s_packed_bytes, .capacity = sizeof s_packed_bytes};
  TEST_ASSERT_EQ(k_ra8_ok, internal_build_flat(&image, &flat));
  TEST_ASSERT_EQ(k_ra8_ok, book_validate(flat.bytes, flat.length));
  book_header_t header = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_wrap_and_validate(&flat, &packed, &header));
  TEST_ASSERT_EQ(k_comic_pages, header.chapter_count);
  TEST_ASSERT_EQ(k_comic_pages, header.image_count);
  TEST_ASSERT_EQ(0U, header.cover_image_index);
  TEST_ASSERT_EQ(0, strcmp(book_string(flat.bytes, header.title_off), "Fetched Comic"));
  const book_chapter_t* chapters = book_chapters(flat.bytes);
  const book_node_t*    nodes    = book_nodes(flat.bytes);
  TEST_ASSERT_EQ(0, strcmp(book_string(flat.bytes, chapters[0].href_off), "page-001.bmp"));
  TEST_ASSERT_EQ(0, strcmp(book_node_name(flat.bytes, &nodes[chapters[0].root_node]), "body"));
  const book_node_t* image_node = &nodes[nodes[chapters[0].root_node].first_child];
  TEST_ASSERT_EQ(0, strcmp(book_node_name(flat.bytes, image_node), "img"));
  const book_attr_t* attr = &book_attrs(flat.bytes)[image_node->first_attr];
  TEST_ASSERT_EQ(0, strcmp(book_string(flat.bytes, attr->name_off), "src"));
  TEST_ASSERT_EQ(0, strcmp(book_string(flat.bytes, attr->value_off), "page-001.bmp"));
  const book_image_t* images = book_images(flat.bytes);
  TEST_ASSERT_EQ(0U, images[0].data_off);
  TEST_ASSERT_EQ(images[0].data_size, images[1].data_off);
  TEST_ASSERT_EQ(image.length, header.image_pool_size);
  TEST_END("rabook comic: source images to strict RBKC");
}

/**
 * @brief A short image-spool write poisons the private comic transaction.
 * @test The comic facade fails closed on incomplete backing-store writes.
 * @details Injects one successful short write after a real decode, then proves
 * no page count advances and no later page can reuse the contaminated build.
 * @pre Static fixture state is exclusively owned.
 * @pre The short-write flag remains enabled for the first page call.
 * @post Add-page returns invalid-size and the builder becomes inactive.
 * @post A second add-page returns invalid-state without another spool write.
 * @note Caller cleanup discards the private image/flat spools after this fault.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_short_spool_write_fails_closed(void)
{
  TEST_BEGIN("rabook comic: short spool write fails closed");
  comic_memory_t               image     = {.bytes       = s_image_bytes,
                                            .capacity    = sizeof s_image_bytes,
                                            .short_write = true};
  ra8_img_arena_t              stb       = {};
  ra8_webp_arena_t             webp      = {};
  ra8_rabook_comic_workspace_t workspace = internal_workspace(&stb, &webp);
  ra8_rabook_comic_t           comic     = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_comic_init(&comic,
                                       &workspace,
                                       internal_memory_read_progress,
                                       internal_image_write,
                                       &image,
                                       0U,
                                       (uint8_t)k_book_pixfmt_gray4));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_comic_add_page(&comic, "page.bmp", s_bmp, sizeof s_bmp));
  const uint32_t length_after_fault = image.length;
  TEST_ASSERT_EQ(0U, comic.page_count);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rabook_comic_add_page(&comic, "later.bmp", s_bmp, sizeof s_bmp));
  TEST_ASSERT_EQ(length_after_fault, image.length);
  TEST_END("rabook comic: short spool write fails closed");
}

/**
 * @brief Every public entry rejects a NULL comic before touching the caller.
 * @test The three facade entry points guard their own state pointer.
 * @details Each call supplies otherwise-complete arguments so the only reason
 * to fail is the missing builder state itself.
 * @pre Static fixture state is exclusively owned.
 * @pre The workspace and metadata controls are complete and valid.
 * @post Every entry point returns the null-pointer status.
 * @post No fixture byte is written because no build was ever created.
 * @note These are single-condition guards, so no MC/DC vector table applies.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_null_comic_rejected(void)
{
  TEST_BEGIN("rabook comic: null comic rejected");
  comic_memory_t   image = {.bytes = s_image_bytes, .capacity = sizeof s_image_bytes};
  comic_memory_t   flat  = {.bytes = s_flat_bytes, .capacity = sizeof s_flat_bytes};
  ra8_img_arena_t  stb   = {};
  ra8_webp_arena_t webp  = {};
  const ra8_rabook_comic_workspace_t workspace   = internal_workspace(&stb, &webp);
  const ra8_rabook_comic_metadata_t  metadata    = {.title      = "Comic",
                                                    .author     = "RA8",
                                                    .language   = "en",
                                                    .identifier = "urn:ra8:test:null"};
  uint32_t                           flat_length = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_comic_init(nullptr,
                                       &workspace,
                                       internal_memory_read_progress,
                                       internal_image_write,
                                       &image,
                                       (uint16_t)k_comic_edge_unclamped,
                                       (uint8_t)k_book_pixfmt_gray4));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_comic_add_page(nullptr, "page.bmp", s_bmp, sizeof s_bmp));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rabook_comic_finish(nullptr, &metadata, internal_flat_append, &flat, &flat_length));
  TEST_ASSERT_EQ(k_comic_no_bytes, image.length);
  TEST_ASSERT_EQ(k_comic_no_bytes, flat.length);
  TEST_END("rabook comic: null comic rejected");
}

/**
 * @brief An unterminated page identifier is rejected and poisons the build.
 * @test The bounded-string scan refuses metadata that never terminates.
 * @details The candidate identifier fills the complete public string window
 * with non-NUL bytes, which is exactly the bound the scanner may inspect.
 * @pre Static fixture state is exclusively owned.
 * @pre The hostile identifier spans the exact public bound in readable bytes.
 * @post Add-page returns the invalid-size status and appends no page.
 * @post The poisoned build refuses a later well-formed page.
 * @note The scan reads no byte beyond the fixed public window.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_unbounded_page_id_rejected(void)
{
  TEST_BEGIN("rabook comic: unbounded page id rejected");
  const comic_caps_t    roomy   = {.chapter_cap = k_comic_chapters,
                                   .image_cap   = k_comic_images,
                                   .string_cap  = k_comic_strings};
  comic_fault_fixture_t fixture = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_begin_capped(&fixture, &roomy));
  char page_id[k_ra8_rabook_comic_string_max];
  (void)memset(page_id, 'x', sizeof page_id);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_comic_add_page(&fixture.comic, page_id, s_bmp, sizeof s_bmp));
  TEST_ASSERT_EQ(k_comic_no_pages, fixture.comic.page_count);
  TEST_ASSERT_EQ(k_comic_no_bytes, fixture.image.length);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rabook_comic_add_page(&fixture.comic, "later.bmp", s_bmp, sizeof s_bmp));
  TEST_END("rabook comic: unbounded page id rejected");
}

/**
 * @brief Finishing an empty comic reports empty, then reports inactive.
 * @test The finish lifecycle transition happens before the emptiness check.
 * @details A comic that never received a page cannot produce a book, and the
 * failed finish still consumes the build so a retry is a state error.
 * @pre Static fixture state is exclusively owned.
 * @pre The metadata control is complete and bounded.
 * @post The first finish returns the empty status and writes no flat byte.
 * @post The second finish returns the invalid-state status.
 * @note Both guards are single conditions, so no MC/DC vector table applies.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_empty_then_inactive_finish(void)
{
  TEST_BEGIN("rabook comic: empty then inactive finish");
  const comic_caps_t    roomy   = {.chapter_cap = k_comic_chapters,
                                   .image_cap   = k_comic_images,
                                   .string_cap  = k_comic_strings};
  comic_fault_fixture_t fixture = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_begin_capped(&fixture, &roomy));
  const ra8_rabook_comic_metadata_t metadata    = {.title      = "Comic",
                                                   .author     = "RA8",
                                                   .language   = "en",
                                                   .identifier = "urn:ra8:test:empty"};
  uint32_t                          flat_length = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_empty,
                 ra8_rabook_comic_finish(&fixture.comic,
                                         &metadata,
                                         internal_flat_append,
                                         &fixture.flat,
                                         &flat_length));
  TEST_ASSERT_EQ(k_comic_no_bytes, fixture.flat.length);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rabook_comic_finish(&fixture.comic,
                                         &metadata,
                                         internal_flat_append,
                                         &fixture.flat,
                                         &flat_length));
  TEST_ASSERT_EQ(k_comic_no_bytes, fixture.flat.length);
  TEST_END("rabook comic: empty then inactive finish");
}

/**
 * @brief Missing, unbounded, and unstorable metadata each fail their own way.
 * @test Final metadata is validated and interned before any book is streamed.
 * @details The absent record, the absent title, and a title too large for the
 * remaining string arena select three distinct production rejections after a
 * real page has already been normalized and spooled.
 * @pre Static fixture state is exclusively owned.
 * @pre The tight fixture leaves fewer free string bytes than the long title.
 * @post Each vector returns its canonical status and streams no flat book.
 * @post No vector can be satisfied by a different guard's status.
 * @note The tight vector proves the metadata intern failure propagates.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_metadata_guards(void)
{
  TEST_BEGIN("rabook comic: metadata guards");
  const comic_caps_t                roomy         = {.chapter_cap = k_comic_chapters,
                                                     .image_cap   = k_comic_images,
                                                     .string_cap  = k_comic_strings};
  const comic_caps_t                tight         = {.chapter_cap = k_comic_chapters,
                                                     .image_cap   = k_comic_images,
                                                     .string_cap  = k_comic_strings_tight};
  const ra8_rabook_comic_metadata_t missing_title = {.title      = nullptr,
                                                     .author     = "RA8",
                                                     .language   = "en",
                                                     .identifier = "urn:ra8:test:meta"};
  const ra8_rabook_comic_metadata_t long_title    = {.title      = "AAAAAAAAAAAAAAAAAAAAAAAA",
                                                     .author     = "RA8",
                                                     .language   = "en",
                                                     .identifier = "urn:ra8:test:meta"};
  internal_expect_capped_finish(&roomy, nullptr, k_ra8_err_null_ptr);
  internal_expect_capped_finish(&roomy, &missing_title, k_ra8_err_invalid_size);
  internal_expect_capped_finish(&tight, &long_title, k_ra8_err_no_mem);
  TEST_END("rabook comic: metadata guards");
}

/**
 * @brief Each starved builder arena stops the page before it is counted.
 * @test String, image, and chapter exhaustion are all reported as no-memory.
 * @details The three vectors starve one arena apiece: the string pool cannot
 * intern the page identifier, the image table cannot hold a descriptor, and
 * the chapter table cannot hold the DOM chapter that a stored page requires.
 * @pre Static fixture state is exclusively owned.
 * @pre Every arena except the one under test remains sufficient.
 * @post Every vector returns no-memory and retains zero pages.
 * @post Every failed build refuses a later page with an invalid-state status.
 * @note Each vector owns a fresh private build and spool.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_builder_capacity_faults(void)
{
  TEST_BEGIN("rabook comic: builder capacity faults");
  const comic_caps_t no_strings  = {.chapter_cap = k_comic_chapters,
                                    .image_cap   = k_comic_images,
                                    .string_cap  = k_comic_strings_min};
  const comic_caps_t no_images   = {.chapter_cap = k_comic_chapters,
                                    .image_cap   = k_comic_cap_none,
                                    .string_cap  = k_comic_strings};
  const comic_caps_t no_chapters = {.chapter_cap = k_comic_cap_none,
                                    .image_cap   = k_comic_images,
                                    .string_cap  = k_comic_strings};
  internal_expect_capped_add(&no_strings, k_ra8_err_no_mem);
  internal_expect_capped_add(&no_images, k_ra8_err_no_mem);
  internal_expect_capped_add(&no_chapters, k_ra8_err_no_mem);
  TEST_END("rabook comic: builder capacity faults");
}

/**
 * @brief A refused image-spool write propagates its backend status unchanged.
 * @test The facade never rewrites a storage failure into its own vocabulary.
 * @details The scripted backend refuses the transfer instead of accepting a
 * short span, so the observed status distinguishes error propagation from the
 * short-write conversion the companion fault test covers.
 * @pre Static fixture state is exclusively owned.
 * @pre The refusing callback reports zero accepted bytes.
 * @post Add-page returns the exact backend status and counts no page.
 * @post The private spool remains completely uninitialized.
 * @note The caller discards the private spool after this fault.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_spool_write_error_propagates(void)
{
  TEST_BEGIN("rabook comic: spool write error propagates");
  comic_memory_t               image = {.bytes = s_image_bytes, .capacity = sizeof s_image_bytes};
  ra8_img_arena_t              stb   = {};
  ra8_webp_arena_t             webp  = {};
  ra8_rabook_comic_workspace_t workspace = internal_workspace(&stb, &webp);
  ra8_rabook_comic_t           comic     = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_comic_init(&comic,
                                       &workspace,
                                       internal_memory_read_progress,
                                       internal_image_write_fault,
                                       &image,
                                       (uint16_t)k_comic_edge_unclamped,
                                       (uint8_t)k_book_pixfmt_gray4));
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_rabook_comic_add_page(&comic, "pg", s_bmp, sizeof s_bmp));
  TEST_ASSERT_EQ(k_comic_no_pages, comic.page_count);
  TEST_ASSERT_EQ(k_comic_no_bytes, image.length);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rabook_comic_add_page(&comic, "pg2", s_bmp, sizeof s_bmp));
  TEST_END("rabook comic: spool write error propagates");
}

int main(void)
{
  internal_test_image_sequence_to_rbkc();
  internal_test_short_spool_write_fails_closed();
  internal_test_null_comic_rejected();
  internal_test_unbounded_page_id_rejected();
  internal_test_empty_then_inactive_finish();
  internal_test_metadata_guards();
  internal_test_builder_capacity_faults();
  internal_test_spool_write_error_propagates();
  return 0;
}
