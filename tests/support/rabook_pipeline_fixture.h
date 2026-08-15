/**
 * @file rabook_pipeline_fixture.h
 * @brief Shared test fixture for the EPUB -> RABOOK1 pipeline suites.
 *
 * @details
 * Header-only fixture extracted from test_ra8_rabook_pipeline.c to keep the
 * test translation units under the repository file-size cap. Provides the
 * in-memory block-device FS backend, the static compile pools, the
 * synthetic-EPUB zip builders (text-only, cover, raster, tall, css, absent
 * image, nav TOC variants), and the shared no-op log sink. Every function
 * is `static inline`, so each including test binary gets an independent
 * copy and unused builders compile away; the static pools are per-binary.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_book.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_rabook_compile.h"
#include "ra8_rabook_pipeline.h"
#include "unity_minimal.h"

/**
 * @enum rabook_pipeline_fixture_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_rabook_pipeline_fixture_put_u32_le_10 =
    10, /**< BMP file-header offset of the pixel-data offset field. */
  k_rabook_pipeline_fixture_put_u32_le_14 =
    14, /**< BMP file-header offset of the DIB header size field. */
  /** BMP DIB offset of the image width field. */
  k_rabook_pipeline_fixture_put_u32_le_18 = 18,
  k_rabook_pipeline_fixture_put_u32_le_22 =
    22, /**< BMP DIB offset of the image height field (bottom-up). */
  /** BMP DIB offset of the raw image size field. */
  k_rabook_pipeline_fixture_put_u32_le_34 = 34,
  k_rabook_pipeline_fixture_put_u32_le_40 =
    40U, /**< BITMAPINFOHEADER size in bytes, written as the DIB header size. */
  k_rabook_pipeline_fixture_le32_hi_shift =
    24U,                                   /**< Top-byte shift of a little-endian 32-bit field. */
  k_rabook_pipeline_fixture_bmp_bpp = 24U, /**< BMP bits per pixel: 24bpp BGR, no palette.      */
  k_rabook_pipeline_fixture_v_ff =
    0xFFU, /**< Low-byte mask while serialising a little-endian 32-bit field. */
  /** Minimal in-test BMP buffer capacity. */
  k_rabook_pipeline_fixture_small_bmp_cap = 128,
  k_rabook_pipeline_fixture_bmp_off_planes =
    26, /**< BMP DIB header offset of the colour-plane count. */
  k_rabook_pipeline_fixture_bmp_off_bpp =
    28, /**< BMP DIB header offset of the bits-per-pixel field. */
} rabook_pipeline_fixture_uint8_const_t;

/**
 * @enum rabook_pipeline_fixture_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_rabook_pipeline_fixture_u_1024 = 1024U, /**< Bytes per KiB, sizing the BMP staging buffer. */
} rabook_pipeline_fixture_uint16_const_t;

/* -------------------------------------------------------------------------- */
/* Sizing constants */
/* -------------------------------------------------------------------------- */

/**
 * @enum pipe_disk_t
 * @brief RAM block-device geometry (4 MiB -> auto-detects FAT16).
 */
typedef enum : uint32_t {
  k_disk_block_size = 512U,  /**< Bytes per sector (the only size ra8_fs accepts). */
  k_disk_blocks     = 8192U, /**< 4 MiB volume -> count_of_clusters lands FAT16.   */
} pipe_disk_t;

/**
 * @enum pipe_image_dim_t
 * @brief Image-fixture dimensions for the opt-in downscale-clamp tests.
 */
typedef enum : uint16_t {
  k_pl_clamp_edge  = 1600U, /**< Opt-in long-edge clamp the clamp tests apply.   */
  k_pl_big_edge    = 1601U, /**< Oversized fixture edge (one past the clamp).    */
  k_pl_narrow_edge = 1U,    /**< Short edge of the tall cover (stays unchanged). */
} pipe_image_dim_t;

/**
 * @enum pipe_cap_t
 * @brief Builder-arena capacities for the compiled book.
 */
typedef enum : uint32_t {
  k_chapter_cap = 8U,           /**< Max chapters.                                 */
  k_node_cap    = 512U,         /**< Max DOM nodes (real-book chapters: 338).      */
  k_attr_cap    = 128U,         /**< Max attribute records (real-book: 47).        */
  k_style_cap   = 4U,           /**< Max stylesheets.                              */
  k_image_cap   = 8U,           /**< Max image descriptors.                        */
  k_string_cap  = 96U * 1024U,  /**< String-pool capacity (real-book pool ~58 KB). */
  k_imgpool_cap = 256U * 1024U, /**< Image-pool capacity (bytes).                  */
  k_out_cap     = 128U * 1024U, /**< Output-blob capacity (real-book blob ~67 KB). */
} pipe_cap_t;

/**
 * @enum pipe_scratch_t
 * @brief Pipeline scratch-buffer capacities.
 */
typedef enum : uint32_t {
  k_xhtml_cap   = 64U * 1024U,  /**< Chapter XHTML scratch (real-book ch ~28 KB). */
  k_imgraw_cap  = 64U * 1024U,  /**< Raw cover/image byte scratch (bytes).        */
  k_arena_cap   = 256U * 1024U, /**< stb_image bump-arena scratch (bytes).        */
  k_graypix_cap = 64U * 1024U,  /**< Intermediate gray downscale (pixels).        */
  k_css_cap     = 16U * 1024U,  /**< Stylesheet load scratch (bytes).             */
  k_epub_cap    = 16U * 1024U,  /**< In-memory ZIP build buffer (bytes).          */
  k_read_cap    = 128U * 1024U, /**< .rabook read-back buffer (real-book ~67 KB). */
} pipe_scratch_t;

/* -------------------------------------------------------------------------- */
/* Static storage */
/* -------------------------------------------------------------------------- */

static ra8_book_chapter_t    s_chapters[k_chapter_cap];
static ra8_book_node_t       s_nodes[k_node_cap];
static ra8_book_attr_t       s_attrs[k_attr_cap];
static ra8_book_stylesheet_t s_styles[k_style_cap];
static ra8_book_image_t      s_images[k_image_cap];
static char                  s_strpool[k_string_cap];
static uint8_t               s_imgpool[k_imgpool_cap];
static uint8_t               s_out[k_out_cap];

static uint8_t                    s_xhtml[k_xhtml_cap];
static ra8_rabook_xml_workspace_t s_xml_workspace;
static uint8_t                    s_image_raw[k_imgraw_cap];
static uint8_t                    s_img_scratch[k_arena_cap];
static uint8_t                    s_gray[k_graypix_cap];
static char                       s_css[k_css_cap];

static uint8_t s_epub[k_epub_cap];
static size_t  s_epub_len;
static uint8_t s_readback[k_read_cap];

typedef struct {
  uint8_t* bytes;       /**< Bytes.       */
  uint32_t block_count; /**< Block count. */
} mem_disk_t;

static mem_disk_t s_disk = {};

/* -------------------------------------------------------------------------- */
/* RAM block backend (4 MiB -> FAT16 via ra8_fs_format) */
/* -------------------------------------------------------------------------- */

/** @brief Provide the file-local mem read test helper. @details Implements the mem read fixture operation used only by this focused test executable. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[in] lba Fixture argument governed by the exercised interface contract. @param[in] count Fixture argument governed by the exercised interface contract. @param[out] buf Fixture argument governed by the exercised interface contract. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static inline ra8_err_t
internal_mem_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  if (lba + count > disk->block_count) {
    return k_ra8_err_out_of_range;
  }
  size_t off = (size_t)lba * (size_t)k_disk_block_size;
  size_t len = (size_t)count * (size_t)k_disk_block_size;
  memcpy(buf, &disk->bytes[off], len);
  return k_ra8_ok;
}

/** @brief Provide the file-local mem write test helper. @details Implements the mem write fixture operation used only by this focused test executable. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[in] lba Fixture argument governed by the exercised interface contract. @param[in] count Fixture argument governed by the exercised interface contract. @param[in] buf Fixture argument governed by the exercised interface contract. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static inline ra8_err_t
internal_mem_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  if (lba + count > disk->block_count) {
    return k_ra8_err_out_of_range;
  }
  size_t off = (size_t)lba * (size_t)k_disk_block_size;
  size_t len = (size_t)count * (size_t)k_disk_block_size;
  memcpy(&disk->bytes[off], buf, len);
  return k_ra8_ok;
}

/** @brief Provide the file-local mem capacity test helper. @details Implements the mem capacity fixture operation used only by this focused test executable. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[out] block_count Fixture argument governed by the exercised interface contract. @param[out] block_size Fixture argument governed by the exercised interface contract. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static inline ra8_err_t
internal_mem_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  *block_count     = disk->block_count;
  *block_size      = (uint32_t)k_disk_block_size;
  return k_ra8_ok;
}

static const ra8_fs_backend_t s_backend = {
  .read_block   = internal_mem_read,
  .write_block  = internal_mem_write,
  .get_capacity = internal_mem_capacity,
  .ctx          = &s_disk,
};

/* -------------------------------------------------------------------------- */
/* EPUB fixture (text-only, optional undecodable cover) */
/* -------------------------------------------------------------------------- */

static const char* const s_mimetype = "application/epub+zip";
static const char* const s_container =
  "<?xml version=\"1.0\"?><container version=\"1.0\" "
  "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
  "<rootfile full-path=\"OEBPS/content.opf\" "
  "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";

static const char* const s_opf_no_cover =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Pipeline Title</dc:title><dc:creator>Pipeline Author</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:test:pipe</dc:identifier></metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
  "<spine><itemref idref=\"c1\"/></spine></package>";

static const char* const s_opf_with_cover =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Pipeline Title</dc:title><dc:creator>Pipeline Author</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:test:pipe</dc:identifier></metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"cov\" href=\"cover.bin\" media-type=\"image/png\" properties=\"cover-image\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

static const char* const s_chapter_xhtml = "<html><body><p>Hello pipeline.</p></body></html>";

/* Not a valid image: stb_image rejects it, so a present cover fails to decode. */
static const uint8_t s_garbage_cover[16] = {
  0xDEU,
  0xADU,
  0xBEU,
  0xEFU,
  0x00U,
  0x01U,
  0x02U,
  0x03U,
  0x04U,
  0x05U,
  0x06U,
  0x07U,
  0x08U,
  0x09U,
  0x0AU,
  0x0BU,
};

/**
 * @brief Build a tiny text-only `.epub` into @p s_epub.
 * @details Writes the spec-mandated `mimetype`, the container, an OPF (with or
 *          without a cover-image manifest entry), one XHTML chapter, and -- when
 *          @p with_cover -- a deliberately undecodable `cover.bin` blob.
 * @param[in] with_cover True to add a present-but-undecodable cover image.
 * @pre miniz is available (host build).
 * @pre @p s_epub is large enough for the finalized archive.
 * @post @p s_epub holds the finalized ZIP and @p s_epub_len its length.
 * @post No filesystem state is touched.
 * @note Not thread-safe (writes file-scope fixture buffers). @since Version 0.1.0 */
RA8_INTERNAL static inline void internal_build_epub(bool with_cover)
{
  const char* opf = with_cover ? s_opf_with_cover : s_opf_no_cover;

  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_epub_cap) == MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "mimetype", s_mimetype, strlen(s_mimetype), MZ_NO_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "META-INF/container.xml",
                                    s_container,
                                    strlen(s_container),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/content.opf", opf, strlen(opf), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/c1.xhtml",
                                    s_chapter_xhtml,
                                    strlen(s_chapter_xhtml),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  if (with_cover) {
    TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                      "OEBPS/cover.bin",
                                      s_garbage_cover,
                                      sizeof(s_garbage_cover),
                                      MZ_NO_COMPRESSION) == MZ_TRUE);
  }

  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT((heap != nullptr) && (hsz > 0U) && (hsz <= sizeof(s_epub)));
  memcpy(s_epub, heap, hsz);
  s_epub_len = hsz;
  mz_free(heap);
  mz_zip_writer_end(&zip);
}

/**
 * @brief Format a fresh FAT16 RAM volume and return a mounted handle.
 * @return Mounted volume handle.
 * @pre The RAM backend descriptor @p s_backend is initialised.
 * @pre Any prior volume has been unmounted.
 * @post A formatted, mounted FAT16 volume backs @p s_disk.
 * @post @p s_disk.bytes owns a fresh zeroed backing store.
 * @note Not thread-safe.
 */
RA8_INTERNAL static inline ra8_fs_mount_t* internal_fresh_volume(void)
{
  free(s_disk.bytes);
  s_disk.block_count = (uint32_t)k_disk_blocks;
  s_disk.bytes       = (uint8_t*)calloc((size_t)k_disk_blocks, (size_t)k_disk_block_size);
  TEST_ASSERT(s_disk.bytes != nullptr);

  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "RABOOK";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));

  ra8_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mount));
  return mount;
}

/**
 * @brief Unmount @p mount and release the RAM backing store.
 * @param[in,out] mount Mounted volume to release.
 * @pre @p mount is a live mount returned by @ref internal_fresh_volume.
 * @pre Every open file on @p mount has been closed.
 * @post @p mount is unmounted and @p s_disk.bytes is freed.
 * @post @p s_disk.bytes is reset to NULL.
 * @note Not thread-safe. @details Implements the teardown fixture operation used only by this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static inline void internal_teardown(ra8_fs_mount_t* mount)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
}

/**
 * @brief Populate a buffers + scratch pair over the file-scope arenas.
 * @param[out] bufs  Receives the builder-arena view (non-NULL).
 * @param[out] scr   Receives the pipeline scratch view (non-NULL).
 * @param[out] arena Receives the stb_image bump arena (non-NULL).
 * @pre The output pointers are non-NULL.
 * @pre The file-scope arenas are defined (always true at TU scope).
 * @post @p bufs, @p scr and @p arena reference the static storage.
 * @post No global state beyond the outputs is mutated.
 * @note Not thread-safe (returns views over shared file-scope arenas). @details Implements the make views fixture operation used only by this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static inline void internal_make_views(ra8_rabook_buffers_t*          bufs,
                                                    ra8_rabook_pipeline_scratch_t* scr,
                                                    ra8_img_arena_t*               arena)
{
  *bufs = (ra8_rabook_buffers_t){
    .chapters       = s_chapters,
    .chapter_cap    = (uint32_t)k_chapter_cap,
    .nodes          = s_nodes,
    .node_cap       = (uint32_t)k_node_cap,
    .attrs          = s_attrs,
    .attr_cap       = (uint32_t)k_attr_cap,
    .stylesheets    = s_styles,
    .stylesheet_cap = (uint32_t)k_style_cap,
    .images         = s_images,
    .image_cap      = (uint32_t)k_image_cap,
    .string_pool    = s_strpool,
    .string_cap     = (uint32_t)k_string_cap,
    .image_pool     = s_imgpool,
    .image_pool_cap = (uint32_t)k_imgpool_cap,
    .out            = s_out,
    .out_cap        = (uint32_t)k_out_cap,
  };
  *arena = (ra8_img_arena_t){s_img_scratch, sizeof(s_img_scratch), 0U, 0U};
  *scr   = (ra8_rabook_pipeline_scratch_t){
    .xhtml         = s_xhtml,
    .xhtml_cap     = sizeof(s_xhtml),
    .image_raw     = s_image_raw,
    .image_cap     = sizeof(s_image_raw),
    .img_arena     = arena,
    .gray          = s_gray,
    .gray_cap      = (uint32_t)k_graypix_cap,
    .css           = s_css,
    .css_cap       = sizeof(s_css),
    .xml_workspace = &s_xml_workspace,
  };
}

/* -------------------------------------------------------------------------- */
/* Extended fixtures (branch-coverage drivers) */
/* -------------------------------------------------------------------------- */

/**
 * @struct pipe_zip_entry_t
 * @brief One archive member fed to @ref internal_build_zip.
 * @details Mirrors the arguments of @c mz_zip_writer_add_mem so a fixture can
 *          be declared as a flat array of members instead of a hand-unrolled
 *          sequence of writer calls.
 */
typedef struct {
  const char* name;  /**< Archive path, e.g. "OEBPS/c1.xhtml".         */
  const void* data;  /**< Member bytes (borrowed for the call only).   */
  size_t      len;   /**< Member byte count.                           */
  bool        store; /**< true: STORE (no compression, e.g. mimetype). */
} pipe_zip_entry_t;

/**
 * @brief Store a 32-bit value little-endian into a byte buffer.
 * @param[out] p Destination (>= 4 writable bytes).
 * @param[in]  v Value to store.
 * @pre @p p is non-NULL with at least four writable bytes.
 * @pre The caller owns @p p for the duration of the call.
 * @post @p p[0..3] hold @p v least-significant byte first.
 * @post No other memory is touched.
 * @note Not thread-safe (writes through the caller's pointer). @details Implements the put u32 le fixture operation used only by this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static inline void internal_put_u32_le(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & k_rabook_pipeline_fixture_v_ff);
  p[1] = (uint8_t)((v >> 8U) & k_rabook_pipeline_fixture_v_ff);
  p[2] = (uint8_t)((v >> 16U) & k_rabook_pipeline_fixture_v_ff);
  p[3] = (uint8_t)((v >> k_rabook_pipeline_fixture_le32_hi_shift) & k_rabook_pipeline_fixture_v_ff);
}

/**
 * @brief Synthesise a solid-gray, uncompressed 24-bpp Windows BMP.
 * @details Emits a 14-byte BITMAPFILEHEADER + 40-byte BITMAPINFOHEADER followed
 *          by bottom-up BGR rows padded to a 4-byte stride -- the minimal form
 *          stb_image decodes. Used to drive the raster transcode path (decode ->
 *          gray4) that the text-only fixtures never reach.
 * @param[out] out  Destination buffer (>= the returned length).
 * @param[in]  w    Image width in pixels (> 0).
 * @param[in]  h    Image height in pixels (> 0).
 * @param[in]  gray Solid gray level written to every B/G/R channel.
 * @return Total BMP byte length written to @p out.
 * @pre @p out holds at least 54 + stride * @p h bytes.
 * @pre @p w and @p h are non-zero.
 * @post @p out holds a decodable 24-bpp BMP of @p w x @p h.
 * @post Every pixel channel equals @p gray.
 * @note Not thread-safe. @retval value The computed fixture value for the supplied inputs. @since Version 0.1.0 */
RA8_INTERNAL static inline size_t
internal_make_bmp(uint8_t* out, uint16_t w, uint16_t h, uint8_t gray)
{
  const uint32_t hdr   = 54U;
  const uint32_t row   = ((((uint32_t)w * 3U) + 3U) & ~3U);
  const uint32_t data  = row * (uint32_t)h;
  const uint32_t total = hdr + data;
  memset(out, 0, (size_t)total);
  out[0] = (uint8_t)'B';
  out[1] = (uint8_t)'M';
  internal_put_u32_le(out + 2, total);                                     /* file size         */
  internal_put_u32_le(out + k_rabook_pipeline_fixture_put_u32_le_10, hdr); /* pixel data offset */
  internal_put_u32_le(out + k_rabook_pipeline_fixture_put_u32_le_14,
                      k_rabook_pipeline_fixture_put_u32_le_40);          /* DIB header size    */
  internal_put_u32_le(out + k_rabook_pipeline_fixture_put_u32_le_18, w); /* width              */
  internal_put_u32_le(out + k_rabook_pipeline_fixture_put_u32_le_22, h); /* height (bottom-up) */
  out[k_rabook_pipeline_fixture_bmp_off_planes] = 1U;
  out[k_rabook_pipeline_fixture_bmp_off_bpp]    = k_rabook_pipeline_fixture_bmp_bpp;
  internal_put_u32_le(out + k_rabook_pipeline_fixture_put_u32_le_34, data); /* raw image size */
  for (uint32_t y = 0U; y < (uint32_t)h; y++) {
    uint8_t* px = out + hdr + ((size_t)y * row);
    for (uint32_t x = 0U; x < (uint32_t)w; x++) {
      px[(x * 3U) + 0U] = gray;
      px[(x * 3U) + 1U] = gray;
      px[(x * 3U) + 2U] = gray;
    }
  }
  return (size_t)total;
}

/**
 * @brief Assemble an in-memory `.epub` ZIP from @p entries into @p s_epub.
 * @details The generic version of @ref internal_build_epub: each fixture builder below
 *          declares its members as a @ref pipe_zip_entry_t array and hands it
 *          here. Entry 0 is by spec the stored `mimetype`.
 * @param[in] entries Member list (non-NULL).
 * @param[in] n       Number of members.
 * @pre @p entries has @p n valid members and @p entries[0] is the mimetype.
 * @pre The finalized archive fits in @p s_epub.
 * @post @p s_epub holds the ZIP and @p s_epub_len its length.
 * @post No filesystem state is touched.
 * @note Not thread-safe (writes the file-scope fixture buffers). @since Version 0.1.0 */
RA8_INTERNAL static inline void internal_build_zip(const pipe_zip_entry_t* entries, size_t n)
{
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_epub_cap) == MZ_TRUE);
  for (size_t i = 0U; i < n; i++) {
    const mz_uint level =
      entries[i].store ? (mz_uint)MZ_NO_COMPRESSION : (mz_uint)MZ_DEFAULT_COMPRESSION;
    TEST_ASSERT(
      mz_zip_writer_add_mem(&zip, entries[i].name, entries[i].data, entries[i].len, level) ==
      MZ_TRUE);
  }
  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT((heap != nullptr) && (hsz > 0U) && (hsz <= sizeof(s_epub)));
  memcpy(s_epub, heap, hsz);
  s_epub_len = hsz;
  mz_free(heap);
  mz_zip_writer_end(&zip);
}

/** @brief OPF declaring one chapter plus a small + a large BMP image. */
static const char* const s_opf_raster =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Pipeline Title</dc:title><dc:creator>Pipeline Author</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:test:pipe</dc:identifier></metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"small\" href=\"small.bmp\" media-type=\"image/bmp\" properties=\"cover-image\"/>"
  "<item id=\"big\" href=\"big.bmp\" media-type=\"image/bmp\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/** @brief OPF declaring one chapter plus a single tall (portrait) BMP cover. */
static const char* const s_opf_tall =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Pipeline Title</dc:title><dc:creator>Pipeline Author</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:test:pipe</dc:identifier></metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"tall\" href=\"tall.bmp\" media-type=\"image/bmp\" properties=\"cover-image\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/** @brief OPF declaring one chapter plus a single `text/css` stylesheet item. */
static const char* const s_opf_css =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Pipeline Title</dc:title><dc:creator>Pipeline Author</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:test:pipe</dc:identifier></metadata>"
  "<manifest><item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/** @brief OPF declaring one chapter plus an image item with no archive bytes. */
static const char* const s_opf_img_absent =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Pipeline Title</dc:title><dc:creator>Pipeline Author</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:test:pipe</dc:identifier></metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"img\" href=\"missing.png\" media-type=\"image/png\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/** @brief OPF + nav declaring two chapters with a mapped table of contents. */
static const char* const s_opf_toc =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Pipeline Title</dc:title><dc:creator>Pipeline Author</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:test:pipe</dc:identifier></metadata>"
  "<manifest><item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" "
  "properties=\"nav\"/>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest><spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine></package>";

/** @brief Nav mapping two entries onto the spine, led by an unresolvable entry.
 *  @details The leading `<a href="orphan.xhtml">` points at no spine chapter, so
 *           ra8_epub_toc_entry_to_chapter() fails on it -- driving the
 *           first-condition-false leg of internal_chapter_title's compound guard. */
static const char* const s_nav_xhtml =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
  "<head><title>Contents</title></head><body>"
  "<nav epub:type=\"toc\"><ol>"
  "<li><a href=\"orphan.xhtml\">Orphan Entry</a></li>"
  "<li><a href=\"c1.xhtml#start\">Chapter One</a></li>"
  "<li><a href=\"c2.xhtml\">Chapter Two</a></li>"
  "</ol></nav></body></html>";

static const char* const s_chapter2_xhtml = "<html><body><p>Second chapter.</p></body></html>";
/** @brief Stylesheet body interned by the CSS-present fixture. */
static const char* const s_style_css = ".lead { color: #C00000; } p { margin: 0; }";
/** @brief Chapter body with no XML root element (forces a parse miss). */
static const char* const s_chapter_plain = "just plain text with no markup elements at all";

/** @brief Backing store for synthesised BMP fixtures (large image fits). */
static uint8_t s_bmp[8U * k_rabook_pipeline_fixture_u_1024];

/**
 * @brief Build a text-chapter `.epub` whose body is @p chapter (no images/CSS).
 * @param[in] chapter NUL-terminated chapter XHTML body.
 * @pre @p chapter is non-NULL.
 * @pre @p s_epub is large enough for the finalized archive.
 * @post @p s_epub holds the ZIP and @p s_epub_len its length.
 * @post No filesystem state is touched.
 * @note Not thread-safe. @details Implements the build epub chapter fixture operation used only by this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static inline void internal_build_epub_chapter(const char* chapter)
{
  const pipe_zip_entry_t entries[] = {
    {"mimetype", s_mimetype, strlen(s_mimetype), true},
    {"META-INF/container.xml", s_container, strlen(s_container), false},
    {"OEBPS/content.opf", s_opf_no_cover, strlen(s_opf_no_cover), false},
    {"OEBPS/c1.xhtml", chapter, strlen(chapter), false},
  };
  internal_build_zip(entries, sizeof(entries) / sizeof(entries[0]));
}

/**
 * @brief Build an `.epub` carrying a small (2x2) and a large (1601x1) BMP image.
 * @pre @p s_epub and @p s_bmp are large enough for the fixtures.
 * @pre miniz is available (host build).
 * @post @p s_epub holds the ZIP; the small image is the declared cover.
 * @post No filesystem state is touched.
 * @note Not thread-safe. @details Implements the build epub raster fixture operation used only by this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static inline void internal_build_epub_raster(void)
{
  static uint8_t local_small_bmp[k_rabook_pipeline_fixture_small_bmp_cap];
  const size_t   small_len = internal_make_bmp(local_small_bmp, 2U, 2U, 0x80U);
  const size_t   big_len   = internal_make_bmp(s_bmp, (uint16_t)k_pl_big_edge, 1U, 0x80U);

  const pipe_zip_entry_t entries[] = {
    {"mimetype", s_mimetype, strlen(s_mimetype), true},
    {"META-INF/container.xml", s_container, strlen(s_container), false},
    {"OEBPS/content.opf", s_opf_raster, strlen(s_opf_raster), false},
    {"OEBPS/c1.xhtml", s_chapter_xhtml, strlen(s_chapter_xhtml), false},
    {"OEBPS/small.bmp", local_small_bmp, small_len, false},
    {"OEBPS/big.bmp", s_bmp, big_len, false},
  };
  internal_build_zip(entries, sizeof(entries) / sizeof(entries[0]));
}

/**
 * @brief Build an `.epub` carrying a single tall (1 x 1601) BMP cover image.
 * @details The cover's short edge (width 1) stays below the clamp while its long
 *          edge (height 1601) is reduced to 1600, so internal_downscale_if_needed sees
 *          ow == sw (1 == 1) but oh != sh (1600 != 1601) -- the height-varying
 *          MC/DC leg of `if (ow == sw && oh == sh)`.
 * @pre @p s_epub and @p s_bmp are large enough for the fixtures.
 * @post @p s_epub holds the ZIP; the tall image is the declared cover.
 * @post No filesystem state is touched.
 * @note Not thread-safe. @pre Fixed-capacity fixture storage required by this operation is available. @since Version 0.1.0 */
RA8_INTERNAL static inline void internal_build_epub_tall(void)
{
  const size_t tall_len =
    internal_make_bmp(s_bmp, (uint16_t)k_pl_narrow_edge, (uint16_t)k_pl_big_edge, 0x80U);

  const pipe_zip_entry_t entries[] = {
    {"mimetype", s_mimetype, strlen(s_mimetype), true},
    {"META-INF/container.xml", s_container, strlen(s_container), false},
    {"OEBPS/content.opf", s_opf_tall, strlen(s_opf_tall), false},
    {"OEBPS/c1.xhtml", s_chapter_xhtml, strlen(s_chapter_xhtml), false},
    {"OEBPS/tall.bmp", s_bmp, tall_len, false},
  };
  internal_build_zip(entries, sizeof(entries) / sizeof(entries[0]));
}

/**
 * @brief Build an `.epub` with a `text/css` item, present or absent in the ZIP.
 * @param[in] present true to store the stylesheet bytes; false to declare the
 *                    item in the manifest but omit it from the archive.
 * @pre @p s_epub is large enough for the finalized archive.
 * @post @p s_epub holds the ZIP and @p s_epub_len its length.
 * @post No filesystem state is touched.
 * @note Not thread-safe. @details Implements the build epub css fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @since Version 0.1.0 */
RA8_INTERNAL static inline void internal_build_epub_css(bool present)
{
  pipe_zip_entry_t entries[] = {
    {"mimetype", s_mimetype, strlen(s_mimetype), true},
    {"META-INF/container.xml", s_container, strlen(s_container), false},
    {"OEBPS/content.opf", s_opf_css, strlen(s_opf_css), false},
    {"OEBPS/c1.xhtml", s_chapter_xhtml, strlen(s_chapter_xhtml), false},
    {"OEBPS/style.css", s_style_css, strlen(s_style_css), false},
  };
  const size_t base = 4U; /* without the trailing style.css member */
  internal_build_zip(entries, present ? (sizeof(entries) / sizeof(entries[0])) : base);
}

/**
 * @brief Build an `.epub` declaring an image item with no archive bytes.
 * @pre @p s_epub is large enough for the finalized archive.
 * @post @p s_epub holds the ZIP and @p s_epub_len its length.
 * @post No filesystem state is touched.
 * @note Not thread-safe. @details Implements the build epub image absent fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @since Version 0.1.0 */
RA8_INTERNAL static inline void internal_build_epub_image_absent(void)
{
  const pipe_zip_entry_t entries[] = {
    {"mimetype", s_mimetype, strlen(s_mimetype), true},
    {"META-INF/container.xml", s_container, strlen(s_container), false},
    {"OEBPS/content.opf", s_opf_img_absent, strlen(s_opf_img_absent), false},
    {"OEBPS/c1.xhtml", s_chapter_xhtml, strlen(s_chapter_xhtml), false},
  };
  internal_build_zip(entries, sizeof(entries) / sizeof(entries[0]));
}

/**
 * @brief Build a two-chapter `.epub` with a nav table of contents.
 * @pre @p s_epub is large enough for the finalized archive.
 * @post @p s_epub holds the ZIP; both spine chapters carry a TOC title.
 * @post No filesystem state is touched.
 * @note Not thread-safe. @details Implements the build epub toc fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @since Version 0.1.0 */
RA8_INTERNAL static inline void internal_build_epub_toc(void)
{
  const pipe_zip_entry_t entries[] = {
    {"mimetype", s_mimetype, strlen(s_mimetype), true},
    {"META-INF/container.xml", s_container, strlen(s_container), false},
    {"OEBPS/content.opf", s_opf_toc, strlen(s_opf_toc), false},
    {"OEBPS/nav.xhtml", s_nav_xhtml, strlen(s_nav_xhtml), false},
    {"OEBPS/c1.xhtml", s_chapter_xhtml, strlen(s_chapter_xhtml), false},
    {"OEBPS/c2.xhtml", s_chapter2_xhtml, strlen(s_chapter2_xhtml), false},
  };
  internal_build_zip(entries, sizeof(entries) / sizeof(entries[0]));
}

/* -------------------------------------------------------------------------- */
/* Tests */
/* -------------------------------------------------------------------------- */

/**
 * @brief No-op log byte sink so the logger never pokes ITM MMIO on the host.
 * @param[in] ctx  Unused sink context.
 * @param[in] byte Unused log byte.
 * @pre Installed in main() before any test runs.
 * @pre Never called from interrupt context (host build).
 * @post No global state is mutated.
 * @post The byte is discarded.
 * @note Not thread-safe (host single-thread test driver). @details Implements the log sink fixture operation used only by this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static inline void internal_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}
