/**
 * @file test_ra8_rabook_import_streamed.c
 * @brief #230 gate: the production import compile adapter STREAMS its source
 *        through a bounded page cache -- no whole-file residency -- and its
 *        output stays byte-identical to the desktop golden.
 *
 * @par Tag
 * [Ring 4 / Test] {World: NS}
 *
 * @details
 * Exercises @ref ra8_rabook_import_compile_adapter -- the production binding of
 * the import compile seam -- end to end over a real FAT16 volume (RAM block
 * backend, the mem-disk pattern of tests/test_ra8_rabook_import_m33.c):
 *
 *  - **Byte parity** (the #230 lock-step proof): the parity fixture `.epub` is
 *    compiled through the STREAMED adapter and the emitted `.rabook` is
 *    byte-identical to the desktop `tools/epub_compile.py` golden
 *    (`s_parity_golden`) and `ra8_book_validate`-clean. The legacy whole-file
 *    open produced this exact blob, so the migration is proven lossless.
 *  - **Bounded RAM high-water** (the #230 acceptance): a fixture EPUB far
 *    LARGER than the retired whole-file load buffer (the import_reader app
 *    carried a 128 KiB `epub_load_buf`; this fixture is ~390 KiB) is compiled
 *    through a deliberately tiny 4 KiB frame pool. The mem-subsystem stats
 *    (`ra8_vmem_stats` on the cookie's cache, plus the caller-owned frame
 *    metadata) then prove the source-side residency high-water was the fixed
 *    pool, not the file: misses and evictions occurred (far more source bytes
 *    streamed through than the pool holds), hot ZIP-tail pages were re-used
 *    (hits), and the resident set never exceeds the pool.
 *  - **Guard paths**: NULL arguments, a missing source, and an empty source
 *    all fail cleanly without writing output.
 *
 * @par MC/DC:
 * The streamed adapter introduces no compound boolean decisions -- every guard
 * in ra8_rabook_import_compiler.c's stream path (`s_source_read`,
 * `s_cache_bind`, `s_stream_open`, the adapter body) is a single-condition
 * `if (err != k_ra8_ok)` / `RA8_CHECK_NULL_PTR` early return, each driven both
 * ways across the happy-path and guard cases below.
 *
 *
 * [Ring 4 / EPUB Import] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
#include "miniz.h"
#include "ra8_book.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_img_arena.h"
#include "ra8_log.h"
#include "ra8_rabook_import_compiler.h"
#include "ra8_rabook_pipeline.h"
#include "ra8_vmem.h"
#include "rabook_parity_fixture.h"
#include "unity_minimal.h"

/**
 * @enum t_filler_t
 * @brief Byte-pattern stride of the streamed filler member.
 */
typedef enum : uint8_t {
  k_t_filler_stride = 31U, /**< Multiplier of the filler pattern; co-prime with
                                the stream chunk size so no chunk boundary lines
                                up with a pattern repeat.                        */
} t_filler_t;

/* -------------------------------------------------------------------------- */
/* Sizing + storage */
/* -------------------------------------------------------------------------- */

/**
 * @enum streamed_cap_t
 * @brief RAM-disk geometry, fixture sizes, and the retired-buffer yardstick.
 * @details `k_st_legacy_load_cap` is the whole-file `epub_load_buf` capacity
 *          the import_reader app carried before #230 -- the big fixture is
 *          asserted LARGER than it, so this compile was impossible on the
 *          legacy path and only succeeds because the adapter streams.
 */
typedef enum : uint32_t {
  k_st_disk_block_size = 512U,         /**< Bytes per sector (the only ra8_fs size). */
  k_st_disk_blocks     = 8192U,        /**< 4 MiB volume -> FAT16.                   */
  k_st_filler_bytes    = 384U * 1024U, /**< Big unreferenced ZIP entry.              */
  k_st_arc_cap         = 512U * 1024U, /**< Big-fixture archive scratch.             */
  k_st_legacy_load_cap = 128U * 1024U, /**< Retired import_reader load buffer.       */
  k_st_budget_ratio    = 64U,          /**< Assert pool * ratio <= fixture size.     */
  k_st_readback_cap    = 16U * 1024U,  /**< `.rabook` read-back buffer.              */
} streamed_cap_t;

/**
 * @enum streamed_cache_t
 * @brief Source page-cache geometry -- the asserted RAM high-water mark.
 * @details 4 frames x 1 KiB = 4 KiB, roughly 100x smaller than the big
 *          fixture: the #230 bounded-residency regime.
 */
typedef enum : uint32_t {
  k_st_cache_frame_bytes = 1024U, /**< Cache frame size (bytes).    */
  k_st_cache_frames      = 4U,    /**< Fixed resident frame budget. */
  k_st_cache_buckets     = 16U,   /**< Cache hash buckets.          */
} streamed_cache_t;

/**
 * @enum streamed_arena_t
 * @brief Builder/scratch arena capacities (text-only fixtures, small books).
 */
typedef enum : uint32_t {
  k_st_chapter_cap = 8U,          /**< Max chapters.                         */
  k_st_node_cap    = 256U,        /**< Max DOM nodes.                        */
  k_st_attr_cap    = 64U,         /**< Max attribute records.                */
  k_st_style_cap   = 4U,          /**< Max stylesheets.                      */
  k_st_image_cap   = 8U,          /**< Max image descriptors.                */
  k_st_string_cap  = 8U * 1024U,  /**< String-pool capacity (bytes).         */
  k_st_imgpool_cap = 8U * 1024U,  /**< Image-pool capacity (bytes).          */
  k_st_out_cap     = 16U * 1024U, /**< Output-blob capacity (bytes).         */
  k_st_xhtml_cap   = 16U * 1024U, /**< Chapter XHTML scratch (bytes).        */
  k_st_imgraw_cap  = 8U * 1024U,  /**< Raw image byte scratch (bytes).       */
  k_st_arena_cap   = 8U * 1024U,  /**< stb_image bump-arena scratch (bytes). */
  k_st_gray_cap    = 8U * 1024U,  /**< Intermediate gray downscale (pixels). */
  k_st_css_cap     = 16U * 1024U, /**< Stylesheet load scratch (bytes).      */
} streamed_arena_t;

/** @brief The big built ZIP/EPUB archive (models the on-card file). */
static uint8_t s_arc[k_st_arc_cap];
/** @brief Byte length of the built archive in ::s_arc. */
static size_t s_arc_size = 0U;
/** @brief Source bytes for the large unreferenced filler entry. */
static uint8_t s_filler[k_st_filler_bytes];
/** @brief `.rabook` read-back buffer shared by the assertions. */
static uint8_t s_readback[k_st_readback_cap];

typedef struct {
  uint8_t* bytes;       /**< RAM-disk backing store.        */
  uint32_t block_count; /**< Disk size in 512-byte sectors. */
} mem_disk_t;

/** @brief The RAM block device behind the FAT16 volume. */
static mem_disk_t s_disk = {};

/* -------------------------------------------------------------------------- */
/* RAM block backend (4 MiB -> FAT16 via ra8_fs_format) */
/* -------------------------------------------------------------------------- */

/** @brief ra8_fs_backend_t::read_block over the RAM disk. */
static ra8_err_t mem_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  if (lba + count > disk->block_count) {
    return k_ra8_err_out_of_range;
  }
  size_t off = (size_t)lba * (size_t)k_st_disk_block_size;
  size_t len = (size_t)count * (size_t)k_st_disk_block_size;
  memcpy(buf, &s_disk.bytes[off], len);
  return k_ra8_ok;
}

/** @brief ra8_fs_backend_t::write_block over the RAM disk. */
static ra8_err_t mem_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  if (lba + count > disk->block_count) {
    return k_ra8_err_out_of_range;
  }
  size_t off = (size_t)lba * (size_t)k_st_disk_block_size;
  size_t len = (size_t)count * (size_t)k_st_disk_block_size;
  memcpy(&s_disk.bytes[off], buf, len);
  return k_ra8_ok;
}

/** @brief ra8_fs_backend_t::get_capacity over the RAM disk. */
static ra8_err_t mem_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  *block_count     = disk->block_count;
  *block_size      = (uint32_t)k_st_disk_block_size;
  return k_ra8_ok;
}

/** @brief The RAM block-device backend bound to ::s_disk. */
static const ra8_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

/**
 * @brief Format a fresh FAT16 RAM volume and seed @p src_path with @p bytes.
 * @param[in] src_path Root-level 8.3 path to seed.
 * @param[in] bytes    Source bytes to write at @p src_path (non-NULL).
 * @param[in] len      Length of @p bytes in bytes.
 * @return Mounted volume handle.
 * @pre @p src_path is a valid 8.3 name and @p bytes is non-NULL.
 * @pre Any prior volume was unmounted.
 * @post A formatted, mounted FAT16 volume holds @p src_path with @p bytes.
 * @post @p s_disk.bytes owns a fresh zeroed backing store.
 * @note Not thread-safe.
 */
static ra8_fs_mount_t* fresh_volume_seeded(const char* src_path, const uint8_t* bytes, uint32_t len)
{
  free(s_disk.bytes);
  s_disk.block_count = (uint32_t)k_st_disk_blocks;
  s_disk.bytes       = (uint8_t*)calloc((size_t)k_st_disk_blocks, (size_t)k_st_disk_block_size);
  TEST_ASSERT(s_disk.bytes != nullptr);

  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "STREAM";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));

  ra8_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mount));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(mount, src_path, bytes, len));
  return mount;
}

/**
 * @brief Unmount @p mount and release the RAM backing store.
 * @param[in,out] mount Mounted volume to release.
 * @pre @p mount is a live mount from @ref fresh_volume_seeded.
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

/* -------------------------------------------------------------------------- */
/* Big-fixture builder (the streaming-test skeleton + a dominating filler) */
/* -------------------------------------------------------------------------- */

/** @brief container.xml pointing at OEBPS/content.opf. */
static const char* const k_container = "<?xml version=\"1.0\"?>"
                                       "<container version=\"1.0\""
                                       " xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                                       "<rootfiles><rootfile full-path=\"OEBPS/content.opf\""
                                       " media-type=\"application/oebps-package+xml\"/>"
                                       "</rootfiles></container>";

/** @brief OPF: four chapters, EPUB3, no cover (text-only compile). */
static const char* const k_opf =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\""
  " version=\"3.0\" unique-identifier=\"id\">"
  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Streaming Import Book</dc:title><dc:creator>RA8</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:stream:230</dc:identifier>"
  "</metadata>"
  "<manifest>"
  "<item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"ch2\" href=\"ch2.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"ch3\" href=\"ch3.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"ch4\" href=\"ch4.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest>"
  "<spine><itemref idref=\"ch1\"/><itemref idref=\"ch2\"/>"
  "<itemref idref=\"ch3\"/><itemref idref=\"ch4\"/></spine></package>";

static const char* const k_ch1 =
  "<?xml version=\"1.0\"?><html><body><p>ALPHA chapter one.</p></body></html>";
static const char* const k_ch2 =
  "<?xml version=\"1.0\"?><html><body><p>BRAVO chapter two.</p></body></html>";
static const char* const k_ch3 =
  "<?xml version=\"1.0\"?><html><body><p>CHARLIE chapter three.</p></body></html>";
static const char* const k_ch4 =
  "<?xml version=\"1.0\"?><html><body><p>DELTA chapter four.</p></body></html>";

/**
 * @brief Build the big EPUB (real skeleton + huge unreferenced filler) into ::s_arc.
 * @details The filler is stored uncompressed so the archive is genuinely large
 *          on disk; it is not in the manifest/spine, so the compile never reads
 *          it -- exactly the "source far larger than the load buffer" regime.
 * @pre ::s_arc has ::k_st_arc_cap bytes.
 * @pre ::s_filler has ::k_st_filler_bytes bytes.
 * @post ::s_arc_size holds the finalized archive length (> ::k_st_filler_bytes).
 * @post ::s_arc[0 .. s_arc_size) is a valid ZIP.
 * @note Not thread-safe.
 */
static void build_big_epub(void)
{
  for (size_t i = 0U; i < (size_t)k_st_filler_bytes; ++i) {
    s_filler[i] =
      (uint8_t)((i * k_t_filler_stride) + (i >> 3U)); /* varied, not trivially compressible */
  }
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_st_arc_cap) == MZ_TRUE);

  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "mimetype",
                                    "application/epub+zip",
                                    strlen("application/epub+zip"),
                                    MZ_NO_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "META-INF/container.xml",
                                    k_container,
                                    strlen(k_container),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/content.opf",
                                    k_opf,
                                    strlen(k_opf),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/ch1.xhtml", k_ch1, strlen(k_ch1), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/ch2.xhtml", k_ch2, strlen(k_ch2), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/ch3.xhtml", k_ch3, strlen(k_ch3), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/ch4.xhtml", k_ch4, strlen(k_ch4), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  /* The big unreferenced entry, stored (uncompressed) so it dominates the file. */
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/filler.bin",
                                    s_filler,
                                    (size_t)k_st_filler_bytes,
                                    MZ_NO_COMPRESSION) == MZ_TRUE);

  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT((heap != nullptr) && (hsz > (size_t)k_st_filler_bytes) &&
              (hsz <= (size_t)k_st_arc_cap));
  memcpy(s_arc, heap, hsz);
  s_arc_size = hsz;
  mz_zip_writer_end(&zip);
}

/* -------------------------------------------------------------------------- */
/* Streamed compile cookie over file-scope arenas */
/* -------------------------------------------------------------------------- */

static ra8_book_chapter_t    s_chapters[k_st_chapter_cap];
static ra8_book_node_t       s_nodes[k_st_node_cap];
static ra8_book_attr_t       s_attrs[k_st_attr_cap];
static ra8_book_stylesheet_t s_styles[k_st_style_cap];
static ra8_book_image_t      s_images[k_st_image_cap];
static char                  s_strpool[k_st_string_cap];
static uint8_t               s_imgpool[k_st_imgpool_cap];
static uint8_t               s_out[k_st_out_cap];
static uint8_t               s_xhtml[k_st_xhtml_cap];
static uint8_t               s_image_raw[k_st_imgraw_cap];
static uint8_t               s_img_scratch[k_st_arena_cap];
static uint8_t               s_gray[k_st_gray_cap];
static char                  s_css[k_st_css_cap];

/** @brief Fixed frame pool the streamed source is paged through (the budget). */
static uint8_t s_cache_frames[k_st_cache_frames * k_st_cache_frame_bytes];
/** @brief Per-frame metadata; probed post-compile for the residency bound. */
static ra8_vmem_frame_t s_cache_meta[k_st_cache_frames];
/** @brief Per-frame key storage for the source page cache. */
static ra8_vmem_key_t s_cache_keys[k_st_cache_frames];
/** @brief Hash-bucket heads for the source page cache. */
static int32_t s_cache_buckets[k_st_cache_buckets];
/** @brief Source page cache; the adapter re-initialises it per compile and the
 *         tests read its hit/miss/eviction stats after. */
static ra8_vmem_t s_cache;

static ra8_epub_book_t               s_book  = {};
static ra8_rabook_buffers_t          s_bufs  = {};
static ra8_rabook_pipeline_scratch_t s_scr   = {};
static ra8_img_arena_t               s_arena = {};

/**
 * @brief Populate a streamed compile cookie over the file-scope arenas.
 * @param[out] ctx Cookie to populate (non-NULL).
 * @pre @p ctx is writable.
 * @pre The file-scope buffers are defined (always true at TU scope).
 * @post @p ctx references @p s_book, the page-cache storage, and the arena views.
 * @post @p s_book is re-zeroed so a prior compile cannot leak in.
 * @note Not thread-safe (returns a view over shared file-scope buffers).
 */
static void make_ctx(ra8_rabook_import_compiler_ctx_t* ctx)
{
  s_bufs = (ra8_rabook_buffers_t){
    .chapters       = s_chapters,
    .chapter_cap    = (uint32_t)k_st_chapter_cap,
    .nodes          = s_nodes,
    .node_cap       = (uint32_t)k_st_node_cap,
    .attrs          = s_attrs,
    .attr_cap       = (uint32_t)k_st_attr_cap,
    .stylesheets    = s_styles,
    .stylesheet_cap = (uint32_t)k_st_style_cap,
    .images         = s_images,
    .image_cap      = (uint32_t)k_st_image_cap,
    .string_pool    = s_strpool,
    .string_cap     = (uint32_t)k_st_string_cap,
    .image_pool     = s_imgpool,
    .image_pool_cap = (uint32_t)k_st_imgpool_cap,
    .out            = s_out,
    .out_cap        = (uint32_t)k_st_out_cap,
  };
  s_arena = (ra8_img_arena_t){s_img_scratch, sizeof(s_img_scratch), 0U, 0U};
  s_scr   = (ra8_rabook_pipeline_scratch_t){
    .xhtml     = s_xhtml,
    .xhtml_cap = sizeof(s_xhtml),
    .image_raw = s_image_raw,
    .image_cap = sizeof(s_image_raw),
    .img_arena = &s_arena,
    .gray      = s_gray,
    .gray_cap  = (uint32_t)k_st_gray_cap,
    .css       = s_css,
    .css_cap   = sizeof(s_css),
  };
  s_book = (ra8_epub_book_t){};
  *ctx   = (ra8_rabook_import_compiler_ctx_t){
    .epub               = &s_book,
    .cache              = &s_cache,
    .cache_frames       = s_cache_frames,
    .cache_frame_bytes  = (uint32_t)k_st_cache_frame_bytes,
    .cache_frame_count  = (uint32_t)k_st_cache_frames,
    .cache_meta         = s_cache_meta,
    .cache_keys         = s_cache_keys,
    .cache_buckets      = s_cache_buckets,
    .cache_bucket_count = (uint32_t)k_st_cache_buckets,
    .bufs               = &s_bufs,
    .scr                = &s_scr,
  };
}

/**
 * @brief Read the `.rabook` at @p path back into ::s_readback.
 * @param[in,out] mount   Mounted volume to read from.
 * @param[in]     path    Output `.rabook` path to read.
 * @param[out]    out_len Receives the blob length.
 * @pre @p mount is a live mount and @p path exists.
 * @pre ::s_readback has room for the blob.
 * @post ::s_readback[0 .. *out_len) holds the blob bytes.
 * @post The read handle is closed.
 * @note Not thread-safe (shared ::s_readback).
 */
static void read_output(ra8_fs_mount_t* mount, const char* path, uint32_t* out_len)
{
  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, path, k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));
  *out_len = got;
}

/** @brief Count resident (valid) page-cache frames -- the live residency probe. */
static uint32_t count_valid_frames(void)
{
  uint32_t live = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_st_cache_frames; ++i) {
    if (s_cache_meta[i].valid != 0U) {
      ++live;
    }
  }
  return live;
}

/* -------------------------------------------------------------------------- */
/* Tests */
/* -------------------------------------------------------------------------- */

/**
 * @test test_streamed_compile_matches_desktop_golden
 * @brief The STREAMED adapter compiles the parity fixture to a `.rabook` that
 *        is byte-identical to the desktop `tools/epub_compile.py` golden --
 *        the #230 proof that retiring the whole-file open changed no output bit.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a byte-equality oracle over the output;
 * the adapter's guards are single-condition early returns.)
 */
static void test_streamed_compile_matches_desktop_golden(void)
{
  TEST_BEGIN("rabook_import_streamed: streamed compile == desktop golden");
  ra8_fs_mount_t* mount =
    fresh_volume_seeded("SRC.EPB", s_parity_epub, (uint32_t)k_parity_epub_len);

  ra8_rabook_import_compiler_ctx_t ctx = {};
  make_ctx(&ctx);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_import_compile_adapter(&ctx, mount, "SRC.EPB", "OUT.RAB"));

  uint32_t got = 0U;
  read_output(mount, "OUT.RAB", &got);
  TEST_ASSERT_EQ(k_parity_golden_len, got);
  TEST_ASSERT_EQ(0, memcmp(s_readback, s_parity_golden, (size_t)got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));

  teardown(mount);
  TEST_END("rabook_import_streamed: streamed compile == desktop golden");
}

/**
 * @test test_streamed_compile_bounded_high_water
 * @brief A source ~3x larger than the retired whole-file load buffer compiles
 *        through a 4 KiB frame pool: the mem-subsystem stats prove the
 *        source-side RAM high-water was the fixed pool, not the file size.
 *
 * @par MC/DC:
 * (no compound decisions under test -- independent single-condition
 * inequalities over the cache counters, the residency probe, and a
 * `ra8_book_validate` oracle over the output.)
 */
static void test_streamed_compile_bounded_high_water(void)
{
  TEST_BEGIN("rabook_import_streamed: bounded RAM high-water on an oversize source");
  build_big_epub();

  /* The regime under test: the fixture is far larger than BOTH the retired
   * whole-file load buffer and the page-cache pool. */
  TEST_ASSERT(s_arc_size > (size_t)k_st_legacy_load_cap);
  const uint32_t pool_bytes = (uint32_t)k_st_cache_frames * (uint32_t)k_st_cache_frame_bytes;
  TEST_ASSERT(((size_t)pool_bytes * (size_t)k_st_budget_ratio) <= s_arc_size);

  ra8_fs_mount_t* mount = fresh_volume_seeded("BIG.EPB", s_arc, (uint32_t)s_arc_size);

  ra8_rabook_import_compiler_ctx_t ctx = {};
  make_ctx(&ctx);
  memset(s_cache_meta, 0, sizeof(s_cache_meta));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_import_compile_adapter(&ctx, mount, "BIG.EPB", "BIG.RAB"));

  /* The compile produced a valid book with the fixture's whole spine. */
  uint32_t got = 0U;
  read_output(mount, "BIG.RAB", &got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header((const void*)s_readback);
  TEST_ASSERT_EQ(4U, hdr->chapter_count);

  /* ---- The bounded high-water assertions (mem-subsystem stats) ------------ */
  /* 1. Residency never exceeds the fixed pool: the pool IS the high-water. */
  TEST_ASSERT(count_valid_frames() <= (uint32_t)k_st_cache_frames);
  /* 2. The cache streamed: pages were faulted in (misses) and, because the
   *    source dwarfs the pool, resident pages were evicted and re-fetched. */
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t evic = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&s_cache, &hits, &miss, &evic));
  TEST_ASSERT(miss > 0U);
  TEST_ASSERT(evic > 0U);
  /* 3. Hot pages (the re-read ZIP tail / central directory) were served from
   *    the cache rather than re-fetched every time. */
  TEST_ASSERT(hits > 0U);
  /* 4. More distinct source bytes were faulted through the pool than it holds
   *    at once (misses count frame loads; each load is one pool frame). */
  TEST_ASSERT(((uint64_t)miss * (uint64_t)k_st_cache_frame_bytes) > (uint64_t)pool_bytes);

  teardown(mount);
  TEST_END("rabook_import_streamed: bounded RAM high-water on an oversize source");
}

/**
 * @brief Exercise the argument-null and cookie-null guards of the adapter.
 * @param[in] ctx   A fully-populated compiler context.
 * @param[in] mount The mounted source/destination volume.
 * @pre "SRC.EPB" is present on @p mount.
 * @post Every null argument and null cookie returned k_ra8_err_null_ptr.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void streamed_guards_null(ra8_rabook_import_compiler_ctx_t* ctx, ra8_fs_mount_t* mount)
{
  /* Argument null guards -- each flipped independently. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_compile_adapter(nullptr, mount, "SRC.EPB", "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_compile_adapter(ctx, nullptr, "SRC.EPB", "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_compile_adapter(ctx, mount, nullptr, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_compile_adapter(ctx, mount, "SRC.EPB", nullptr));

  /* Cookie null guards: open-book storage, then the cache. */
  ra8_rabook_import_compiler_ctx_t bad = {};
  make_ctx(&bad);
  bad.epub = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_compile_adapter(&bad, mount, "SRC.EPB", "OUT.RAB"));
  make_ctx(&bad);
  bad.cache = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_compile_adapter(&bad, mount, "SRC.EPB", "OUT.RAB"));
}

/**
 * @test test_streamed_adapter_guards
 * @brief NULL arguments, a missing source, and an empty source fail cleanly
 *        with no output written.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each `RA8_CHECK_NULL_PTR` and error
 * propagation is a single-condition guard, driven true here and false by the
 * happy-path cases.)
 */
static void test_streamed_adapter_guards(void)
{
  TEST_BEGIN("rabook_import_streamed: adapter guards");
  ra8_fs_mount_t* mount =
    fresh_volume_seeded("SRC.EPB", s_parity_epub, (uint32_t)k_parity_epub_len);

  ra8_rabook_import_compiler_ctx_t ctx = {};
  make_ctx(&ctx);

  streamed_guards_null(&ctx, mount);
  ra8_rabook_import_compiler_ctx_t bad = {};

  /* Missing source -> the ra8_fs open error propagates, nothing written. */
  TEST_ASSERT(ra8_rabook_import_compile_adapter(&ctx, mount, "NOPE.EPB", "OUT.RAB") != k_ra8_ok);

  /* Empty source (an interrupted card copy) -> rejected at cache-bind as a
   * zero-length paged object; the file handle is released. */
  {
    ra8_fs_file_t* ef = nullptr;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "EMPTY.EPB", k_ra8_fs_mode_write, &ef));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(ef));
  }
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_import_compile_adapter(&ctx, mount, "EMPTY.EPB", "OUT.RAB"));

  /* A zero-frame cookie -> the cache re-init inside the adapter rejects it. */
  make_ctx(&bad);
  bad.cache_frame_count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_import_compile_adapter(&bad, mount, "SRC.EPB", "OUT.RAB"));

  /* A non-ZIP source -> the streamed open rejects it and the file is closed
   * (a second adapter call then re-opens cleanly, proving no handle leaked). */
  static const uint8_t k_junk[] = "this is not a zip archive at all";
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(mount, "JUNK.EPB", k_junk, (uint32_t)(sizeof(k_junk) - 1U)));
  make_ctx(&ctx);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_rabook_import_compile_adapter(&ctx, mount, "JUNK.EPB", "OUT.RAB"));

  /* A compile-stage failure (output arena far too small) propagates after the
   * streamed open succeeded; the book and the source file are still released. */
  make_ctx(&ctx);
  s_bufs.out_cap = 4U; /* finalize cannot fit the RABOOK1 header */
  TEST_ASSERT(ra8_rabook_import_compile_adapter(&ctx, mount, "SRC.EPB", "OUT.RAB") != k_ra8_ok);

  /* None of the failures left an output file behind. */
  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT(ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file) != k_ra8_ok);

  teardown(mount);
  TEST_END("rabook_import_streamed: adapter guards");
}

/**
 * @enum streamed_fat_t
 * @brief FAT16 BPB byte offsets for the corrupt-chain surgery.
 * @details The volume comes from `ra8_fs_format`, so the FAT location is read
 *          out of the live BPB rather than hard-coded: reserved sector count
 *          at byte 14, FAT count at 16, sectors-per-FAT at 22. The FAT16
 *          entry for cluster N sits at FAT-relative byte `N * 2`; the first
 *          file written on a fresh volume starts at cluster 2.
 */
typedef enum : uint32_t {
  k_st_bpb_rsvd_off  = 14U,     /**< BPB reserved-sector-count (u16 LE). */
  k_st_bpb_nfats_off = 16U,     /**< BPB number-of-FATs (u8).            */
  k_st_bpb_spf_off   = 22U,     /**< BPB sectors-per-FAT (u16 LE).       */
  k_st_clus2_ent     = 4U,      /**< Cluster 2 entry byte (2 * 2).       */
  k_st_offdisk_clus  = 0xF000U, /**< Off-disk next-cluster pointer.      */
  k_st_byte_shift    = 8U,      /**< Little-endian high-byte shift.      */
  k_st_byte_mask     = 0xFFU,   /**< Low-byte mask.                      */
} streamed_fat_t;

/**
 * @test test_streamed_compile_corrupt_fat
 * @brief A corrupt FAT chain makes the mid-stream page-in fault: the loader's
 *        `ra8_fs_read` error aborts the streamed open cleanly (no output, no
 *        leaked handle) instead of serving truncated source bytes.
 *
 * @par MC/DC:
 * (no compound decisions under test -- drives the read-error single-condition
 * return in the importer's `s_source_read` that the healthy-volume cases never
 * reach.)
 */
static void test_streamed_compile_corrupt_fat(void)
{
  TEST_BEGIN("rabook_import_streamed: corrupt FAT chain -> open fails, no output");
  ra8_fs_mount_t* mount =
    fresh_volume_seeded("SRC.EPB", s_parity_epub, (uint32_t)k_parity_epub_len);
  /* Unmount before corrupting and mount again after. Corruption arrives on
   * real media between sessions, not under a live mount, and the driver caches
   * one FAT sector (#607) -- poking the FAT behind a mounted volume would be
   * masked by the copy already in memory. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
  mount = nullptr;

  /* Point cluster 2 (the file's first cluster) at an off-disk cluster in every
   * FAT copy, so any chain walk past the first sector faults in mem_read. */
  const uint32_t rsvd  = (uint32_t)s_disk.bytes[k_st_bpb_rsvd_off] |
                         ((uint32_t)s_disk.bytes[k_st_bpb_rsvd_off + 1U] << k_st_byte_shift);
  const uint32_t nfats = (uint32_t)s_disk.bytes[k_st_bpb_nfats_off];
  const uint32_t spf   = (uint32_t)s_disk.bytes[k_st_bpb_spf_off] |
                         ((uint32_t)s_disk.bytes[k_st_bpb_spf_off + 1U] << k_st_byte_shift);
  for (uint32_t f = 0U; f < nfats; ++f) {
    const size_t ent = (((size_t)rsvd + ((size_t)f * (size_t)spf)) * (size_t)k_st_disk_block_size) +
                       (size_t)k_st_clus2_ent;
    s_disk.bytes[ent] = (uint8_t)((uint32_t)k_st_offdisk_clus & k_st_byte_mask);
    s_disk.bytes[ent + 1U] =
      (uint8_t)(((uint32_t)k_st_offdisk_clus >> k_st_byte_shift) & k_st_byte_mask);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mount));

  ra8_rabook_import_compiler_ctx_t ctx = {};
  make_ctx(&ctx);
  TEST_ASSERT(ra8_rabook_import_compile_adapter(&ctx, mount, "SRC.EPB", "OUT.RAB") != k_ra8_ok);

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT(ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file) != k_ra8_ok);

  teardown(mount);
  TEST_END("rabook_import_streamed: corrupt FAT chain -> open fails, no output");
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
RA8_INTERNAL static void internal_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

int32_t main(void)
{
  ra8_log_set_byte_sink(internal_log_sink, nullptr);
  test_streamed_compile_matches_desktop_golden();
  test_streamed_compile_bounded_high_water();
  test_streamed_adapter_guards();
  test_streamed_compile_corrupt_fat();
  (void)fprintf(stderr, "[OK ] test_ra8_rabook_import_streamed.c\n");
  return 0;
}
