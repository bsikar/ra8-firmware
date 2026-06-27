/**
 * @file test_ra_rabook_pipeline.c
 * @brief End-to-end EPUB -> RABOOK1 pipeline test (ra_rabook_pipeline, #149).
 *
 * @details
 * Exercises @ref ra_rabook_compile_from_epub -- the only entry point that wires
 * the metadata, cover, chapter and finalize stages together -- against a tiny
 * synthetic book:
 *
 *  - A text-only `.epub` is assembled in memory with miniz (the build pattern
 *    from tests/test_ra_epub.c) and opened with @ref ra_epub_open.
 *  - A real FAT16 volume is formatted over a RAM block backend (the RAM-disk
 *    pattern from tests/test_ra_epub_fs.c) so the pipeline's
 *    @ref ra_fs_write_file lands on a genuine filesystem.
 *  - Happy path (no cover): the compile succeeds, the written `.rabook` passes
 *    @ref ra_book_validate, chapter_count / title / author read back as built,
 *    string-pool offset 0 is the reserved "", and the absent cover leaves the
 *    cover-image index nil.
 *  - Error path (present-but-undecodable cover): a manifest cover whose bytes
 *    are NOT a valid image makes stb_image fail to decode; per B's fix a present
 *    cover that cannot be transcoded surfaces as @ref k_ra_err_no_mem (it is not
 *    silently dropped), and no output file is published.
 *
 * @par MC/DC:
 * The pipeline's only compound decision reachable from here is
 * `if (ow == sw && oh == sh)` in s_downscale_if_needed; it is NOT exercised
 * because the cover path errors before any successful decode (the decoder
 * rejects the garbage cover). All other branches touched here
 * (cover-not-found vs cover-present-fail, and the per-stage error propagation)
 * are single-condition `if (err != k_ra_ok)` guards, so each is covered by
 * driving its one condition true (error path) or false (happy path).
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
#include "ra_book.h"
#include "ra_epub.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_img_arena.h"
#include "ra_log.h"
#include "ra_rabook_compile.h"
#include "ra_rabook_pipeline.h"
#include "ra_reflow_image.h"
#include "unity_minimal.h"

/* -------------------------------------------------------------------------- */
/* Sizing constants */
/* -------------------------------------------------------------------------- */

/**
 * @enum pipe_disk_t
 * @brief RAM block-device geometry (4 MiB -> auto-detects FAT16).
 */
typedef enum : uint32_t {
  k_disk_block_size = 512U,  /**< Bytes per sector (the only size ra_fs accepts). */
  k_disk_blocks     = 8192U, /**< 4 MiB volume -> count_of_clusters lands FAT16.  */
} pipe_disk_t;

/**
 * @enum pipe_cap_t
 * @brief Builder-arena capacities for the compiled book.
 */
typedef enum : uint32_t {
  k_chapter_cap = 8U,           /**< Max chapters.                 */
  k_node_cap    = 256U,         /**< Max DOM nodes.                */
  k_attr_cap    = 64U,          /**< Max attribute records.        */
  k_style_cap   = 4U,           /**< Max stylesheets.              */
  k_image_cap   = 8U,           /**< Max image descriptors.        */
  k_string_cap  = 8U * 1024U,   /**< String-pool capacity (bytes). */
  k_imgpool_cap = 256U * 1024U, /**< Image-pool capacity (bytes).  */
  k_out_cap     = 64U * 1024U,  /**< Output-blob capacity (bytes). */
} pipe_cap_t;

/**
 * @enum pipe_scratch_t
 * @brief Pipeline scratch-buffer capacities.
 */
typedef enum : uint32_t {
  k_xhtml_cap   = 16U * 1024U,  /**< Chapter XHTML scratch (bytes).        */
  k_imgraw_cap  = 64U * 1024U,  /**< Raw cover/image byte scratch (bytes). */
  k_arena_cap   = 256U * 1024U, /**< stb_image bump-arena scratch (bytes). */
  k_graypix_cap = 64U * 1024U,  /**< Intermediate gray downscale (pixels). */
  k_epub_cap    = 16U * 1024U,  /**< In-memory ZIP build buffer (bytes).   */
  k_read_cap    = 64U * 1024U,  /**< .rabook read-back buffer (bytes).     */
} pipe_scratch_t;

/* -------------------------------------------------------------------------- */
/* Static storage */
/* -------------------------------------------------------------------------- */

static ra_book_chapter_t    s_chapters[k_chapter_cap];
static ra_book_node_t       s_nodes[k_node_cap];
static ra_book_attr_t       s_attrs[k_attr_cap];
static ra_book_stylesheet_t s_styles[k_style_cap];
static ra_book_image_t      s_images[k_image_cap];
static char                 s_strpool[k_string_cap];
static uint8_t              s_imgpool[k_imgpool_cap];
static uint8_t              s_out[k_out_cap];

static uint8_t s_xhtml[k_xhtml_cap];
static uint8_t s_image_raw[k_imgraw_cap];
static uint8_t s_img_scratch[k_arena_cap];
static uint8_t s_gray[k_graypix_cap];

static uint8_t s_epub[k_epub_cap];
static size_t  s_epub_len;
static uint8_t s_readback[k_read_cap];

typedef struct {
  uint8_t* bytes;
  uint32_t block_count;
} mem_disk_t;

static mem_disk_t s_disk = {};

/* -------------------------------------------------------------------------- */
/* RAM block backend (4 MiB -> FAT16 via ra_fs_format) */
/* -------------------------------------------------------------------------- */

static ra_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  if (lba + count > disk->block_count) {
    return k_ra_err_out_of_range;
  }
  size_t off = (size_t)lba * (size_t)k_disk_block_size;
  size_t len = (size_t)count * (size_t)k_disk_block_size;
  memcpy(buf, &disk->bytes[off], len);
  return k_ra_ok;
}

static ra_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  if (lba + count > disk->block_count) {
    return k_ra_err_out_of_range;
  }
  size_t off = (size_t)lba * (size_t)k_disk_block_size;
  size_t len = (size_t)count * (size_t)k_disk_block_size;
  memcpy(&disk->bytes[off], buf, len);
  return k_ra_ok;
}

static ra_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  *block_count     = disk->block_count;
  *block_size      = (uint32_t)k_disk_block_size;
  return k_ra_ok;
}

static const ra_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

/* -------------------------------------------------------------------------- */
/* EPUB fixture (text-only, optional undecodable cover) */
/* -------------------------------------------------------------------------- */

static const char* const k_mimetype  = "application/epub+zip";
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

static const char* const k_chapter_xhtml =
  "<html><body><p>Hello pipeline.</p></body></html>";

/* Not a valid image: stb_image rejects it, so a present cover fails to decode. */
static const uint8_t k_garbage_cover[16] = {
  0xDEU, 0xADU, 0xBEU, 0xEFU, 0x00U, 0x01U, 0x02U, 0x03U,
  0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU,
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
static ra_fs_mount_t* fresh_volume(void)
{
  free(s_disk.bytes);
  s_disk.block_count = (uint32_t)k_disk_blocks;
  s_disk.bytes       = (uint8_t*)calloc((size_t)k_disk_blocks, (size_t)k_disk_block_size);
  TEST_ASSERT(s_disk.bytes != nullptr);

  ra_fs_format_opts_t opts = {};
  opts.type                = k_ra_fs_type_fat16;
  opts.label               = "RABOOK";
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_format(&s_backend, &opts));

  ra_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &mount));
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
static void teardown(ra_fs_mount_t* mount)
{
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(mount));
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
static void make_views(ra_rabook_buffers_t*          bufs,
                       ra_rabook_pipeline_scratch_t* scr,
                       ra_img_arena_t*               arena)
{
  *bufs = (ra_rabook_buffers_t){
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
  *arena = (ra_img_arena_t){s_img_scratch, sizeof(s_img_scratch), 0U, 0U};
  *scr   = (ra_rabook_pipeline_scratch_t){
      .xhtml     = s_xhtml,
      .xhtml_cap = sizeof(s_xhtml),
      .image_raw = s_image_raw,
      .image_cap = sizeof(s_image_raw),
      .img_arena = arena,
      .gray      = s_gray,
      .gray_cap  = (uint32_t)k_graypix_cap,
  };
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
 * Drives the false (happy) arm of every `if (err != k_ra_ok)` stage guard in
 * @ref ra_rabook_compile_from_epub and the @ref k_ra_err_not_found arm of
 * s_compile_cover (no cover -> nil index, success). No compound decision is
 * reached on this path.
 */
static void test_pipeline_text_only_no_cover(void)
{
  TEST_BEGIN("ra_rabook_pipeline: text-only EPUB -> valid .rabook");
  build_epub(false);
  ra_fs_mount_t* mount = fresh_volume();

  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub, .size = s_epub_len};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, "pipe.epub", &book));

  ra_rabook_buffers_t          bufs  = {};
  ra_rabook_pipeline_scratch_t scr   = {};
  ra_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra_ok, ra_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));

  /* Read the emitted blob back off the FAT volume. */
  ra_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(mount, "OUT.RAB", k_ra_fs_mode_read, &file));
  uint32_t size = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_size(file, &size));
  TEST_ASSERT(size > 0U && (size_t)size <= sizeof(s_readback));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(size, got);
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(file));

  /* The on-device reader accepts the blob. */
  TEST_ASSERT_EQ(k_ra_ok, ra_book_validate(s_readback, (size_t)got));

  const ra_book_header_t* hdr = ra_book_header(s_readback);
  TEST_ASSERT_EQ(1U, hdr->chapter_count);
  TEST_ASSERT_EQ((uint32_t)k_ra_book_nil, hdr->cover_image_index);
  TEST_ASSERT_EQ(0U, hdr->image_count);
  TEST_ASSERT_EQ(0, strcmp(ra_book_string(s_readback, hdr->title_off), "Pipeline Title"));
  TEST_ASSERT_EQ(0, strcmp(ra_book_string(s_readback, hdr->author_off), "Pipeline Author"));
  TEST_ASSERT_EQ(0, strcmp(ra_book_string(s_readback, hdr->language_off), "en"));

  /* String-pool offset 0 is the reserved empty string. */
  TEST_ASSERT_EQ(0, strcmp(ra_book_string(s_readback, 0U), ""));

  /* The single spine chapter has a 'body' root element. */
  const ra_book_chapter_t* chaps = ra_book_chapters(s_readback);
  const ra_book_node_t*    nodes = ra_book_nodes(s_readback);
  const ra_book_node_t*    root  = &nodes[chaps[0].root_node];
  TEST_ASSERT_EQ((uint8_t)k_ra_book_node_element, root->kind);
  TEST_ASSERT_EQ(0, strcmp(ra_book_node_name(s_readback, root), "body"));

  teardown(mount);
  TEST_END("ra_rabook_pipeline: text-only EPUB -> valid .rabook");
}

/**
 * @test test_pipeline_undecodable_cover_no_mem
 * @brief A present-but-undecodable cover makes the compile fail with no_mem and
 *        publishes no output file (B's fix: a failed cover is not dropped).
 *
 * @par MC/DC:
 * Drives the cover stage past the @ref k_ra_err_not_found early-return (the
 * cover IS present) into the transcode-failure arm: stb_image rejects the
 * garbage bytes, so s_transcode_image returns nil and s_compile_cover maps that
 * to @ref k_ra_err_no_mem -- the true arm of `if (cover_index == nil)`.
 */
static void test_pipeline_undecodable_cover_no_mem(void)
{
  TEST_BEGIN("ra_rabook_pipeline: present-but-undecodable cover -> no_mem");
  build_epub(true);
  ra_fs_mount_t* mount = fresh_volume();

  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_epub, .size = s_epub_len};
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_open(&media, "pipe.epub", &book));
  /* Sanity: the fixture really does declare a cover. */
  TEST_ASSERT(book.cover_path[0] != '\0');

  ra_rabook_buffers_t          bufs  = {};
  ra_rabook_pipeline_scratch_t scr   = {};
  ra_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra_err_no_mem,
                 ra_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra_ok, ra_epub_close(&book));

  /* The aborted compile never wrote the output file. */
  ra_fs_file_t* file = nullptr;
  TEST_ASSERT(ra_fs_open(mount, "OUT.RAB", k_ra_fs_mode_read, &file) != k_ra_ok);

  teardown(mount);
  TEST_END("ra_rabook_pipeline: present-but-undecodable cover -> no_mem");
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
  ra_log_set_byte_sink(s_log_sink, nullptr);
  test_pipeline_text_only_no_cover();
  test_pipeline_undecodable_cover_no_mem();
  (void)fprintf(stderr, "[OK ] test_ra_rabook_pipeline.c\n");
  return 0;
}
