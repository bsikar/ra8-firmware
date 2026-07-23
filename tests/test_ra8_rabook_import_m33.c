/**
 * @file test_ra8_rabook_import_m33.c
 * @brief Host test for the M33-offload import adapter (ra8_rabook_import, #149).
 *
 * @par Tag
 * [Ring 4 / Test] {World: NS}
 *
 * @details
 * Exercises @ref ra8_rabook_import_compile_adapter_m33 -- the import seam binding
 * that dispatches a compile to the Cortex-M33 instead of compiling in-core --
 * with a MOCK @ref ra8_dual_core_compile_dispatch_fn, so the adapter's read ->
 * dispatch -> validate -> write logic is covered without a second core:
 *
 *  - Happy path: a mock that returns the known-good parity golden blob -> the
 *    adapter validates it and writes it to the out path; the file reads back
 *    byte-identical and passes @ref ra8_book_validate.
 *  - Reject path: a mock that returns a corrupted blob -> @ref ra8_book_validate
 *    fails inside the adapter, it returns an error, and NO output file is left
 *    (the manager would fall back without poisoning the cache).
 *  - Propagate path: a mock that returns a dispatch error with NO fallback bound
 *    -> the adapter returns that error verbatim and writes nothing.
 *  - Null-guard path: a NULL cookie field -> @ref k_ra8_err_null_ptr.
 *  - Fallback path: a mock that returns a TIMEOUT/FAULT (`k_ra8_err_hw_error`) or a
 *    transport overflow (`k_ra8_err_no_mem`) WITH an in-core fallback cookie bound
 *    -> the adapter compiles the real fixture `.epub` IN-CORE and writes a blob
 *    byte-identical to the desktop golden (proving the appliance still produces a
 *    valid `.rabook` when the offload fails).
 *  - Oversize-source path (#230): a source larger than the cross-core transport
 *    buffer is reported as a transport overflow (`k_ra8_err_no_mem`) BEFORE any
 *    dispatch, and the streamed in-core fallback -- which has no size ceiling --
 *    still imports it byte-identically to the golden.
 *  - No-fallback-on-clean: a golden-returning mock with a deliberately POISONED
 *    fallback -> the M33 result is used directly and the fallback is never touched.
 *  - No-fallback-on-other-error: a mock returning a non-offload error
 *    (`k_ra8_err_invalid_arg`) WITH a valid fallback bound -> the error propagates
 *    and the in-core path is NOT taken.
 *
 * @par MC/DC:
 * Cited decision: libs/ra8_rabook_import/src/ra8_rabook_import_compiler.c@s_is_dispatch_failure
 * The fallback gate `s_is_dispatch_failure(err)` has one compound decision,
 * `(err == k_ra8_err_hw_error) || (err == k_ra8_err_no_mem)` (2 conditions). The
 * fallback tests supply its N+1 = 3 vectors:
 * - hw_error (condition 1 true)  -> falls back  (`...falls_back_on_timeout`)
 * - no_mem   (condition 2 true)  -> falls back  (`...falls_back_on_oom`)
 * - invalid_arg (both false)     -> propagates  (`...no_fallback_on_other_error`)
 * Pairs (hw_error, invalid_arg) and (no_mem, invalid_arg) each prove one
 * condition independently drives the outcome. Every other adapter branch is a
 * single-condition `if (err ...)` or `RA8_CHECK_NULL_PTR` guard, covered by driving
 * its one condition both ways across the cases above.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB Import] {World: NS}
 *
 * @since Version 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * @enum t_import_t
 * @brief Tamper mask applied to the imported body.
 */
typedef enum : uint8_t {
  k_t_tamper_mask = 0xFFU, /**< XOR that flips the last body byte so the CRC
                                check must fail.                                */
} t_import_t;

/* -------------------------------------------------------------------------- */
/* Sizing + storage */
/* -------------------------------------------------------------------------- */

/**
 * @enum import_m33_cap_t
 * @brief RAM-disk geometry and adapter buffer capacities.
 */
typedef enum : uint32_t {
  k_disk_block_size = 512U,        /**< Bytes per sector (the only size ra8_fs accepts). */
  k_disk_blocks     = 8192U,       /**< 4 MiB volume -> count_of_clusters lands FAT16.   */
  k_epub_load_cap   = 4U * 1024U,  /**< Source `.epub` read buffer (bytes).              */
  k_blob_cap        = 16U * 1024U, /**< Dispatched blob buffer (bytes).                  */
  k_readback_cap    = 16U * 1024U, /**< `.rabook` read-back buffer (bytes).              */
} import_m33_cap_t;

static uint8_t s_epub_load[k_epub_load_cap];
static uint8_t s_blob[k_blob_cap];
static uint8_t s_readback[k_readback_cap];

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
  s_disk.block_count = (uint32_t)k_disk_blocks;
  s_disk.bytes       = (uint8_t*)calloc((size_t)k_disk_blocks, (size_t)k_disk_block_size);
  TEST_ASSERT(s_disk.bytes != nullptr);

  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "RABOOK";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));

  ra8_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mount));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(mount, src_path, bytes, len));
  return mount;
}

/**
 * @brief Format a fresh FAT16 RAM volume with a dummy source `.epub` on it.
 * @param[in] src_path Root-level 8.3 path to seed with placeholder `.epub` bytes.
 * @return Mounted volume handle.
 * @pre @p src_path is a valid 8.3 name.
 * @pre Any prior volume was unmounted.
 * @post A formatted, mounted FAT16 volume holds @p src_path.
 * @post @p s_disk.bytes owns a fresh zeroed backing store.
 * @note Not thread-safe. Use for cases where a mock ignores the source bytes.
 */
static ra8_fs_mount_t* fresh_volume_with_epub(const char* src_path)
{
  /* The mock dispatch ignores the bytes; the adapter only needs the read to
   * succeed, so seed an arbitrary non-empty payload. */
  static const uint8_t k_dummy_epub[] = "PK\x03\x04 not-a-real-epub, the M33 mock ignores it";
  return fresh_volume_seeded(src_path, k_dummy_epub, (uint32_t)sizeof(k_dummy_epub));
}

/**
 * @brief Unmount @p mount and release the RAM backing store.
 * @param[in,out] mount Mounted volume to release.
 * @pre @p mount is a live mount from @ref fresh_volume_with_epub.
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
/* Mock dispatch implementations (stand in for the M33 cross-core shim) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Mock dispatch: return the known-good parity golden blob.
 * @param[in]  ctx      Unused mock context.
 * @param[in]  epub     Unused source bytes (the mock does not parse).
 * @param[in]  epub_len Unused source length.
 * @param[out] out_buf  Receives the golden blob.
 * @param[in]  out_cap  Capacity of @p out_buf.
 * @param[out] out_len  Receives the golden blob length.
 * @return @c k_ra8_ok, or @c k_ra8_err_no_mem if @p out_cap is too small.
 * @pre @p out_buf and @p out_len are non-NULL.
 * @pre @p out_cap is the caller's blob buffer capacity.
 * @post On success @p out_buf holds the golden blob and @p *out_len its length.
 * @post @p ctx / @p epub are untouched.
 * @note Stateless; safe to call repeatedly.
 */
static ra8_err_t mock_dispatch_golden(void*          ctx,
                                      const uint8_t* epub,
                                      uint32_t       epub_len,
                                      uint8_t*       out_buf,
                                      uint32_t       out_cap,
                                      uint32_t*      out_len)
{
  (void)ctx;
  (void)epub;
  (void)epub_len;
  if (out_cap < (uint32_t)k_parity_golden_len) {
    return k_ra8_err_no_mem;
  }
  memcpy(out_buf, s_parity_golden, (size_t)k_parity_golden_len);
  *out_len = (uint32_t)k_parity_golden_len;
  return k_ra8_ok;
}

/**
 * @brief Mock dispatch: return the golden blob with one body byte flipped.
 * @param[in]  ctx      Unused mock context.
 * @param[in]  epub     Unused source bytes.
 * @param[in]  epub_len Unused source length.
 * @param[out] out_buf  Receives the corrupted blob.
 * @param[in]  out_cap  Capacity of @p out_buf.
 * @param[out] out_len  Receives the blob length.
 * @return @c k_ra8_ok (a successful dispatch of a bad blob), or @c k_ra8_err_no_mem.
 * @pre @p out_buf and @p out_len are non-NULL.
 * @pre @p out_cap holds at least the golden length.
 * @post @p out_buf holds a blob whose body CRC no longer matches its header.
 * @post @p ctx / @p epub are untouched.
 * @note Models a cross-core transfer slip the adapter must catch via validation.
 */
static ra8_err_t mock_dispatch_corrupt(void*          ctx,
                                       const uint8_t* epub,
                                       uint32_t       epub_len,
                                       uint8_t*       out_buf,
                                       uint32_t       out_cap,
                                       uint32_t*      out_len)
{
  ra8_err_t err = mock_dispatch_golden(ctx, epub, epub_len, out_buf, out_cap, out_len);
  if (err != k_ra8_ok) {
    return err;
  }
  out_buf[*out_len - 1U] ^= k_t_tamper_mask; /* flip the last body byte -> CRC mismatch */
  return k_ra8_ok;
}

/**
 * @brief Mock dispatch: report a worker failure (the M33 never produced a blob).
 * @param[in]  ctx      Unused mock context.
 * @param[in]  epub     Unused source bytes.
 * @param[in]  epub_len Unused source length.
 * @param[out] out_buf  Untouched.
 * @param[in]  out_cap  Unused capacity.
 * @param[out] out_len  Untouched.
 * @return Always @c k_ra8_err_hw_error.
 * @pre None beyond the seam contract.
 * @pre Bound as the cookie's dispatch for the propagate-path test.
 * @post No output is written.
 * @post @p ctx / @p epub / @p out_buf are untouched.
 * @note Models the M33 stalling or faulting before completion.
 */
/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTBEGIN(readability-non-const-parameter)
static ra8_err_t mock_dispatch_error(void*          ctx,
                                     const uint8_t* epub,
                                     uint32_t       epub_len,
                                     uint8_t*       out_buf,
                                     uint32_t       out_cap,
                                     uint32_t*      out_len)
// NOLINTEND(readability-non-const-parameter)
{
  (void)ctx;
  (void)epub;
  (void)epub_len;
  (void)out_buf;
  (void)out_cap;
  (void)out_len;
  return k_ra8_err_hw_error;
}

/**
 * @brief Mock dispatch: report a transport overflow (blob did not fit the shim).
 * @param[in]  ctx      Unused mock context.
 * @param[in]  epub     Unused source bytes.
 * @param[in]  epub_len Unused source length.
 * @param[out] out_buf  Untouched.
 * @param[in]  out_cap  Unused capacity.
 * @param[out] out_len  Untouched.
 * @return Always @c k_ra8_err_no_mem.
 * @pre None beyond the seam contract.
 * @pre Bound as the cookie's dispatch for the OOM-fallback test.
 * @post No output is written.
 * @post @p ctx / @p epub / @p out_buf are untouched.
 * @note Models the dispatched blob exceeding the cross-core transport buffer.
 */
/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTBEGIN(readability-non-const-parameter)
static ra8_err_t mock_dispatch_oom(void*          ctx,
                                   const uint8_t* epub,
                                   uint32_t       epub_len,
                                   uint8_t*       out_buf,
                                   uint32_t       out_cap,
                                   uint32_t*      out_len)
// NOLINTEND(readability-non-const-parameter)
{
  (void)ctx;
  (void)epub;
  (void)epub_len;
  (void)out_buf;
  (void)out_cap;
  (void)out_len;
  return k_ra8_err_no_mem;
}

/**
 * @brief Mock dispatch: report a non-offload error (must NOT trigger fallback).
 * @param[in]  ctx      Unused mock context.
 * @param[in]  epub     Unused source bytes.
 * @param[in]  epub_len Unused source length.
 * @param[out] out_buf  Untouched.
 * @param[in]  out_cap  Unused capacity.
 * @param[out] out_len  Untouched.
 * @return Always @c k_ra8_err_invalid_arg.
 * @pre None beyond the seam contract.
 * @pre Bound as the cookie's dispatch for the no-fallback-on-other-error test.
 * @post No output is written.
 * @post @p ctx / @p epub / @p out_buf are untouched.
 * @note A code outside {hw_error, no_mem} the fallback classifier must reject.
 */
/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTBEGIN(readability-non-const-parameter)
static ra8_err_t mock_dispatch_other(void*          ctx,
                                     const uint8_t* epub,
                                     uint32_t       epub_len,
                                     uint8_t*       out_buf,
                                     uint32_t       out_cap,
                                     uint32_t*      out_len)
// NOLINTEND(readability-non-const-parameter)
{
  (void)ctx;
  (void)epub;
  (void)epub_len;
  (void)out_buf;
  (void)out_cap;
  (void)out_len;
  return k_ra8_err_invalid_arg;
}

/* -------------------------------------------------------------------------- */
/* In-core fallback storage + cookie (the real compile the fallback retries) */
/* -------------------------------------------------------------------------- */

/**
 * @enum incore_cap_t
 * @brief Builder/scratch arena capacities for the in-core fallback compile.
 * @details The parity fixture is text-only (well-formed XHTML, no images), so the
 *          image-pool / raster scratch only need to be non-empty -- the compile
 *          never decodes a bitmap. The text-side pools mirror the in-core pipeline
 *          test's geometry, which compiles this same fixture to the golden blob.
 */
typedef enum : uint32_t {
  k_ic_chapter_cap = 8U,          /**< Max chapters.                         */
  k_ic_node_cap    = 256U,        /**< Max DOM nodes.                        */
  k_ic_attr_cap    = 64U,         /**< Max attribute records.                */
  k_ic_style_cap   = 4U,          /**< Max stylesheets.                      */
  k_ic_image_cap   = 8U,          /**< Max image descriptors.                */
  k_ic_string_cap  = 8U * 1024U,  /**< String-pool capacity (bytes).         */
  k_ic_imgpool_cap = 8U * 1024U,  /**< Image-pool capacity (bytes).          */
  k_ic_out_cap     = 16U * 1024U, /**< Output-blob capacity (bytes).         */
  k_ic_xhtml_cap   = 16U * 1024U, /**< Chapter XHTML scratch (bytes).        */
  k_ic_imgraw_cap  = 8U * 1024U,  /**< Raw image byte scratch (bytes).       */
  k_ic_arena_cap   = 8U * 1024U,  /**< stb_image bump-arena scratch (bytes). */
  k_ic_gray_cap    = 8U * 1024U,  /**< Intermediate gray downscale (pixels). */
  k_ic_css_cap     = 16U * 1024U, /**< Stylesheet load scratch (bytes).      */
} incore_cap_t;

/**
 * @enum incore_cache_t
 * @brief Source page-cache geometry for the streamed in-core compile (#230).
 * @details Deliberately tiny (8 x 1 KiB = 8 KiB) -- the streamed adapter's
 *          source-side RAM budget is this fixed pool, not the book size.
 */
typedef enum : uint32_t {
  k_ic_cache_frame_bytes = 1024U, /**< Cache frame size (bytes).    */
  k_ic_cache_frames      = 8U,    /**< Fixed resident frame budget. */
  k_ic_cache_buckets     = 16U,   /**< Cache hash buckets.          */
} incore_cache_t;

static ra8_book_chapter_t    s_ic_chapters[k_ic_chapter_cap];
static ra8_book_node_t       s_ic_nodes[k_ic_node_cap];
static ra8_book_attr_t       s_ic_attrs[k_ic_attr_cap];
static ra8_book_stylesheet_t s_ic_styles[k_ic_style_cap];
static ra8_book_image_t      s_ic_images[k_ic_image_cap];
static char                  s_ic_strpool[k_ic_string_cap];
static uint8_t               s_ic_imgpool[k_ic_imgpool_cap];
static uint8_t               s_ic_out[k_ic_out_cap];
static uint8_t               s_ic_xhtml[k_ic_xhtml_cap];
static uint8_t               s_ic_image_raw[k_ic_imgraw_cap];
static uint8_t               s_ic_img_scratch[k_ic_arena_cap];
static uint8_t               s_ic_gray[k_ic_gray_cap];
static char                  s_ic_css[k_ic_css_cap];

/** @brief Fixed frame pool the streamed in-core source is paged through. */
static uint8_t s_ic_cache_frames[k_ic_cache_frames * k_ic_cache_frame_bytes];
/** @brief Per-frame metadata for the in-core source page cache. */
static ra8_vmem_frame_t s_ic_cache_meta[k_ic_cache_frames];
/** @brief Hash-bucket heads for the in-core source page cache. */
static int32_t s_ic_cache_buckets[k_ic_cache_buckets];
/** @brief In-core source page cache; re-initialised by the adapter per compile. */
static ra8_vmem_t s_ic_cache;

static ra8_epub_book_t               s_ic_book  = {};
static ra8_rabook_buffers_t          s_ic_bufs  = {};
static ra8_rabook_pipeline_scratch_t s_ic_scr   = {};
static ra8_img_arena_t               s_ic_arena = {};

/**
 * @brief Wire the in-core builder + scratch views over the file-scope arenas.
 * @pre The file-scope in-core buffers are defined (always true at TU scope).
 * @pre Called before each in-core compile so the arena cursor is reset.
 * @post @p s_ic_bufs / @p s_ic_scr reference the arenas with a zeroed cursor.
 * @post No state outside the in-core view structs is mutated.
 * @note Not thread-safe (returns views over shared file-scope buffers).
 */
static void make_incore_views(void)
{
  s_ic_bufs = (ra8_rabook_buffers_t){
    .chapters       = s_ic_chapters,
    .chapter_cap    = (uint32_t)k_ic_chapter_cap,
    .nodes          = s_ic_nodes,
    .node_cap       = (uint32_t)k_ic_node_cap,
    .attrs          = s_ic_attrs,
    .attr_cap       = (uint32_t)k_ic_attr_cap,
    .stylesheets    = s_ic_styles,
    .stylesheet_cap = (uint32_t)k_ic_style_cap,
    .images         = s_ic_images,
    .image_cap      = (uint32_t)k_ic_image_cap,
    .string_pool    = s_ic_strpool,
    .string_cap     = (uint32_t)k_ic_string_cap,
    .image_pool     = s_ic_imgpool,
    .image_pool_cap = (uint32_t)k_ic_imgpool_cap,
    .out            = s_ic_out,
    .out_cap        = (uint32_t)k_ic_out_cap,
  };
  s_ic_arena = (ra8_img_arena_t){s_ic_img_scratch, sizeof(s_ic_img_scratch), 0U, 0U};
  s_ic_scr   = (ra8_rabook_pipeline_scratch_t){
    .xhtml     = s_ic_xhtml,
    .xhtml_cap = sizeof(s_ic_xhtml),
    .image_raw = s_ic_image_raw,
    .image_cap = sizeof(s_ic_image_raw),
    .img_arena = &s_ic_arena,
    .gray      = s_ic_gray,
    .gray_cap  = (uint32_t)k_ic_gray_cap,
    .css       = s_ic_css,
    .css_cap   = sizeof(s_ic_css),
  };
}

/**
 * @brief Populate an in-core fallback cookie over the file-scope arenas.
 * @param[out] ctx Cookie to populate (non-NULL).
 * @pre @p ctx is writable.
 * @pre The file-scope in-core buffers are defined (always true at TU scope).
 * @post @p ctx references @p s_ic_book, the source page-cache storage, and the
 *       wired arena views.
 * @post @p s_ic_book is re-zeroed so a prior compile cannot leak in.
 * @note Not thread-safe (returns a view over shared file-scope buffers).
 */
static void make_incore_ctx(ra8_rabook_import_compiler_ctx_t* ctx)
{
  make_incore_views();
  s_ic_book = (ra8_epub_book_t){};
  *ctx      = (ra8_rabook_import_compiler_ctx_t){
    .epub               = &s_ic_book,
    .cache              = &s_ic_cache,
    .cache_frames       = s_ic_cache_frames,
    .cache_frame_bytes  = (uint32_t)k_ic_cache_frame_bytes,
    .cache_frame_count  = (uint32_t)k_ic_cache_frames,
    .cache_meta         = s_ic_cache_meta,
    .cache_buckets      = s_ic_cache_buckets,
    .cache_bucket_count = (uint32_t)k_ic_cache_buckets,
    .bufs               = &s_ic_bufs,
    .scr                = &s_ic_scr,
  };
}

/**
 * @brief Assert @p mount holds a golden-identical, validated `.rabook` at @p path.
 * @param[in,out] mount Mounted volume to read @p path from.
 * @param[in]     path  Output `.rabook` path to verify.
 * @pre @p mount is a live mount and @p path was just written by the adapter.
 * @pre @p s_readback has room for the golden blob.
 * @post @p s_readback holds the blob bytes read back from @p path.
 * @post The test fails unless the blob equals the desktop golden and validates.
 * @note Not thread-safe (reuses the shared @p s_readback buffer).
 */
static void assert_output_is_golden(ra8_fs_mount_t* mount, const char* path)
{
  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, path, k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));
  TEST_ASSERT_EQ(k_parity_golden_len, got);
  TEST_ASSERT_EQ(0, memcmp(s_readback, s_parity_golden, (size_t)got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
}

/* -------------------------------------------------------------------------- */
/* Tests */
/* -------------------------------------------------------------------------- */

/**
 * @brief Bind a cookie over the file-scope buffers with @p dispatch.
 * @param[out] ctx      Cookie to populate (non-NULL).
 * @param[in]  dispatch Dispatch seam to bind.
 * @pre @p ctx is writable.
 * @pre The file-scope adapter buffers are defined (always true at TU scope).
 * @post @p ctx references @p s_epub_load / @p s_blob and @p dispatch.
 * @post No global state beyond @p ctx is mutated.
 * @note Not thread-safe (returns a view over shared file-scope buffers).
 */
static void make_cookie(ra8_rabook_import_compiler_m33_ctx_t* ctx,
                        ra8_dual_core_compile_dispatch_fn     dispatch)
{
  *ctx = (ra8_rabook_import_compiler_m33_ctx_t){
    .epub_load_buf = s_epub_load,
    .epub_load_cap = (uint32_t)k_epub_load_cap,
    .blob_buf      = s_blob,
    .blob_cap      = (uint32_t)k_blob_cap,
    .dispatch      = dispatch,
    .dispatch_ctx  = nullptr,
  };
}

/**
 * @test A golden-returning dispatch yields a validated, byte-identical .rabook.
 *
 * @par MC/DC:
 * (no compound decision is exercised by this case -- the golden dispatch returns
 * k_ra8_ok, so `s_offload_or_fallback` short-circuits at `if (err == k_ra8_ok)`
 * and the module's sole compound `s_is_dispatch_failure` is never reached; every
 * guard on this happy path -- the RA8_CHECK_NULL_PTR argument checks, the
 * `size > cap` transport check, the `err != k_ra8_ok` checks -- is single-condition)
 */
static void test_m33_adapter_writes_validated_blob(void)
{
  TEST_BEGIN("ra8_rabook_import_m33: golden dispatch -> validated .rabook written");
  ra8_fs_mount_t* mount = fresh_volume_with_epub("SRC.EPB");

  ra8_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_golden);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_import_compile_adapter_m33(&ctx, mount, "SRC.EPB", "OUT.RAB"));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_parity_golden_len, got);
  TEST_ASSERT_EQ(0, memcmp(s_readback, s_parity_golden, (size_t)got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));

  teardown(mount);
  TEST_END("ra8_rabook_import_m33: golden dispatch -> validated .rabook written");
}

/**
 * @test A corrupt dispatched blob is rejected by validation and not written.
 *
 * @par MC/DC:
 * (no independent MC/DC contribution -- the dispatch returns k_ra8_ok but the
 * blob fails `ra8_book_validate`, so `s_fallback_or_propagate` reaches the
 * module's sole compound `s_is_dispatch_failure(err)` =
 * `(err == k_ra8_err_hw_error) || (err == k_ra8_err_no_mem)` only at its
 * both-false control: the validation error is outside {hw_error, no_mem}, the
 * guard is false, and the error propagates with no output written. That
 * decision's N+1 = 3 vectors live in test_m33_adapter_falls_back_on_timeout,
 * _falls_back_on_oom, and _no_fallback_on_other_error)
 */
static void test_m33_adapter_rejects_corrupt_blob(void)
{
  TEST_BEGIN("ra8_rabook_import_m33: corrupt blob -> rejected, no output");
  ra8_fs_mount_t* mount = fresh_volume_with_epub("SRC.EPB");

  ra8_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_corrupt);
  TEST_ASSERT(ra8_rabook_import_compile_adapter_m33(&ctx, mount, "SRC.EPB", "OUT.RAB") != k_ra8_ok);

  /* The validation failure must leave no output file behind. */
  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT(ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file) != k_ra8_ok);

  teardown(mount);
  TEST_END("ra8_rabook_import_m33: corrupt blob -> rejected, no output");
}

/**
 * @test A dispatch error propagates verbatim and writes nothing.
 *
 * @par MC/DC:
 * Decision: `s_is_dispatch_failure(err)` =
 * `(err == k_ra8_err_hw_error) || (err == k_ra8_err_no_mem)` (2 conditions, OR;
 * ra8_rabook_import_compiler.c). The mock returns k_ra8_err_hw_error with no
 * fallback bound, so this case drives C1=T (hw_error -> short-circuit -> true) --
 * the same true leg as test_m33_adapter_falls_back_on_timeout; the N+1 = 3 set is
 * completed by _falls_back_on_oom (C2=T) and _no_fallback_on_other_error (both
 * false). This case additionally drives the single-condition
 * `ctx->fallback == nullptr` propagate arm (fallback NULL -> err returned
 * verbatim, no output written).
 */
static void test_m33_adapter_propagates_dispatch_error(void)
{
  TEST_BEGIN("ra8_rabook_import_m33: dispatch error -> propagated, no output");
  ra8_fs_mount_t* mount = fresh_volume_with_epub("SRC.EPB");

  ra8_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_error);
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_rabook_import_compile_adapter_m33(&ctx, mount, "SRC.EPB", "OUT.RAB"));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT(ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file) != k_ra8_ok);

  teardown(mount);
  TEST_END("ra8_rabook_import_m33: dispatch error -> propagated, no output");
}

/**
 * @test NULL arguments and a NULL cookie field are rejected with null-ptr.
 *
 * @par MC/DC:
 * (no compound decisions in this case -- it drives the single-condition
 * RA8_CHECK_NULL_PTR guards of ra8_rabook_import_compile_adapter_m33: a NULL
 * compile_ctx and a NULL `ctx->dispatch` cookie field each return
 * k_ra8_err_null_ptr; no `&&` or `||` decision is reached)
 */
static void test_m33_adapter_null_guards(void)
{
  TEST_BEGIN("ra8_rabook_import_m33: null guards");
  ra8_fs_mount_t* mount = fresh_volume_with_epub("SRC.EPB");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_compile_adapter_m33(nullptr, mount, "SRC.EPB", "OUT.RAB"));

  ra8_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_golden);
  ctx.dispatch = nullptr; /* a NULL cookie field must be caught */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_compile_adapter_m33(&ctx, mount, "SRC.EPB", "OUT.RAB"));

  teardown(mount);
  TEST_END("ra8_rabook_import_m33: null guards");
}

/**
 * @test A missing source `.epub` surfaces the read error and writes nothing.
 *
 * @par MC/DC:
 * (no independent MC/DC contribution -- the missing source makes `ra8_fs_open`
 * fail inside `s_read_whole_file`, so `s_fallback_or_propagate` reaches the
 * module's sole compound `s_is_dispatch_failure(err)` =
 * `(err == k_ra8_err_hw_error) || (err == k_ra8_err_no_mem)` only at its
 * both-false control: the open error is outside {hw_error, no_mem}, the guard is
 * false, and the error propagates with no output written. That decision's
 * N+1 = 3 vectors live in test_m33_adapter_falls_back_on_timeout,
 * _falls_back_on_oom, and _no_fallback_on_other_error)
 */
static void test_m33_adapter_handles_missing_source(void)
{
  TEST_BEGIN("ra8_rabook_import_m33: missing source -> read error, no output");
  ra8_fs_mount_t* mount = fresh_volume_with_epub("SRC.EPB");

  ra8_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_golden);
  /* NOPE.EPB does not exist -> ra8_fs_open fails inside s_read_whole_file. */
  TEST_ASSERT(ra8_rabook_import_compile_adapter_m33(&ctx, mount, "NOPE.EPB", "OUT.RAB") !=
              k_ra8_ok);

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT(ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file) != k_ra8_ok);

  teardown(mount);
  TEST_END("ra8_rabook_import_m33: missing source -> read error, no output");
}

/**
 * @test A TIMEOUT/FAULT offload (hw_error) with a fallback compiles in-core.
 *
 * @par MC/DC:
 * Supplies the `(err == k_ra8_err_hw_error)` true vector for the fallback gate
 * `s_is_dispatch_failure`; pairs with `...no_fallback_on_other_error` (both-false)
 * to prove condition 1 independently drives the fallback.
 */
static void test_m33_adapter_falls_back_on_timeout(void)
{
  TEST_BEGIN("ra8_rabook_import_m33: hw_error offload -> in-core fallback -> golden");
  ra8_fs_mount_t* mount =
    fresh_volume_seeded("SRC.EPB", s_parity_epub, (uint32_t)k_parity_epub_len);

  ra8_rabook_import_compiler_ctx_t incore = {};
  make_incore_ctx(&incore);

  ra8_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_error); /* returns k_ra8_err_hw_error */
  ctx.fallback = &incore;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_import_compile_adapter_m33(&ctx, mount, "SRC.EPB", "OUT.RAB"));
  assert_output_is_golden(mount, "OUT.RAB");

  teardown(mount);
  TEST_END("ra8_rabook_import_m33: hw_error offload -> in-core fallback -> golden");
}

/**
 * @test A transport-overflow offload (no_mem) with a fallback compiles in-core.
 *
 * @par MC/DC:
 * Supplies the `(err == k_ra8_err_no_mem)` true vector for the fallback gate;
 * pairs with `...no_fallback_on_other_error` (both-false) to prove condition 2
 * independently drives the fallback.
 */
static void test_m33_adapter_falls_back_on_oom(void)
{
  TEST_BEGIN("ra8_rabook_import_m33: no_mem offload -> in-core fallback -> golden");
  ra8_fs_mount_t* mount =
    fresh_volume_seeded("SRC.EPB", s_parity_epub, (uint32_t)k_parity_epub_len);

  ra8_rabook_import_compiler_ctx_t incore = {};
  make_incore_ctx(&incore);

  ra8_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_oom); /* returns k_ra8_err_no_mem */
  ctx.fallback = &incore;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_import_compile_adapter_m33(&ctx, mount, "SRC.EPB", "OUT.RAB"));
  assert_output_is_golden(mount, "OUT.RAB");

  teardown(mount);
  TEST_END("ra8_rabook_import_m33: no_mem offload -> in-core fallback -> golden");
}

/**
 * @test A clean offload result is used directly; the fallback is never invoked.
 *
 * @details The fallback cookie is deliberately POISONED (NULL open-book storage)
 * so an in-core compile would fail. A successful, golden-identical output
 * therefore proves the adapter short-circuited on the clean M33 result without
 * touching it.
 *
 * @par MC/DC:
 * Exercises the adapter's `if (err == k_ra8_ok)` short-circuit (a single-condition
 * branch, no N+1 set required): drives it TRUE so the clean offload result is used
 * and the fallback gate is never reached. The false side is driven by the
 * fallback / propagate cases.
 */
static void test_m33_adapter_uses_clean_result_no_fallback(void)
{
  TEST_BEGIN("ra8_rabook_import_m33: clean offload -> M33 result used, no fallback");
  ra8_fs_mount_t* mount = fresh_volume_with_epub("SRC.EPB");

  /* A fallback that would FAIL if it were ever invoked. */
  ra8_rabook_import_compiler_ctx_t poison = {};
  make_incore_ctx(&poison);
  poison.epub = nullptr;

  ra8_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_golden);
  ctx.fallback = &poison;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_import_compile_adapter_m33(&ctx, mount, "SRC.EPB", "OUT.RAB"));
  assert_output_is_golden(mount, "OUT.RAB");

  teardown(mount);
  TEST_END("ra8_rabook_import_m33: clean offload -> M33 result used, no fallback");
}

/**
 * @test A source larger than the cross-core transport buffer is reported as a
 *       transport overflow BEFORE dispatch, and the streamed in-core fallback
 *       (which has no size ceiling, #230) still imports it to the golden blob.
 *
 * @details The cookie's @p epub_load_cap is shrunk below the parity fixture's
 * length, so `s_read_whole_file` reports `k_ra8_err_no_mem` without ever
 * dispatching (the mock would abort the test if invoked). The classifier treats
 * no_mem as an offload failure, so the in-core fallback streams the FULL source
 * off the volume and the output is byte-identical to the desktop golden --
 * proving an import too big for the M33 transport is not truncated or lost.
 *
 * @par MC/DC:
 * Exercises the new `if (size > cap)` transport-overflow guard in
 * `s_read_whole_file` (a single-condition branch, no N+1 set required): drives
 * it TRUE here; every other m33 case (full-size load buffer) drives it FALSE.
 */
static void test_m33_adapter_falls_back_on_oversize_source(void)
{
  TEST_BEGIN("ra8_rabook_import_m33: oversize source -> overflow -> streamed fallback");
  ra8_fs_mount_t* mount =
    fresh_volume_seeded("SRC.EPB", s_parity_epub, (uint32_t)k_parity_epub_len);

  ra8_rabook_import_compiler_ctx_t incore = {};
  make_incore_ctx(&incore);

  /* mock_dispatch_corrupt would poison the output if the dispatch ever ran; a
   * golden result therefore proves the overflow was caught BEFORE dispatch. */
  ra8_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_corrupt);
  ctx.epub_load_cap = (uint32_t)k_parity_epub_len - 1U; /* source no longer fits */
  ctx.fallback      = &incore;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_import_compile_adapter_m33(&ctx, mount, "SRC.EPB", "OUT.RAB"));
  assert_output_is_golden(mount, "OUT.RAB");

  teardown(mount);
  TEST_END("ra8_rabook_import_m33: oversize source -> overflow -> streamed fallback");
}

/**
 * @test A non-offload error propagates even with a fallback bound.
 *
 * @details `k_ra8_err_invalid_arg` is outside {hw_error, no_mem}, so the classifier
 * must reject it: the error propagates verbatim and the in-core path is NOT taken
 * (proven by the absence of any output file).
 *
 * @par MC/DC:
 * Cited decision: libs/ra8_rabook_import/src/ra8_rabook_import_compiler.c@s_is_dispatch_failure
 * Supplies the both-conditions-false vector for
 * `(err == k_ra8_err_hw_error) || (err == k_ra8_err_no_mem)`: err=invalid_arg makes
 * both conditions false so the gate returns false and the error propagates. Paired
 * with `...falls_back_on_timeout` (hw_error true) and `...falls_back_on_oom`
 * (no_mem true), it completes the N+1 = 3 vector set proving each condition
 * independently drives the fallback.
 */
static void test_m33_adapter_no_fallback_on_other_error(void)
{
  TEST_BEGIN("ra8_rabook_import_m33: non-offload error -> propagated, no fallback");
  ra8_fs_mount_t* mount = fresh_volume_with_epub("SRC.EPB");

  ra8_rabook_import_compiler_ctx_t incore = {};
  make_incore_ctx(&incore); /* a VALID fallback that must NOT be used */

  ra8_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_other); /* returns k_ra8_err_invalid_arg */
  ctx.fallback = &incore;

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rabook_import_compile_adapter_m33(&ctx, mount, "SRC.EPB", "OUT.RAB"));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT(ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file) != k_ra8_ok);

  teardown(mount);
  TEST_END("ra8_rabook_import_m33: non-offload error -> propagated, no fallback");
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
  test_m33_adapter_writes_validated_blob();
  test_m33_adapter_rejects_corrupt_blob();
  test_m33_adapter_propagates_dispatch_error();
  test_m33_adapter_handles_missing_source();
  test_m33_adapter_null_guards();
  test_m33_adapter_falls_back_on_timeout();
  test_m33_adapter_falls_back_on_oom();
  test_m33_adapter_falls_back_on_oversize_source();
  test_m33_adapter_uses_clean_result_no_fallback();
  test_m33_adapter_no_fallback_on_other_error();
  (void)fprintf(stderr, "[OK ] test_ra8_rabook_import_m33.c\n");
  return 0;
}
