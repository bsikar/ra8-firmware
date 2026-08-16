/**
 * @file test_app_media_download_format.c
 * @brief End-to-end test for the firmware source-image RABOOK formatter.
 * @details Converts a real BMP through the app-private composition helper,
 *          opens the emitted RBKC container, and strictly validates the inner
 *          RABOOK1 book with production reader code.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "media_download_format_internal.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_book_chunked.h"
#include "ra8_io_compress.h"
#include "ra8_webp_arena.h"
#include "unity_minimal.h"

/** @brief Bounded fixture storage dimensions. */
typedef enum : uint32_t {
  k_format_codec_bytes    = 4096U, /**< Per-codec arena bytes.          */
  k_format_strings        = 512U,  /**< Builder string-pool bytes.      */
  k_format_image_bytes    = 64U,   /**< Normalized image-spool bytes.   */
  k_format_flat_bytes     = 2048U, /**< Flat RABOOK1 capacity.          */
  k_format_packed_bytes   = 4096U, /**< Chunked RBKC capacity.          */
  k_format_chunk_bytes    = 256U,  /**< Inflated chunk geometry.        */
  k_format_compressed     = 512U,  /**< One compressed-stream capacity. */
  k_format_offsets        = 32U,   /**< RBKC table entries.             */
  k_format_validate_bytes = 512U,  /**< Strict semantic scratch.        */
} format_fixture_limit_t;

/** @brief Aligned miniz compressor storage. */
typedef union {
  max_align_t align;                                  /**< Portable alignment member. */
  uint8_t     bytes[k_ra8_io_compress_scratch_bytes]; /**< Compressor backing.        */
} format_compressor_t;

/** @brief Tiny 2x2 24-bit BMP accepted by the real raster decoder. */
static const uint8_t s_bmp[] = {
  0x42, 0x4D, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
  0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00,
  0x13, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF,
  0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00,
};

static ra8_book_chapter_t    s_chapters[1];
static ra8_book_node_t       s_nodes[2];
static ra8_book_attr_t       s_attrs[1];
static ra8_book_stylesheet_t s_styles[1];
static ra8_book_image_t      s_images[1];
static char                  s_strings[k_format_strings];
static uint8_t               s_dummy_image[1];
static uint8_t               s_dummy_output[1];
alignas(16) static uint8_t s_stb_arena[k_format_codec_bytes];
alignas(16) static uint8_t s_webp_arena[k_format_codec_bytes];
static uint8_t             s_rgba[k_format_image_bytes];
static uint8_t             s_gray[k_format_image_bytes];
static uint8_t             s_normalized[k_format_image_bytes];
static uint8_t             s_stream[k_format_image_bytes];
static uint8_t             s_image[k_format_image_bytes];
static uint8_t             s_flat[k_format_flat_bytes];
static uint8_t             s_packed[k_format_packed_bytes];
static uint8_t             s_container_input[k_format_chunk_bytes];
static uint8_t             s_container_compressed[k_format_compressed];
static uint64_t            s_container_offsets[k_format_offsets];
static format_compressor_t s_compressor;
static uint8_t             s_reader_compressed[k_format_compressed];
static uint8_t             s_reader_chunk[k_format_chunk_bytes];
static uint8_t             s_reader_scratch[k_format_validate_bytes];
static uint64_t            s_reader_offsets[k_format_offsets];

/**
 * @brief Read one exact range from the produced packed fixture.
 * @details Bounds-checks against the packed length passed as callback context.
 * @param[in] context Pointer to the complete packed byte count.
 * @param[in] offset Absolute artifact byte offset.
 * @param[out] destination Writable destination span.
 * @param[in] requested Exact requested byte count.
 * @return Packed-read status.
 * @retval k_ra8_ok The complete range was copied.
 * @retval k_ra8_err_invalid_size The range exceeds the packed artifact.
 * @pre Context and destination are non-NULL and remain live.
 * @pre The packed fixture is immutable.
 * @post Success initializes every requested destination byte.
 * @post Failure leaves the source fixture unchanged.
 * @note Test-only callback over fixed storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_packed_read(void* context, uint64_t offset, uint8_t* destination, uint32_t requested)
{
  const uint64_t length = *(const uint64_t*)context;
  if ((offset > length) || (requested > (length - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(destination, &s_packed[(size_t)offset], requested);
  return k_ra8_ok;
}

/**
 * @brief Inflate one zlib-wrapped RBKC stream through miniz.
 * @details Maps miniz's failure sentinel to the canonical validation status.
 * @param[in] source Compressed stream bytes.
 * @param[in] source_bytes Compressed byte count.
 * @param[out] destination Inflate destination.
 * @param[in] destination_capacity Writable destination capacity.
 * @param[out] output_bytes Exact inflated byte count.
 * @return Inflation status.
 * @retval k_ra8_ok One complete zlib stream inflated.
 * @retval k_ra8_err_validation_failed Miniz rejected the stream.
 * @pre Pointer arguments are non-NULL and extents are exact.
 * @pre Destination does not overlap source.
 * @post Success initializes exactly @p output_bytes destination bytes.
 * @post Failure reports no successful reader operation.
 * @note No allocator is enabled in this test target.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_inflate(const void* source,
                                               size_t      source_bytes,
                                               void*       destination,
                                               size_t      destination_capacity,
                                               size_t*     output_bytes)
{
  const size_t length = tinfl_decompress_mem_to_mem(
    destination,
    destination_capacity,
    source,
    source_bytes,
    (uint32_t)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  if (length == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
    return k_ra8_err_validation_failed;
  }
  *output_bytes = length;
  return k_ra8_ok;
}

/**
 * @brief Bind every fixed fixture span to the app-private formatter.
 * @details Clears logical output bytes and constructs the nested builder,
 *          raster, and container descriptors without hidden storage.
 * @param[out] stb Caller descriptor for the stb arena.
 * @param[out] webp Caller descriptor for the WebP arena.
 * @return Complete formatter workspace by value.
 * @retval media_download_format_workspace_t Every fixed span is bound.
 * @pre Static fixture storage is exclusively owned.
 * @pre Descriptor output pointers are non-NULL.
 * @post Codec arenas are reset and output buffers are zeroed.
 * @post Every capacity equals its backing array extent.
 * @note Test-only helper; not thread-safe through static storage.
 * @since 0.1.0
 */
RA8_INTERNAL static media_download_format_workspace_t internal_workspace(ra8_img_arena_t*  stb,
                                                                         ra8_webp_arena_t* webp)
{
  (void)memset(s_image, 0, sizeof s_image);
  (void)memset(s_flat, 0, sizeof s_flat);
  (void)memset(s_packed, 0, sizeof s_packed);
  *stb  = (ra8_img_arena_t){.base = s_stb_arena, .cap = sizeof s_stb_arena};
  *webp = (ra8_webp_arena_t){.base = s_webp_arena, .cap = sizeof s_webp_arena};
  const ra8_rabook_buffers_t         builder = {.chapters       = s_chapters,
                                                .nodes          = s_nodes,
                                                .attrs          = s_attrs,
                                                .stylesheets    = s_styles,
                                                .images         = s_images,
                                                .string_pool    = s_strings,
                                                .image_pool     = s_dummy_image,
                                                .out            = s_dummy_output,
                                                .chapter_cap    = 1U,
                                                .node_cap       = 2U,
                                                .attr_cap       = 1U,
                                                .stylesheet_cap = 1U,
                                                .image_cap      = 1U,
                                                .string_cap     = sizeof s_strings,
                                                .image_pool_cap = sizeof s_dummy_image,
                                                .out_cap        = sizeof s_dummy_output};
  const ra8_rabook_comic_workspace_t comic   = {
    .builder            = builder,
    .raster             = {.stb_arena  = stb,
                           .webp_arena = webp,
                           .rgba       = s_rgba,
                           .rgba_cap   = sizeof s_rgba,
                           .gray       = s_gray,
                           .gray_cap   = sizeof s_gray},
    .normalized         = s_normalized,
    .normalized_cap     = sizeof s_normalized,
    .stream_scratch     = s_stream,
    .stream_scratch_cap = sizeof s_stream,
  };
  const ra8_rabook_container_workspace_t container = {
    .input          = s_container_input,
    .compressed     = s_container_compressed,
    .compressor     = s_compressor.bytes,
    .offsets        = s_container_offsets,
    .input_cap      = sizeof s_container_input,
    .compressed_cap = sizeof s_container_compressed,
    .compressor_cap = sizeof s_compressor.bytes,
    .offset_cap     = k_format_offsets,
  };
  return (media_download_format_workspace_t){.comic      = comic,
                                             .container  = container,
                                             .image      = s_image,
                                             .flat       = s_flat,
                                             .packed     = s_packed,
                                             .image_cap  = sizeof s_image,
                                             .flat_cap   = sizeof s_flat,
                                             .packed_cap = sizeof s_packed};
}

/**
 * @brief A fetched image becomes a strict reader-consumable `.rabook`.
 * @test The app formatter emits valid RBKC and canonical inner RABOOK1 bytes.
 * @details Runs the production raster, comic, container, reader, inflater, and
 *          strict semantic validator over one deterministic BMP source.
 * @pre Static fixture storage is exclusively owned.
 * @pre Production formatter and reader code are linked into the target.
 * @post The validated book has one chapter, image, and cover index zero.
 * @post The packed artifact is nonempty and begins with RBKC magic.
 * @note Proves conversion rather than pass-through of a prebuilt artifact.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_image_to_rabook(void)
{
  TEST_BEGIN("media download format: image to rabook");
  ra8_img_arena_t                      stb       = {};
  ra8_webp_arena_t                     webp      = {};
  media_download_format_workspace_t    workspace = internal_workspace(&stb, &webp);
  const media_download_format_config_t config    = {
    .page_id        = "page-001.bmp",
    .metadata       = {.title      = "Fetched Page",
                       .author     = "C6 source",
                       .language   = "en",
                       .identifier = "urn:ra8:test:download-format"},
    .chunk_bytes    = k_format_chunk_bytes,
    .max_image_edge = 0U,
    .pixel_format   = (uint8_t)k_ra8_book_pixfmt_gray4,
  };
  uint64_t packed_size = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_media_download_format_rabook(s_bmp, sizeof s_bmp, &config, &workspace, &packed_size));
  TEST_ASSERT(packed_size > 4U);
  TEST_ASSERT_EQ(0, memcmp(s_packed, "RBKC", 4U));
  ra8_book_chunked_t reader = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_chunked_open(&reader,
                                       internal_packed_read,
                                       &packed_size,
                                       packed_size,
                                       internal_inflate,
                                       s_reader_offsets,
                                       k_format_offsets,
                                       s_reader_compressed,
                                       sizeof s_reader_compressed));
  ra8_book_header_t header = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_chunked_validate_strict(&reader,
                                                  s_reader_chunk,
                                                  sizeof s_reader_chunk,
                                                  s_reader_scratch,
                                                  sizeof s_reader_scratch,
                                                  &header));
  TEST_ASSERT_EQ(1U, header.chapter_count);
  TEST_ASSERT_EQ(1U, header.image_count);
  TEST_ASSERT_EQ(0U, header.cover_image_index);
  TEST_END("media download format: image to rabook");
}

int main(void)
{
  internal_test_image_to_rabook();
  return 0;
}
