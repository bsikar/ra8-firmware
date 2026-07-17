/**
 * @file test_ra8_rabook_pipeline.c
 * @brief End-to-end EPUB -> RABOOK1 pipeline test (ra8_rabook_pipeline, #149).
 *
 * @details
 * Exercises @ref ra8_rabook_compile_from_epub -- the only entry point that wires
 * the metadata, cover, chapter and finalize stages together -- against a tiny
 * synthetic book:
 *
 *  - A text-only `.epub` is assembled in memory with miniz (the build pattern
 *    from tests/test_ra8_epub.c) and opened with @ref ra8_epub_open.
 *  - A real FAT16 volume is formatted over a RAM block backend (the RAM-disk
 *    pattern from tests/test_ra8_epub_fs.c) so the pipeline's
 *    @ref ra8_fs_write_file lands on a genuine filesystem.
 *  - Happy path (no cover): the compile succeeds, the written `.rabook` passes
 *    @ref ra8_book_validate, chapter_count / title / author read back as built,
 *    string-pool offset 0 is the reserved "", and the absent cover leaves the
 *    cover-image index nil.
 *  - Skip path (present-but-undecodable cover): a manifest cover whose bytes are
 *    NOT a valid image makes stb_image fail to decode; matching the desktop
 *    epub_compile.py (try/except pass), the image loop skips it, so the compile
 *    still succeeds and publishes a valid coverless book (cover index nil).
 *  - Byte-identity: the on-device emit equals the desktop golden (parity case).
 *  - Skip-images byte-identity: the same fixture compiled with
 *    @ref ra8_rabook_pipeline_scratch_t::skip_images set equals the desktop
 *    `--no-images` golden (text/CSS-only, the SVG cover dropped).
 *  - Real-book byte-identity (#151): real Standard Ebooks Walden chapters
 *    (carrying the significant `</abbr> <abbr>` inter-element whitespace)
 *    compiled with skip_images equal the desktop `--no-images` golden -- the
 *    proof the device preserves inline whitespace on real content.
 *
 * The branch-coverage tests that follow drive the legs the parity / happy paths
 * never reach: the raster transcode (decode -> downscale -> gray4 encode) over
 * synthesised BMP covers, the TOC-title scan, the stylesheet / image / chapter
 * error legs, the no-filesystem buffer entry point, and every null / bad-arena
 * guard.
 *
 * @par MC/DC:
 * The pipeline's only compound decision is `if (ow == sw && oh == sh)` in
 * s_downscale_if_needed; @ref test_pipeline_raster_images_transcoded drives the
 * true arm (a 2x2 image copied in place) and the width-varying false arm (a
 * 1601x1 image downscaled), and @ref test_pipeline_gray_scratch_too_small drives
 * the height-varying leg, giving the N+1 = 3 vectors. All other branches touched
 * here (image add vs skip, TOC match, and the per-stage error propagation) are
 * single-condition guards, each covered by driving its one condition true or
 * false; the per-test `@par MC/DC:` blocks name the vectors.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB_Compiler] {World: NS}
 *
 * @since Version 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "ra8_book.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_img_arena.h"
#include "ra8_log.h"
#include "ra8_rabook_compile.h"
#include "ra8_rabook_pipeline.h"
#include "ra8_reflow_image.h"
#include "rabook_parity_fixture.h"
#include "rabook_realbook_fixture.h"
#include "unity_minimal.h"

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

static uint8_t s_xhtml[k_xhtml_cap];
static uint8_t s_image_raw[k_imgraw_cap];
static uint8_t s_img_scratch[k_arena_cap];
static uint8_t s_gray[k_graypix_cap];
static char    s_css[k_css_cap];

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

static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
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

static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
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

static ra8_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  *block_count     = disk->block_count;
  *block_size      = (uint32_t)k_disk_block_size;
  return k_ra8_ok;
}

static const ra8_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

/* -------------------------------------------------------------------------- */
/* EPUB fixture (text-only, optional undecodable cover) */
/* -------------------------------------------------------------------------- */

static const char* const k_mimetype = "application/epub+zip";
static const char* const k_container =
  "<?xml version=\"1.0\"?><container version=\"1.0\" "
  "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
  "<rootfile full-path=\"OEBPS/content.opf\" "
  "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";

static const char* const k_opf_no_cover =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Pipeline Title</dc:title><dc:creator>Pipeline Author</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:test:pipe</dc:identifier></metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
  "<spine><itemref idref=\"c1\"/></spine></package>";

static const char* const k_opf_with_cover =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Pipeline Title</dc:title><dc:creator>Pipeline Author</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:test:pipe</dc:identifier></metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"cov\" href=\"cover.bin\" media-type=\"image/png\" properties=\"cover-image\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

static const char* const k_chapter_xhtml = "<html><body><p>Hello pipeline.</p></body></html>";

/* Not a valid image: stb_image rejects it, so a present cover fails to decode. */
static const uint8_t k_garbage_cover[16] = {
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
 * @note Not thread-safe (writes file-scope fixture buffers).
 */
static void build_epub(bool with_cover)
{
  const char* opf = with_cover ? k_opf_with_cover : k_opf_no_cover;

  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_epub_cap) == MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "mimetype", k_mimetype, strlen(k_mimetype), MZ_NO_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "META-INF/container.xml",
                                    k_container,
                                    strlen(k_container),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/content.opf", opf, strlen(opf), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/c1.xhtml",
                                    k_chapter_xhtml,
                                    strlen(k_chapter_xhtml),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  if (with_cover) {
    TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                      "OEBPS/cover.bin",
                                      k_garbage_cover,
                                      sizeof(k_garbage_cover),
                                      MZ_NO_COMPRESSION) == MZ_TRUE);
  }

  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT((heap != nullptr) && (hsz > 0U) && (hsz <= sizeof(s_epub)));
  memcpy(s_epub, heap, hsz);
  s_epub_len = hsz;
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
static ra8_fs_mount_t* fresh_volume(void)
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
 * @pre @p mount is a live mount returned by @ref fresh_volume.
 * @pre Every open file on @p mount has been closed.
 * @post @p mount is unmounted and @p s_disk.bytes is freed.
 * @post @p s_disk.bytes is reset to NULL.
 * @note Not thread-safe.
 */
static void teardown(ra8_fs_mount_t* mount)
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
 * @note Not thread-safe (returns views over shared file-scope arenas).
 */
static void
make_views(ra8_rabook_buffers_t* bufs, ra8_rabook_pipeline_scratch_t* scr, ra8_img_arena_t* arena)
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
    .xhtml     = s_xhtml,
    .xhtml_cap = sizeof(s_xhtml),
    .image_raw = s_image_raw,
    .image_cap = sizeof(s_image_raw),
    .img_arena = arena,
    .gray      = s_gray,
    .gray_cap  = (uint32_t)k_graypix_cap,
    .css       = s_css,
    .css_cap   = sizeof(s_css),
  };
}

/* -------------------------------------------------------------------------- */
/* Extended fixtures (branch-coverage drivers) */
/* -------------------------------------------------------------------------- */

/**
 * @struct pipe_zip_entry_t
 * @brief One archive member fed to @ref build_zip.
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
 * @note Not thread-safe (writes through the caller's pointer).
 */
static void put_u32_le(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8U) & 0xFFU);
  p[2] = (uint8_t)((v >> 16U) & 0xFFU);
  p[3] = (uint8_t)((v >> 24U) & 0xFFU);
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
 * @note Not thread-safe.
 */
static size_t make_bmp(uint8_t* out, uint16_t w, uint16_t h, uint8_t gray)
{
  const uint32_t hdr   = 54U;
  const uint32_t row   = ((((uint32_t)w * 3U) + 3U) & ~3U);
  const uint32_t data  = row * (uint32_t)h;
  const uint32_t total = hdr + data;
  memset(out, 0, (size_t)total);
  out[0] = (uint8_t)'B';
  out[1] = (uint8_t)'M';
  put_u32_le(out + 2, total); /* file size          */
  put_u32_le(out + 10, hdr);  /* pixel data offset  */
  put_u32_le(out + 14, 40U);  /* DIB header size    */
  put_u32_le(out + 18, w);    /* width              */
  put_u32_le(out + 22, h);    /* height (bottom-up) */
  out[26] = 1U;               /* color planes       */
  out[28] = 24U;              /* bits per pixel     */
  put_u32_le(out + 34, data); /* raw image size     */
  for (uint32_t y = 0U; y < (uint32_t)h; y++) {
    uint8_t* px = out + hdr + (size_t)y * row;
    for (uint32_t x = 0U; x < (uint32_t)w; x++) {
      px[x * 3U + 0U] = gray;
      px[x * 3U + 1U] = gray;
      px[x * 3U + 2U] = gray;
    }
  }
  return (size_t)total;
}

/**
 * @brief Assemble an in-memory `.epub` ZIP from @p entries into @p s_epub.
 * @details The generic version of @ref build_epub: each fixture builder below
 *          declares its members as a @ref pipe_zip_entry_t array and hands it
 *          here. Entry 0 is by spec the stored `mimetype`.
 * @param[in] entries Member list (non-NULL).
 * @param[in] n       Number of members.
 * @pre @p entries has @p n valid members and @p entries[0] is the mimetype.
 * @pre The finalized archive fits in @p s_epub.
 * @post @p s_epub holds the ZIP and @p s_epub_len its length.
 * @post No filesystem state is touched.
 * @note Not thread-safe (writes the file-scope fixture buffers).
 */
static void build_zip(const pipe_zip_entry_t* entries, size_t n)
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
  mz_zip_writer_end(&zip);
}

/** @brief OPF declaring one chapter plus a small + a large BMP image. */
static const char* const k_opf_raster =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Pipeline Title</dc:title><dc:creator>Pipeline Author</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:test:pipe</dc:identifier></metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"small\" href=\"small.bmp\" media-type=\"image/bmp\" properties=\"cover-image\"/>"
  "<item id=\"big\" href=\"big.bmp\" media-type=\"image/bmp\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/** @brief OPF declaring one chapter plus a single tall (portrait) BMP cover. */
static const char* const k_opf_tall =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Pipeline Title</dc:title><dc:creator>Pipeline Author</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:test:pipe</dc:identifier></metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"tall\" href=\"tall.bmp\" media-type=\"image/bmp\" properties=\"cover-image\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/** @brief OPF declaring one chapter plus a single `text/css` stylesheet item. */
static const char* const k_opf_css =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Pipeline Title</dc:title><dc:creator>Pipeline Author</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:test:pipe</dc:identifier></metadata>"
  "<manifest><item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/** @brief OPF declaring one chapter plus an image item with no archive bytes. */
static const char* const k_opf_img_absent =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Pipeline Title</dc:title><dc:creator>Pipeline Author</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:test:pipe</dc:identifier></metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"img\" href=\"missing.png\" media-type=\"image/png\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/** @brief OPF + nav declaring two chapters with a mapped table of contents. */
static const char* const k_opf_toc =
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
 *           first-condition-false leg of s_chapter_title's compound guard. */
static const char* const k_nav_xhtml =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
  "<head><title>Contents</title></head><body>"
  "<nav epub:type=\"toc\"><ol>"
  "<li><a href=\"orphan.xhtml\">Orphan Entry</a></li>"
  "<li><a href=\"c1.xhtml#start\">Chapter One</a></li>"
  "<li><a href=\"c2.xhtml\">Chapter Two</a></li>"
  "</ol></nav></body></html>";

static const char* const k_chapter2_xhtml = "<html><body><p>Second chapter.</p></body></html>";
/** @brief Stylesheet body interned by the CSS-present fixture. */
static const char* const k_style_css = ".lead { color: #C00000; } p { margin: 0; }";
/** @brief Chapter body with no XML root element (forces a parse miss). */
static const char* const k_chapter_plain = "just plain text with no markup elements at all";

/** @brief Backing store for synthesised BMP fixtures (large image fits). */
static uint8_t s_bmp[8U * 1024U];

/**
 * @brief Build a text-chapter `.epub` whose body is @p chapter (no images/CSS).
 * @param[in] chapter NUL-terminated chapter XHTML body.
 * @pre @p chapter is non-NULL.
 * @pre @p s_epub is large enough for the finalized archive.
 * @post @p s_epub holds the ZIP and @p s_epub_len its length.
 * @post No filesystem state is touched.
 * @note Not thread-safe.
 */
static void build_epub_chapter(const char* chapter)
{
  const pipe_zip_entry_t entries[] = {
    {"mimetype", k_mimetype, strlen(k_mimetype), true},
    {"META-INF/container.xml", k_container, strlen(k_container), false},
    {"OEBPS/content.opf", k_opf_no_cover, strlen(k_opf_no_cover), false},
    {"OEBPS/c1.xhtml", chapter, strlen(chapter), false},
  };
  build_zip(entries, sizeof(entries) / sizeof(entries[0]));
}

/**
 * @brief Build an `.epub` carrying a small (2x2) and a large (1601x1) BMP image.
 * @pre @p s_epub and @p s_bmp are large enough for the fixtures.
 * @pre miniz is available (host build).
 * @post @p s_epub holds the ZIP; the small image is the declared cover.
 * @post No filesystem state is touched.
 * @note Not thread-safe.
 */
static void build_epub_raster(void)
{
  static uint8_t small_bmp[128];
  const size_t   small_len = make_bmp(small_bmp, 2U, 2U, 0x80U);
  const size_t   big_len   = make_bmp(s_bmp, (uint16_t)k_pl_big_edge, 1U, 0x80U);

  const pipe_zip_entry_t entries[] = {
    {"mimetype", k_mimetype, strlen(k_mimetype), true},
    {"META-INF/container.xml", k_container, strlen(k_container), false},
    {"OEBPS/content.opf", k_opf_raster, strlen(k_opf_raster), false},
    {"OEBPS/c1.xhtml", k_chapter_xhtml, strlen(k_chapter_xhtml), false},
    {"OEBPS/small.bmp", small_bmp, small_len, false},
    {"OEBPS/big.bmp", s_bmp, big_len, false},
  };
  build_zip(entries, sizeof(entries) / sizeof(entries[0]));
}

/**
 * @brief Build an `.epub` carrying a single tall (1 x 1601) BMP cover image.
 * @details The cover's short edge (width 1) stays below the clamp while its long
 *          edge (height 1601) is reduced to 1600, so s_downscale_if_needed sees
 *          ow == sw (1 == 1) but oh != sh (1600 != 1601) -- the height-varying
 *          MC/DC leg of `if (ow == sw && oh == sh)`.
 * @pre @p s_epub and @p s_bmp are large enough for the fixtures.
 * @post @p s_epub holds the ZIP; the tall image is the declared cover.
 * @post No filesystem state is touched.
 * @note Not thread-safe.
 */
static void build_epub_tall(void)
{
  const size_t tall_len =
    make_bmp(s_bmp, (uint16_t)k_pl_narrow_edge, (uint16_t)k_pl_big_edge, 0x80U);

  const pipe_zip_entry_t entries[] = {
    {"mimetype", k_mimetype, strlen(k_mimetype), true},
    {"META-INF/container.xml", k_container, strlen(k_container), false},
    {"OEBPS/content.opf", k_opf_tall, strlen(k_opf_tall), false},
    {"OEBPS/c1.xhtml", k_chapter_xhtml, strlen(k_chapter_xhtml), false},
    {"OEBPS/tall.bmp", s_bmp, tall_len, false},
  };
  build_zip(entries, sizeof(entries) / sizeof(entries[0]));
}

/**
 * @brief Build an `.epub` with a `text/css` item, present or absent in the ZIP.
 * @param[in] present true to store the stylesheet bytes; false to declare the
 *                    item in the manifest but omit it from the archive.
 * @pre @p s_epub is large enough for the finalized archive.
 * @post @p s_epub holds the ZIP and @p s_epub_len its length.
 * @post No filesystem state is touched.
 * @note Not thread-safe.
 */
static void build_epub_css(bool present)
{
  pipe_zip_entry_t entries[] = {
    {"mimetype", k_mimetype, strlen(k_mimetype), true},
    {"META-INF/container.xml", k_container, strlen(k_container), false},
    {"OEBPS/content.opf", k_opf_css, strlen(k_opf_css), false},
    {"OEBPS/c1.xhtml", k_chapter_xhtml, strlen(k_chapter_xhtml), false},
    {"OEBPS/style.css", k_style_css, strlen(k_style_css), false},
  };
  const size_t base = 4U; /* without the trailing style.css member */
  build_zip(entries, present ? (sizeof(entries) / sizeof(entries[0])) : base);
}

/**
 * @brief Build an `.epub` declaring an image item with no archive bytes.
 * @pre @p s_epub is large enough for the finalized archive.
 * @post @p s_epub holds the ZIP and @p s_epub_len its length.
 * @post No filesystem state is touched.
 * @note Not thread-safe.
 */
static void build_epub_image_absent(void)
{
  const pipe_zip_entry_t entries[] = {
    {"mimetype", k_mimetype, strlen(k_mimetype), true},
    {"META-INF/container.xml", k_container, strlen(k_container), false},
    {"OEBPS/content.opf", k_opf_img_absent, strlen(k_opf_img_absent), false},
    {"OEBPS/c1.xhtml", k_chapter_xhtml, strlen(k_chapter_xhtml), false},
  };
  build_zip(entries, sizeof(entries) / sizeof(entries[0]));
}

/**
 * @brief Build a two-chapter `.epub` with a nav table of contents.
 * @pre @p s_epub is large enough for the finalized archive.
 * @post @p s_epub holds the ZIP; both spine chapters carry a TOC title.
 * @post No filesystem state is touched.
 * @note Not thread-safe.
 */
static void build_epub_toc(void)
{
  const pipe_zip_entry_t entries[] = {
    {"mimetype", k_mimetype, strlen(k_mimetype), true},
    {"META-INF/container.xml", k_container, strlen(k_container), false},
    {"OEBPS/content.opf", k_opf_toc, strlen(k_opf_toc), false},
    {"OEBPS/nav.xhtml", k_nav_xhtml, strlen(k_nav_xhtml), false},
    {"OEBPS/c1.xhtml", k_chapter_xhtml, strlen(k_chapter_xhtml), false},
    {"OEBPS/c2.xhtml", k_chapter2_xhtml, strlen(k_chapter2_xhtml), false},
  };
  build_zip(entries, sizeof(entries) / sizeof(entries[0]));
}

/* -------------------------------------------------------------------------- */
/* Tests */
/* -------------------------------------------------------------------------- */

/**
 * @test test_pipeline_text_only_no_cover
 * @brief A cover-less text book compiles, validates, and round-trips its
 *        metadata; the absent cover leaves the cover index nil.
 *
 * @par MC/DC:
 * Drives the false (happy) arm of every `if (err != k_ra8_ok)` stage guard in
 * @ref ra8_rabook_compile_from_epub and the @ref k_ra8_err_not_found arm of
 * s_compile_cover (no cover -> nil index, success). No compound decision is
 * reached on this path.
 */
static void test_pipeline_text_only_no_cover(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: text-only EPUB -> valid .rabook");
  build_epub(false);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub, .size = s_epub_len};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "pipe.epub", &book));

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  /* Read the emitted blob back off the FAT volume. */
  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(file, &size));
  TEST_ASSERT(size > 0U && (size_t)size <= sizeof(s_readback));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(size, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  /* The on-device reader accepts the blob. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));

  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(1U, hdr->chapter_count);
  TEST_ASSERT_EQ(k_ra8_book_nil, hdr->cover_image_index);
  TEST_ASSERT_EQ(0U, hdr->image_count);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(s_readback, hdr->title_off), "Pipeline Title"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(s_readback, hdr->author_off), "Pipeline Author"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(s_readback, hdr->language_off), "en"));

  /* String-pool offset 0 is the reserved empty string. */
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(s_readback, 0U), ""));

  /* The single spine chapter has a 'body' root element. */
  const ra8_book_chapter_t* chaps = ra8_book_chapters(s_readback);
  const ra8_book_node_t*    nodes = ra8_book_nodes(s_readback);
  const ra8_book_node_t*    root  = &nodes[chaps[0].root_node];
  TEST_ASSERT_EQ(k_ra8_book_node_element, root->kind);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_node_name(s_readback, root), "body"));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: text-only EPUB -> valid .rabook");
}

/**
 * @test test_pipeline_undecodable_cover_skipped
 * @brief A present-but-undecodable cover is skipped (not an error), so the book
 *        still compiles to a valid coverless .rabook.
 *
 * @par MC/DC:
 * Drives s_add_manifest_image's decode-failure arm: cover.bin is an image
 * manifest item (media-type image/png), so the image loop attempts a transcode;
 * stb_image rejects the
 * garbage bytes and s_transcode_image returns nil, taking the
 * `if (idx == nil) continue` skip arm -- the same try/except pass the desktop
 * epub_compile.py uses, so one bad image never fails the whole compile. The cover
 * index stays nil because no image was added to match epub->cover_path.
 */
static void test_pipeline_undecodable_cover_skipped(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: present-but-undecodable cover -> skipped");
  build_epub(true);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub, .size = s_epub_len};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "pipe.epub", &book));
  /* Sanity: the fixture really does declare a cover. */
  TEST_ASSERT(book.cover_path[0] != '\0');

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  /* The bad cover is skipped like the desktop tool, so the compile succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  /* The coverless book was published and is valid, with a nil cover index. */
  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(k_ra8_book_nil, hdr->cover_image_index);
  TEST_ASSERT_EQ(0U, hdr->image_count);

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: present-but-undecodable cover -> skipped");
}

/* -------------------------------------------------------------------------- */
/* #151 byte-identity parity gate */
/* -------------------------------------------------------------------------- */

/**
 * @brief Assert the on-device compiler is byte-identical to the desktop tool.
 *
 * @details
 * The #151 acceptance proof: @ref ra8_rabook_compile_from_epub must emit a
 * RABOOK1 flat blob byte-for-byte identical to tools/epub_compile/epub_compile.py
 * for the text-only slice the pipeline fully supports. Opens the baked fixture
 * @c s_parity_epub from memory, compiles it onto a RAM FAT volume, reads the
 * emitted blob back, and compares it to the baked golden @c s_parity_golden (the
 * desktop output with its RBKC chunked container stripped + inflated). Regenerate
 * both arrays with `make rabook-golden-update` after any format/emitter change.
 *
 * @par MC/DC:
 * Drives the false arm of `if (scr->skip_images)` in s_compile_images (single
 * condition): skip_images is left unset, so the SVG cover IS emitted. Paired
 * with @ref test_pipeline_parity_noimg_byte_identical (which drives the true
 * arm), the two vectors (skip=false image emitted) + (skip=true image dropped)
 * give the guard its MC/DC pair.
 *
 * @pre The generated rabook_parity_fixture.h embeds a matching epub + golden.
 * @pre The RAM backend descriptor @p s_backend is initialised.
 * @post The emitted blob equals the golden, byte for byte.
 * @post The RAM volume is unmounted and its backing store freed.
 * @note Not thread-safe (writes file-scope fixture buffers).
 */
static void test_pipeline_parity_byte_identical(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: text-only fixture byte-identical to desktop");
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_parity_epub, .size = (size_t)k_parity_epub_len};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "rabook_parity.epub", &book));

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  /* The on-device flat blob must equal the desktop golden, byte for byte. On a
   * mismatch, surface the first differing offset so a format/emitter drift is
   * localized to its RABOOK1 field rather than just "not equal". */
  TEST_ASSERT_EQ(k_parity_golden_len, got);
  for (uint32_t i = 0U; i < got; i++) {
    if (s_readback[i] != s_parity_golden[i]) {
      (void)fprintf(stderr,
                    "  parity diff at offset %u: device=0x%02X golden=0x%02X\n",
                    i,
                    s_readback[i],
                    s_parity_golden[i]);
      break;
    }
  }
  TEST_ASSERT_EQ(0, memcmp(s_readback, s_parity_golden, (size_t)got));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: text-only fixture byte-identical to desktop");
}

/**
 * @brief Assert the skip-images compile equals the desktop --no-images golden.
 *
 * @details
 * The #151 skip-images acceptance proof: compiling the SAME parity fixture with
 * @ref ra8_rabook_pipeline_scratch_t::skip_images set must emit a RABOOK1 blob
 * byte-for-byte identical to tools/epub_compile/epub_compile.py run with its
 * @c --no-images flag. The fixture carries an SVG cover, so the default compile
 * (cover present) and the skip-images compile (no image table, nil cover) differ
 * -- proving both sides agree on dropping images on a case that already parses
 * identically. Opens the baked @c s_parity_epub, compiles it with
 * @c scr.skip_images = true onto a RAM FAT volume, reads the blob back, and
 * compares it to the baked @c s_parity_golden_noimg.
 *
 * @par MC/DC:
 * Drives the true arm of `if (scr->skip_images)` in s_compile_images (single
 * condition); the with-images parity test above drives its false arm. Vectors
 * (skip=false image emitted) + (skip=true image dropped) cover the guard.
 *
 * @pre The generated rabook_parity_fixture.h embeds a matching --no-images golden.
 * @pre The RAM backend descriptor @p s_backend is initialised.
 * @post The emitted blob equals the --no-images golden, byte for byte.
 * @post The RAM volume is unmounted and its backing store freed.
 * @note Not thread-safe (writes file-scope fixture buffers).
 */
static void test_pipeline_parity_noimg_byte_identical(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: skip-images fixture byte-identical to --no-images");
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_parity_epub, .size = (size_t)k_parity_epub_len};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "rabook_parity.epub", &book));

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.skip_images = true; /* text/CSS-only: drop the SVG cover like desktop --no-images */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  /* The skip-images blob has no image table and a nil cover index. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(0U, hdr->image_count);
  TEST_ASSERT_EQ(k_ra8_book_nil, hdr->cover_image_index);

  /* It must equal the desktop --no-images golden, byte for byte. On a mismatch,
   * surface the first differing offset to localize any drift. */
  TEST_ASSERT_EQ(k_parity_golden_noimg_len, got);
  for (uint32_t i = 0U; i < got; i++) {
    if (s_readback[i] != s_parity_golden_noimg[i]) {
      (void)fprintf(stderr,
                    "  noimg parity diff at offset %u: device=0x%02X golden=0x%02X\n",
                    i,
                    s_readback[i],
                    s_parity_golden_noimg[i]);
      break;
    }
  }
  TEST_ASSERT_EQ(0, memcmp(s_readback, s_parity_golden_noimg, (size_t)got));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: skip-images fixture byte-identical to --no-images");
}

/**
 * @brief Assert a real-book skip-images compile equals the desktop golden.
 *
 * @details
 * The #151 real-book acceptance proof: a trimmed fixture of REAL Standard Ebooks
 * Walden chapters -- @c visitors and @c conclusion, vendored verbatim under
 * tests/fixtures/rabook_realbook/ -- is compiled on-device with
 * @ref ra8_rabook_pipeline_scratch_t::skip_images set and must emit a RABOOK1
 * blob byte-for-byte identical to tools/epub_compile/epub_compile.py run with
 * @c --no-images. Both chapters carry the significant `</abbr> <abbr>`
 * inter-element whitespace (a space between two name-title abbreviations); the
 * SE markup is also indented, so the body subtree is full of inter-element
 * whitespace text runs. The desktop reference (Python @c HTMLParser) keeps every
 * such run, and -- with the on-device tinyxml2 opting into @c PEDANTIC_WHITESPACE
 * (#151) -- so does the device. Byte-identity here is the direct proof that the
 * device preserves inline whitespace on real content, so words like
 * "@c Hon. @c Mr." no longer merge into "@c Hon.Mr.".
 *
 * @par MC/DC:
 * Drives the same single-condition `if (scr->skip_images)` true arm as
 * @ref test_pipeline_parity_noimg_byte_identical, on a larger real-content
 * input; it adds no new compound decision -- its role is content fidelity, not
 * decision coverage.
 *
 * @pre The generated rabook_realbook_fixture.h embeds a matching epub + golden.
 * @pre The RAM backend descriptor @p s_backend is initialised.
 * @post The emitted blob equals the --no-images golden, byte for byte.
 * @post The RAM volume is unmounted and its backing store freed.
 * @note Not thread-safe (writes file-scope fixture buffers).
 */
static void test_pipeline_parity_realbook_byte_identical(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: real Walden chapters byte-identical to --no-images");
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_realbook_epub, .size = (size_t)k_realbook_epub_len};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "walden.epub", &book));

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.skip_images = true; /* text/CSS-only: real chapters + CSS, no images */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));

  /* The real-book blob must equal the desktop --no-images golden, byte for byte.
   * On a mismatch, surface the first differing offset to localize any drift (a
   * whitespace-fidelity regression would show up as a string-pool divergence). */
  TEST_ASSERT_EQ(k_realbook_golden_noimg_len, got);
  for (uint32_t i = 0U; i < got; i++) {
    if (s_readback[i] != s_realbook_golden_noimg[i]) {
      (void)fprintf(stderr,
                    "  realbook parity diff at offset %u: device=0x%02X golden=0x%02X\n",
                    i,
                    s_readback[i],
                    s_realbook_golden_noimg[i]);
      break;
    }
  }
  TEST_ASSERT_EQ(0, memcmp(s_readback, s_realbook_golden_noimg, (size_t)got));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: real Walden chapters byte-identical to --no-images");
}

/* -------------------------------------------------------------------------- */
/* Branch-coverage tests (raster transcode, TOC, CSS, error legs) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Open the staged @ref s_epub fixture into @p book.
 * @param[out] book Receives the opened book (non-NULL).
 * @pre @p s_epub / @p s_epub_len hold a built archive.
 * @pre @p book is non-NULL.
 * @post @p book->in_use == 1 on success.
 * @post @p s_epub is unmodified.
 * @note Not thread-safe.
 */
static void open_s_epub(ra8_epub_book_t* book)
{
  const ra8_epub_mem_media_t media = {.data = s_epub, .size = s_epub_len};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "pipe.epub", book));
}

/**
 * @test test_pipeline_raster_images_transcoded
 * @brief Two decodable BMPs are decoded, gray4-encoded, and added; the cover
 *        resolves to the small image.
 *
 * @par MC/DC:
 * Decision: `if (ow == sw && oh == sh)` in s_downscale_if_needed (2 conditions).
 * The 2x2 image hits ow==sw && oh==sh -> true (copy in place); the 1601x1 image
 * hits ow!=sw -> false (downscale). N+1 = 3 vectors:
 * - Vector 1: ow==sw, oh==sh (2x2)            -> true  (control: both equal).
 * - Vector 2: ow!=sw, oh==sh (1601x1 -> 1600) -> false (varies width only).
 * - Vector 3: ow==sw, oh!=sh (1x1601 -> 1600) -> false (varies height only,
 *   driven by @ref test_pipeline_gray_scratch_too_small over the tall image).
 * Vectors 1+2 show width independently flips the outcome; 1+3 show height does.
 */
static void test_pipeline_raster_images_transcoded(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: BMP images transcoded to gray4");
  build_epub_raster();
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.max_image_edge = k_pl_clamp_edge; /* opt into the downscale clamp */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(2U, hdr->image_count);       /* small + big both added */
  TEST_ASSERT_EQ(0U, hdr->cover_image_index); /* small.bmp is the cover */

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: BMP images transcoded to gray4");
}

/**
 * @test test_pipeline_default_preserves_resolution
 * @brief With no opt-in clamp (the zero-init default) the oversized image is
 *        stored at its exact source resolution.
 *
 * @par Targeted code:
 * The new opt-out arm of the transcode stage: `scr->max_image_edge == 0`
 * skips ra8_rabook_gray4_output_dims entirely, so ow/oh stay the source
 * dimensions and s_downscale_if_needed takes its copy-in-place arm with no
 * gray scratch needed (issue #210: full-resolution sources for the zoom
 * loupe).
 *
 * @par MC/DC:
 * (no compound decisions under test -- the opt-in clamp gate
 * `scr->max_image_edge != 0U` is a single condition; its true arm is driven
 * by test_pipeline_raster_images_transcoded and this test drives the false
 * arm)
 */
static void test_pipeline_default_preserves_resolution(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: default preserves source resolution");
  build_epub_raster();
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena); /* max_image_edge stays 0: no clamp */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(2U, hdr->image_count);
  /* The 1601x1 image keeps every source pixel under the default. */
  const ra8_book_image_t* imgs    = ra8_book_images(s_readback);
  bool                    saw_big = false;
  for (uint32_t i = 0U; i < hdr->image_count; ++i) {
    if (imgs[i].width == k_pl_big_edge) {
      TEST_ASSERT_EQ(1U, imgs[i].height);
      saw_big = true;
    }
  }
  TEST_ASSERT(saw_big);

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: default preserves source resolution");
}

/**
 * @test test_pipeline_gray_scratch_too_small
 * @brief A too-small gray scratch makes the downscaled image skip while the
 *        in-place (no-downscale) image still compiles.
 *
 * @par MC/DC:
 * Drives the gray-capacity guard `if ((uint32_t)ow * oh > scr->gray_cap)` true
 * arm (single condition): the 1601x1 image needs a 1600-pixel scratch but only 4
 * are offered, so s_downscale_if_needed returns nullptr and s_transcode_image
 * takes its `gray_src == nullptr` cleanup-and-skip arm. The 2x2 image needs no
 * scratch (copy in place), so it still adds -- one decodable image survives.
 * The false arm of the same guard is driven by
 * @ref test_pipeline_raster_images_transcoded (ample scratch).
 */
static void test_pipeline_gray_scratch_too_small(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: gray scratch too small skips downscale image");
  build_epub_raster();
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.max_image_edge = k_pl_clamp_edge; /* opt into the downscale clamp                  */
  scr.gray_cap       = 4U;              /* far below the 1600 pixels the big image needs */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(1U, hdr->image_count);       /* only the 2x2 in-place image survives */
  TEST_ASSERT_EQ(0U, hdr->cover_image_index); /* and it is the cover                  */

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: gray scratch too small skips downscale image");
}

/**
 * @test test_pipeline_tall_image_height_downscaled
 * @brief A tall cover keeps its width but loses height to the clamp, driving the
 *        height-varying leg of the downscale decision.
 *
 * @par MC/DC:
 * Completes `if (ow == sw && oh == sh)` in s_downscale_if_needed (2 conditions):
 * the 1 x 1601 cover clamps to 1 x 1600, so ow == sw (1 == 1 -> C1 true) while
 * oh != sh (1600 != 1601 -> C2 false) -- the (true, false) vector that isolates
 * the height condition. Paired with test_pipeline_raster_images_transcoded's
 * (true, true) 2x2 control and (false, true) wide-image legs, the three vectors
 * are the N+1 = 3 minimal MC/DC set for the decision.
 */
static void test_pipeline_tall_image_height_downscaled(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: tall cover downscales height only");
  build_epub_tall();
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.max_image_edge = k_pl_clamp_edge; /* opt into the downscale clamp */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr  = ra8_book_header(s_readback);
  const ra8_book_image_t*  imgs = ra8_book_images(s_readback);
  TEST_ASSERT_EQ(1U, hdr->image_count);
  /* Width unchanged (1), height clamped to 1600: the (C1 true, C2 false) leg. */
  TEST_ASSERT_EQ(k_pl_narrow_edge, imgs[0].width);
  TEST_ASSERT_EQ(k_pl_clamp_edge, imgs[0].height);

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: tall cover downscales height only");
}

/**
 * @test test_pipeline_toc_titles_resolved
 * @brief Each spine chapter picks up its TOC title via s_chapter_title.
 *
 * @par MC/DC:
 * Decision: `if (toc_to_chapter() == k_ra8_ok && ch_idx == chapter_idx)` in
 * s_chapter_title (2 conditions), exercised over two chapters with a three-entry
 * nav whose first entry ("orphan.xhtml") maps to no spine chapter. N+1 = 3:
 * - V1: entry resolves and ch_idx == chapter_idx -> C1 true, C2 true (control:
 *   entry 1 for chapter 0, entry 2 for chapter 1 -> a title is copied).
 * - V2: entry resolves but ch_idx != chapter_idx -> C1 true, C2 false (entry 1's
 *   ch_idx 0 != chapter_idx 1 while resolving chapter 1; isolates C2 vs V1).
 * - V3: entry fails to resolve -> C1 false, short-circuit (the leading orphan
 *   entry, scanned first for every chapter; isolates C1 vs V1).
 * Reading the two titles back proves each chapter resolved its own entry past the
 * orphan.
 */
static void test_pipeline_toc_titles_resolved(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: TOC titles attach to chapters");
  build_epub_toc();
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t*  hdr   = ra8_book_header(s_readback);
  const ra8_book_chapter_t* chaps = ra8_book_chapters(s_readback);
  TEST_ASSERT_EQ(2U, hdr->chapter_count);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(s_readback, chaps[0].title_off), "Chapter One"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(s_readback, chaps[1].title_off), "Chapter Two"));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: TOC titles attach to chapters");
}

/**
 * @test test_pipeline_css_absent_skipped
 * @brief A `text/css` item declared but missing from the archive is skipped,
 *        like the desktop "only if present" rule.
 *
 * @par MC/DC:
 * Drives the `if (err == k_ra8_err_not_found)` true arm in s_compile_stylesheets
 * (single condition): the manifest declares style.css but the ZIP omits it, so
 * ra8_epub_get_resource returns not_found and the stage `continue`s rather than
 * failing. The false arm (a present stylesheet) is exercised by the parity
 * fixtures, which intern their CSS.
 */
static void test_pipeline_css_absent_skipped(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: absent CSS item skipped");
  build_epub_css(false);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(0U, hdr->stylesheet_count); /* the absent CSS contributed nothing */
  TEST_ASSERT_EQ(1U, hdr->chapter_count);

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: absent CSS item skipped");
}

/**
 * @test test_pipeline_css_load_error_propagates
 * @brief A non-not_found stylesheet error aborts the compile and propagates out
 *        of every stage wrapper.
 *
 * @par MC/DC:
 * Drives the `if (err != k_ra8_ok)` true arm of the stylesheet resource load
 * (single condition): the stylesheet is present but the load buffer is sized to
 * three bytes, so ra8_epub_get_resource returns k_ra8_err_no_mem -- neither ok nor
 * not_found -- and s_compile_stylesheets returns it. That error then propagates
 * the true arm of the stylesheet-stage guard in s_compile_to_blob and the
 * compile-failure guard in ra8_rabook_compile_from_epub (no file is written).
 */
static void test_pipeline_css_load_error_propagates(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: CSS load error propagates");
  build_epub_css(true);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.css_cap = 4U; /* style.css is larger than css_cap - 1 -> no_mem */

  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: CSS load error propagates");
}

/**
 * @test test_pipeline_image_resource_absent_skipped
 * @brief An image item missing from the archive is skipped, not fatal.
 *
 * @par MC/DC:
 * Drives the `if (err != k_ra8_ok)` true arm of the resource load in
 * s_add_manifest_image (single condition): the manifest declares missing.png but
 * the archive omits it, so ra8_epub_get_resource fails and the item returns nil
 * (skipped) -- the desktop try/except pass. The false arm is exercised whenever a
 * present image loads (the raster + parity fixtures).
 */
static void test_pipeline_image_resource_absent_skipped(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: absent image item skipped");
  build_epub_image_absent();
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(0U, hdr->image_count);
  TEST_ASSERT_EQ(k_ra8_book_nil, hdr->cover_image_index);

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: absent image item skipped");
}

/**
 * @test test_pipeline_chapter_load_too_small
 * @brief A chapter that overflows the XHTML scratch aborts and propagates.
 *
 * @par MC/DC:
 * Drives the `if (err != k_ra8_ok)` true arm of the chapter-load guard in
 * s_compile_chapters (single condition): the XHTML scratch is sized to four
 * bytes, so ra8_epub_load_chapter returns k_ra8_err_no_mem and the stage returns
 * it, which propagates the chapter-stage guard in s_compile_to_blob and the
 * compile-failure guard in the filesystem entry point. The false arm is the
 * happy load every passing compile exercises.
 */
static void test_pipeline_chapter_load_too_small(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: chapter overflow propagates");
  build_epub(false);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.xhtml_cap = 4U; /* the chapter does not fit -> no_mem */

  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: chapter overflow propagates");
}

/**
 * @test test_pipeline_malformed_chapter_propagates
 * @brief A chapter with no XML root element fails the parser and propagates.
 *
 * @par MC/DC:
 * Drives the `if (err != k_ra8_ok)` true arm of the parse guard in
 * s_compile_chapters (single condition): the chapter body is plain text with no
 * element, so ra8_rabook_xml_parse_chapter finds no `<body>` root and returns
 * k_ra8_err_no_mem, which the stage and s_compile_to_blob propagate. The false arm
 * is the well-formed chapter every passing compile parses.
 */
static void test_pipeline_malformed_chapter_propagates(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: malformed chapter propagates");
  build_epub_chapter(k_chapter_plain);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: malformed chapter propagates");
}

/**
 * @test test_pipeline_to_buffer_and_null_guards
 * @brief The no-filesystem entry point emits a valid blob in place and rejects
 *        each NULL output pointer.
 *
 * @par MC/DC:
 * Covers every guard of ra8_rabook_compile_from_epub_to_buffer. The shared-arg
 * check is exercised by the valid call (false) and a NULL epub (true); the
 * out_blob / out_len guards are each exercised true with a NULL pointer and
 * false on the valid call. Each is a single-condition null guard, so one
 * true/false vector pair per guard is minimal MC/DC.
 */
static void test_pipeline_to_buffer_and_null_guards(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: compile-to-buffer + null guards");
  build_epub(false);

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub_to_buffer(&book, &bufs, &scr, &blob, &len));
  TEST_ASSERT(blob != nullptr && len > 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate((const uint8_t*)blob, (size_t)len));

  /* Null guards: shared-arg check, then each output pointer. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub_to_buffer(nullptr, &bufs, &scr, &blob, &len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub_to_buffer(&book, &bufs, &scr, nullptr, &len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub_to_buffer(&book, &bufs, &scr, &blob, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("ra8_rabook_pipeline: compile-to-buffer + null guards");
}

/**
 * @test test_pipeline_fs_null_guards
 * @brief The filesystem entry point rejects each NULL pointer.
 *
 * @par MC/DC:
 * Drives the true arm of the shared-arg check (NULL epub) and of the mount /
 * out_path guards (each a single-condition null guard); the false arms are the
 * valid compiles above. One NULL vector per guard plus the valid baseline is
 * minimal MC/DC for these guards.
 */
static void test_pipeline_fs_null_guards(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: filesystem entry null guards");
  build_epub(false);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub(nullptr, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub(&book, &bufs, &scr, nullptr, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  teardown(mount);
  TEST_END("ra8_rabook_pipeline: filesystem entry null guards");
}

/**
 * @test test_pipeline_compile_init_rejects_bad_buffers
 * @brief A NULL builder-arena member fails ra8_rabook_compile_init and propagates
 *        out of s_compile_to_blob.
 *
 * @par MC/DC:
 * Drives the `if (err != k_ra8_ok)` true arm of the builder-init guard in
 * s_compile_to_blob (single condition): the top-level pointers are all non-NULL
 * (so the shared-arg check passes) but @p bufs->out is NULL, so
 * ra8_rabook_compile_init rejects the arenas and s_compile_to_blob returns its
 * error. The false arm is every passing compile.
 */
static void test_pipeline_compile_init_rejects_bad_buffers(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: compile_init rejects bad arenas");
  build_epub(false);

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  bufs.out = nullptr; /* a NULL arena member fails compile_init */

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub_to_buffer(&book, &bufs, &scr, &blob, &len));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("ra8_rabook_pipeline: compile_init rejects bad arenas");
}

/**
 * @test test_pipeline_closed_book_chapter_count_fails
 * @brief A book that is not open makes the chapter stage fail and propagate.
 *
 * @par MC/DC:
 * Drives the `if (err != k_ra8_ok)` true arm of the chapter-count guard in
 * s_compile_chapters (single condition): the book is zero-initialised
 * (in_use == 0), so the empty manifest yields no stylesheets / images and
 * ra8_epub_get_chapter_count then returns k_ra8_err_not_initialized, which the
 * stage and s_compile_to_blob propagate. The false arm is the open-book compile.
 */
static void test_pipeline_closed_book_chapter_count_fails(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: closed book fails at chapter count");

  ra8_epub_book_t book = {}; /* never opened: in_use == 0 */

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_rabook_compile_from_epub_to_buffer(&book, &bufs, &scr, &blob, &len));

  TEST_END("ra8_rabook_pipeline: closed book fails at chapter count");
}

/* -------------------------------------------------------------------------- */
/* Log sink + main */
/* -------------------------------------------------------------------------- */

/**
 * @brief No-op log byte sink so the logger never pokes ITM MMIO on the host.
 * @param[in] ctx  Unused sink context.
 * @param[in] byte Unused log byte.
 * @pre Installed in main() before any test runs.
 * @pre Never called from interrupt context (host build).
 * @post No global state is mutated.
 * @post The byte is discarded.
 * @note Not thread-safe (host single-thread test driver).
 */
static void s_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

int32_t main(void)
{
  ra8_log_set_byte_sink(s_log_sink, nullptr);
  test_pipeline_text_only_no_cover();
  test_pipeline_undecodable_cover_skipped();
  test_pipeline_parity_byte_identical();
  test_pipeline_parity_noimg_byte_identical();
  test_pipeline_parity_realbook_byte_identical();
  test_pipeline_raster_images_transcoded();
  test_pipeline_default_preserves_resolution();
  test_pipeline_gray_scratch_too_small();
  test_pipeline_tall_image_height_downscaled();
  test_pipeline_toc_titles_resolved();
  test_pipeline_css_absent_skipped();
  test_pipeline_css_load_error_propagates();
  test_pipeline_image_resource_absent_skipped();
  test_pipeline_chapter_load_too_small();
  test_pipeline_malformed_chapter_propagates();
  test_pipeline_to_buffer_and_null_guards();
  test_pipeline_fs_null_guards();
  test_pipeline_compile_init_rejects_bad_buffers();
  test_pipeline_closed_book_chapter_count_fails();
  (void)fprintf(stderr, "[OK ] test_ra8_rabook_pipeline.c\n");
  return 0;
}
