/**
 * @file media_download_image.c
 * @brief Convert one C6-fetched encoded image into a strict `.rabook`.
 * @details Owns only fixed application workspaces. Source bytes are first
 *          committed to private RAM, transformed into canonical RABOOK1 and
 *          RBKC, then hashed, strictly validated, and published through the
 *          injected VFS transaction.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "media_download_format_internal.h"
#include "media_download_image_internal.h"
#include "ra8_book.h"
#include "ra8_img_arena.h"
#include "ra8_io_compress.h"
#include "ra8_mdl_format.h"
#include "ra8_mdl_storage_ram.h"
#include "ra8_webp_arena.h"

/** @brief Fixed one-page source and conversion capacities. */
typedef enum : uint32_t {
  k_image_source_bytes     = 4194304U,  /**< Maximum fetched encoded image. */
  k_image_stb_arena_bytes  = 12582912U, /**< stb decoder allocation arena.  */
  k_image_webp_arena_bytes = 4194304U,  /**< WebP decoder scratch arena.    */
  k_image_rgba_bytes       = 16777216U, /**< Maximum decoded RGBA frame.    */
  k_image_edge             = 1600U,     /**< Longest normalized page edge.  */
  k_image_gray_bytes       = 2560000U,  /**< 1600x1600 grayscale frame.     */
  k_image_normalized_bytes = 1280000U,  /**< Gray4 normalized page bytes.   */
  k_image_flat_bytes       = 1500000U,  /**< Flat RABOOK1 capacity.         */
  k_image_packed_bytes     = 1600000U,  /**< Final RBKC capacity.           */
  k_image_string_bytes     = 1024U,     /**< Builder string-pool bytes.     */
  k_image_chunk_bytes      = 65536U,    /**< RBKC inflated chunk geometry.  */
  k_image_compressed_bytes = 66560U,    /**< One zlib stream capacity.      */
  k_image_offset_entries   = 25U,       /**< Flat chunks plus terminator.   */
} image_limit_t;

/** @brief Aligned caller-owned miniz compressor storage. */
typedef union {
  max_align_t align;                                  /**< Portable alignment member. */
  uint8_t     bytes[k_ra8_io_compress_scratch_bytes]; /**< Compressor state bytes.    */
} image_compressor_t;

/** @brief Real application output selection, independent of the C6 transport. */
static const ra8_mdl_format_t s_output_format = k_ra8_mdl_format_rabook;
/** @brief Private transaction for the encoded source bytes. */
static ra8_mdl_storage_ram_t s_source_storage;
/** @brief One-page resident builder tables. */
static ra8_book_chapter_t s_chapters[1];
/** @brief Minimal body/img DOM. */
static ra8_book_node_t s_nodes[2];
/** @brief One image-source attribute. */
static ra8_book_attr_t s_attrs[1];
/** @brief Required non-null stylesheet arena. */
static ra8_book_stylesheet_t s_styles[1];
/** @brief One normalized image descriptor. */
static ra8_book_image_t s_images[1];
/** @brief Metadata and DOM string pool. */
static char s_strings[k_image_string_bytes];
/** @brief External-image mode sentinel arena. */
static uint8_t s_dummy_image[1];
/** @brief Streaming-output mode sentinel arena. */
static uint8_t s_dummy_output[1];

/** @brief Complete fetched encoded image. */
[[gnu::section(".sdram_data"), gnu::aligned(16)]] static uint8_t s_source[k_image_source_bytes];
/** @brief Caller arena for stb JPEG/PNG/GIF/BMP decoding. */
[[gnu::section(".sdram_data"),
  gnu::aligned(16)]] static uint8_t s_stb_arena[k_image_stb_arena_bytes];
/** @brief Caller arena for WebP decoder transient storage. */
[[gnu::section(".sdram_data"),
  gnu::aligned(16)]] static uint8_t s_webp_arena[k_image_webp_arena_bytes];
/** @brief Full-resolution WebP RGBA frame storage. */
[[gnu::section(".sdram_data"), gnu::aligned(16)]] static uint8_t s_rgba[k_image_rgba_bytes];
/** @brief Downscaled grayscale frame storage. */
[[gnu::section(".sdram_data"), gnu::aligned(16)]] static uint8_t s_gray[k_image_gray_bytes];
/** @brief One normalized gray4 payload. */
[[gnu::section(".sdram_data"),
  gnu::aligned(16)]] static uint8_t s_normalized[k_image_normalized_bytes];
/** @brief External normalized-image spool. */
[[gnu::section(".sdram_data"), gnu::aligned(16)]] static uint8_t s_image[k_image_normalized_bytes];
/** @brief Complete private flat RABOOK1 spool. */
[[gnu::section(".sdram_data"), gnu::aligned(16)]] static uint8_t s_flat[k_image_flat_bytes];
/** @brief Complete private chunked RBKC output. */
[[gnu::section(".sdram_data"), gnu::aligned(16)]] static uint8_t s_packed[k_image_packed_bytes];
/** @brief One uncompressed container chunk. */
[[gnu::section(".sdram_data"),
  gnu::aligned(16)]] static uint8_t s_container_input[k_image_chunk_bytes];
/** @brief One compressed container stream. */
[[gnu::section(".sdram_data"),
  gnu::aligned(16)]] static uint8_t s_container_compressed[k_image_compressed_bytes];
/** @brief Complete RBKC offset table for the bounded flat spool. */
[[gnu::section(".sdram_data"),
  gnu::aligned(16)]] static uint64_t s_container_offsets[k_image_offset_entries];
/** @brief RBKC compressor state. */
[[gnu::section(".sdram_data"), gnu::aligned(16)]] static image_compressor_t s_compressor;

/**
 * @brief Construct the fixed one-page builder arena descriptor.
 * @return Complete builder buffers by value.
 * @retval ra8_rabook_buffers_t Every member names its exact backing extent.
 * @pre Static builder arrays are exclusively owned by one conversion.
 * @pre No retained builder context refers to the arrays.
 * @post No backing byte is initialized by this descriptor-only operation.
 * @post Every typed capacity matches its one-page table.
 * @note File-local and not thread-safe through shared static storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_rabook_buffers_t internal_builder(void)
{
  return (ra8_rabook_buffers_t){.chapters       = s_chapters,
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
}

/**
 * @brief Bind all fixed source-image formatter workspaces.
 * @param[out] stb Caller descriptor for the stb arena.
 * @param[out] webp Caller descriptor for the WebP arena.
 * @return Complete formatter workspace by value.
 * @retval media_download_format_workspace_t Every fixed span is bound.
 * @pre Static conversion storage is exclusively owned.
 * @pre Descriptor outputs are non-null and writable.
 * @post Codec arenas are reset and all capacities match their backing arrays.
 * @post No source or output byte is published.
 * @note File-local and not thread-safe through codec arena bindings.
 * @since 0.1.0
 */
RA8_INTERNAL static media_download_format_workspace_t internal_workspace(ra8_img_arena_t*  stb,
                                                                         ra8_webp_arena_t* webp)
{
  *stb  = (ra8_img_arena_t){.base = s_stb_arena, .cap = sizeof s_stb_arena};
  *webp = (ra8_webp_arena_t){.base = s_webp_arena, .cap = sizeof s_webp_arena};
  const ra8_rabook_comic_workspace_t comic = {
    .builder            = internal_builder(),
    .raster             = {.stb_arena  = stb,
                           .webp_arena = webp,
                           .rgba       = s_rgba,
                           .rgba_cap   = sizeof s_rgba,
                           .gray       = s_gray,
                           .gray_cap   = sizeof s_gray},
    .normalized         = s_normalized,
    .normalized_cap     = sizeof s_normalized,
    .stream_scratch     = s_container_input,
    .stream_scratch_cap = sizeof s_container_input,
  };
  const ra8_rabook_container_workspace_t container = {
    .input          = s_container_input,
    .compressed     = s_container_compressed,
    .compressor     = s_compressor.bytes,
    .offsets        = s_container_offsets,
    .input_cap      = sizeof s_container_input,
    .compressed_cap = sizeof s_container_compressed,
    .compressor_cap = sizeof s_compressor.bytes,
    .offset_cap     = k_image_offset_entries,
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
 * @brief Convert the committed encoded source into private RBKC bytes.
 * @param[out] packed_size Exact generated artifact size.
 * @return Source-view or formatter status.
 * @retval k_ra8_ok A complete private RBKC artifact occupies ::s_packed.
 * @retval k_ra8_err_not_supported Output policy is not RABOOK.
 * @pre The source RAM transaction committed a nonempty immutable view.
 * @pre All conversion workspaces are exclusively owned.
 * @post Success initializes exactly @p packed_size packed bytes.
 * @post Failure publishes no VFS object.
 * @note Metadata is fixed because this mode imports one direct image URL.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_format(uint64_t* packed_size)
{
  if (s_output_format != k_ra8_mdl_format_rabook) {
    return k_ra8_err_not_supported;
  }
  const uint8_t* source      = nullptr;
  size_t         source_size = 0U;
  ra8_err_t      err         = ra8_mdl_storage_ram_view(&s_source_storage, &source, &source_size);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_img_arena_t                      stb       = {};
  ra8_webp_arena_t                     webp      = {};
  media_download_format_workspace_t    workspace = internal_workspace(&stb, &webp);
  const media_download_format_config_t config    = {
    .page_id        = "source-image",
    .metadata       = {.title      = "Downloaded image",
                       .author     = "C6 HTTPS source",
                       .language   = "und",
                       .identifier = "urn:ra8:media-download:image"},
    .chunk_bytes    = k_image_chunk_bytes,
    .max_image_edge = (uint16_t)k_image_edge,
    .pixel_format   = (uint8_t)k_ra8_book_pixfmt_gray4,
  };
  return priv_media_download_format_rabook(source, source_size, &config, &workspace, packed_size);
}

/**
 * @brief Append and hash one generated artifact fragment.
 * @param[in] transfer Bound publication transaction and SHA callbacks.
 * @param[in] offset First packed byte to append.
 * @param[in] length Exact fragment byte count.
 * @return Storage, progress, or SHA status.
 * @retval k_ra8_ok The fragment was stored and hashed exactly once.
 * @retval k_ra8_err_invalid_size Storage reported successful short progress.
 * @pre @p transfer owns an active private storage transaction and SHA stream.
 * @pre `offset + length` is inside the generated packed artifact.
 * @post Success appends and hashes exactly @p length ordered bytes.
 * @post Failure leaves the outer caller responsible for transaction abort.
 * @note Length is bounded to the injected uint16 chunk policy.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_publish_fragment(const ra8_mdl_transfer_config_t* transfer,
                                                        uint64_t                         offset,
                                                        uint16_t                         length)
{
  uint16_t  written = 0U;
  ra8_err_t err =
    transfer->storage.write(transfer->storage.ctx, &s_packed[(size_t)offset], length, &written);
  if ((err == k_ra8_ok) && (written != length)) {
    err = k_ra8_err_invalid_size;
  }
  if (err == k_ra8_ok) {
    err = transfer->sha256.update(transfer->sha256.ctx, &s_packed[(size_t)offset], length);
  }
  return err;
}

/**
 * @brief Strictly publish one complete locally generated RBKC artifact.
 * @param[in] transfer Bound publication transaction, validator, and SHA.
 * @param[in] packed_size Exact generated artifact byte count.
 * @param[in] destination Canonical final VFS destination.
 * @return Hash, validation, transaction, or publication status.
 * @retval k_ra8_ok The strict validated destination is visible.
 * @retval k_ra8_err_invalid_size Artifact or chunk policy is not representable.
 * @pre The packed bytes are private and immutable for the call.
 * @pre Every required callback is non-null and storage is idle or committed.
 * @post Success leaves one committed destination and no stage.
 * @post Failure after begin invokes abort and preserves the primary error.
 * @note Local generation has no remote digest; the RA8 hashes its own output.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_publish(const ra8_mdl_transfer_config_t* transfer,
                                               uint64_t                         packed_size,
                                               const char*                      destination)
{
  if ((packed_size == 0U) || (packed_size > sizeof s_packed) || (transfer->chunk_bytes == 0U)) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t err    = transfer->storage.begin(transfer->storage.ctx, destination);
  bool      active = err == k_ra8_ok;
  if (err == k_ra8_ok) {
    err = transfer->sha256.init(transfer->sha256.ctx);
  }
  uint64_t offset = 0U;
  while ((err == k_ra8_ok) && (offset < packed_size)) {
    uint64_t remaining = packed_size - offset;
    uint16_t chunk     = transfer->chunk_bytes;
    if (remaining < (uint64_t)chunk) {
      chunk = (uint16_t)remaining;
    }
    err = internal_publish_fragment(transfer, offset, chunk);
    offset += (err == k_ra8_ok) ? chunk : 0U;
  }
  uint8_t digest[k_ra8_mdl_sha256_bytes] = {};
  if (err == k_ra8_ok) {
    err = transfer->sha256.final(transfer->sha256.ctx, digest);
  }
  if (err == k_ra8_ok) {
    err = transfer->storage.validate(transfer->storage.ctx, packed_size, digest);
  }
  if (err == k_ra8_ok) {
    err = transfer->storage.commit(transfer->storage.ctx);
    if (err == k_ra8_ok) {
      active = false;
    }
  }
  if ((err != k_ra8_ok) && active) {
    (void)transfer->storage.abort(transfer->storage.ctx);
  }
  return err;
}

/**
 * @brief Prove every injected callback required by image mode is present.
 * @param[in] transfer Candidate transfer configuration.
 * @return Callback validation status.
 * @retval k_ra8_ok Every required storage and SHA callback is non-null.
 * @retval k_ra8_err_null_ptr A required configuration or callback is null.
 * @pre @p transfer may be null.
 * @pre No callback is invoked by this check.
 * @post No caller or application state is modified.
 * @post Success permits both source transfer and strict local publication.
 * @note Image mode requires validation because it claims RABOOK output.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_check_transfer(const ra8_mdl_transfer_config_t* transfer)
{
  if (transfer == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((transfer->storage.begin == nullptr) || (transfer->storage.write == nullptr) ||
      (transfer->storage.validate == nullptr) || (transfer->storage.commit == nullptr) ||
      (transfer->storage.abort == nullptr) || (transfer->storage.ctx == nullptr) ||
      (transfer->sha256.init == nullptr) || (transfer->sha256.update == nullptr) ||
      (transfer->sha256.final == nullptr) || (transfer->sha256.ctx == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  return k_ra8_ok;
}

/* See the internal header for the documented contract. */
RA8_PRIV ra8_err_t priv_media_download_image_run(ra8_c6link_t*                    link,
                                                 const char*                      url,
                                                 const char*                      destination,
                                                 const ra8_mdl_transfer_config_t* transfer)
{
  if ((link == nullptr) || (url == nullptr) || (destination == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  ra8_err_t                 err             = internal_check_transfer(transfer);
  ra8_mdl_transfer_config_t source_transfer = {};
  if (err == k_ra8_ok) {
    source_transfer = *transfer;
    err             = ra8_mdl_storage_ram_init(&s_source_storage,
                                               &source_transfer.storage,
                                               s_source,
                                               sizeof s_source);
  }
  if (err == k_ra8_ok) {
    ra8_mdl_transfer_result_t result = {};
    err = ra8_c6link_mdl_transfer(link, url, "source-image", &source_transfer, &result);
  }
  uint64_t packed_size = 0U;
  if (err == k_ra8_ok) {
    err = internal_format(&packed_size);
  }
  if (err == k_ra8_ok) {
    err = internal_publish(transfer, packed_size, destination);
  }
  return err;
}
