/**
 * @file test_ra8_rabook_import.c
 * @brief Host test for the on-import EPUB -> .rabook cache manager (#151).
 *
 * @details
 * Proves the import-and-cache acceptance for #151 on the host without the real
 * compiler: a tiny text-only `.epub` is assembled with miniz and written to a
 * FAT16 volume through `ra8_fs` (over a RAM block backend -- the RAM-disk pattern
 * from `tests/test_ra8_epub_fs.c`). A lightweight compile *spy* is injected
 * through the DIP seam (@ref ra8_rabook_import_compile_fn) so the test can count
 * compiles and inspect the cached bytes directly.
 *
 * The spy never parses the EPUB; that is the point of the seam. The manager
 * still streams the real `.epub` bytes through CRC-32 to key the cache, so the
 * `ra8_fs` read path is exercised end to end.
 *
 * Coverage:
 *   - miss -> compile -> cache -> hit: a second open does NOT recompile
 *     (compile-count spy stays at 1) and returns the same cache path + bytes.
 *   - stale by version bump: a bumped importer version re-derives.
 *   - changed source content: a different `.epub` keys a different cache.
 *   - compile-error fallback: a failing compile leaves no cache (next open is
 *     still a miss) and never touches the source.
 *   - NULL-argument / capacity guards.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB_Import] {World: NS}
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_log.h"
#include "ra8_rabook_import.h"
#include "unity_minimal.h"

/**
 * @brief No-op log byte sink so the logger never touches ITM MMIO on the host.
 * @details Installed in main() before any test runs. With a sink registered,
 *          `internal_itm_ready()` short-circuits to true and the failure-path
 *          `ra8_log_*` calls inside the manager route here instead of poking the
 *          unmapped Cortex-M85 ITM registers (which would SEGV on the host).
 */
static void test_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/* --- RAM block device + a real FAT16 volume via ra8_fs_format --------------- */

/**
 * @enum import_disk_t
 * @brief RAM block-device geometry (4 MiB -> auto-detects FAT16).
 */
typedef enum : uint32_t {
  k_disk_block_size = 512U,  /**< Bytes per sector (the only size ra8_fs accepts). */
  k_disk_blocks     = 8192U, /**< 4 MiB volume -> count_of_clusters lands FAT16.   */
} import_disk_t;

/**
 * @enum import_cap_t
 * @brief Buffer and payload sizing for the test.
 */
typedef enum : size_t {
  k_epub_cap    = 8U * 1024U, /**< In-memory ZIP buffer capacity.                  */
  k_scratch_cap = 512U,       /**< Streaming CRC chunk handed to the manager.      */
  k_path_cap    = 16U,        /**< Cache-path output buffer (holds "BOOK.rabook"). */
  k_read_cap    = 8U * 1024U, /**< Cache read-back buffer.                         */
  k_spy_len     = 8U,         /**< Bytes the compile spy writes per call.          */
} import_cap_t;

/**
 * @enum import_misc_t
 * @brief Misc named constants (avoids magic numbers).
 */
typedef enum : uint32_t {
  k_test_format_version = 1U, /**< Stand-in RABOOK1 format version stamp.           */
  k_importer_version_a  = 1U, /**< First importer/compiler version stamp.           */
  k_importer_version_b  = 2U, /**< Bumped importer version (invalidates).           */
  k_spy_idx_seq         = 7U, /**< Index of the call-sequence byte in the spy blob. */
  k_spy_hdr_len         = 6U, /**< Length of the "RBKSPY" spy header.               */
} import_misc_t;

typedef struct {
  uint8_t* bytes;       /**< Bytes.       */
  uint32_t block_count; /**< Block count. */
} mem_disk_t;

static mem_disk_t s_disk = {};
static uint8_t    s_scratch[k_scratch_cap];
static uint8_t    s_epub[k_epub_cap];
static size_t     s_epub_len;

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

/* --- Compile spy injected through the DIP seam ----------------------------- */

static int     s_compile_calls = 0;
static uint8_t s_last_payload[k_spy_len];

/** @brief Lightweight seam binding: writes a deterministic blob, counts calls. */
static ra8_err_t
spy_compile(void* ctx, ra8_fs_mount_t* mount, const char* epub_path, const char* out_path)
{
  (void)ctx;
  (void)epub_path;
  s_compile_calls++;
  uint8_t payload[k_spy_len] = {};
  memcpy(payload, "RBKSPY", (size_t)k_spy_hdr_len);
  payload[k_spy_idx_seq] = (uint8_t)s_compile_calls;
  memcpy(s_last_payload, payload, sizeof(payload));
  return ra8_fs_write_file(mount, out_path, payload, (uint32_t)sizeof(payload));
}

/** @brief Seam binding that always fails without writing (fallback test). */
static ra8_err_t
spy_compile_fail(void* ctx, ra8_fs_mount_t* mount, const char* epub_path, const char* out_path)
{
  (void)ctx;
  (void)mount;
  (void)epub_path;
  (void)out_path;
  s_compile_calls++;
  return k_ra8_err_hw_error;
}

/* --- Fixtures -------------------------------------------------------------- */

static const char* const k_mimetype = "application/epub+zip";
static const char* const k_container =
  "<?xml version=\"1.0\"?><container version=\"1.0\" "
  "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
  "<rootfile full-path=\"OEBPS/content.opf\" "
  "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
static const char* const k_opf =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
  "unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Tiny</dc:title><dc:identifier id=\"id\">urn:test:1</dc:identifier></metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
  "<spine><itemref idref=\"c1\"/></spine></package>";

/** @brief Build a tiny text-only `.epub` into s_epub; @p body varies content. */
static void build_epub(const char* body)
{
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
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/content.opf",
                                    k_opf,
                                    strlen(k_opf),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/c1.xhtml", body, strlen(body), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT((heap != nullptr) && (hsz > 0U) && (hsz <= sizeof(s_epub)));
  memcpy(s_epub, heap, hsz);
  s_epub_len = hsz;
  mz_zip_writer_end(&zip);
}

/** @brief Format a fresh FAT16 RAM volume and return a mounted handle. */
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

/** @brief (Re)write the current s_epub fixture to @p path on @p mount. */
static void put_source(ra8_fs_mount_t* mount, const char* path)
{
  (void)ra8_fs_unlink(mount, path);
  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, path, k_ra8_fs_mode_write, &file));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(file, s_epub, (uint32_t)s_epub_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));
}

/** @brief Assert the cache file at @p path holds exactly @p want[0..len). */
static void
assert_cache_bytes(ra8_fs_mount_t* mount, const char* path, const uint8_t* want, uint32_t len)
{
  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, path, k_ra8_fs_mode_read, &file));
  uint32_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(file, &size));
  TEST_ASSERT_EQ(len, size);
  uint8_t  got_buf[k_read_cap];
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, got_buf, (uint32_t)sizeof(got_buf), &got));
  TEST_ASSERT_EQ(len, got);
  TEST_ASSERT_EQ(0, memcmp(got_buf, want, (size_t)len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));
}

static ra8_rabook_import_cfg_t
make_cfg(ra8_fs_mount_t* mount, uint32_t importer_version, ra8_rabook_import_compile_fn compile)
{
  ra8_rabook_import_cfg_t cfg = {};
  cfg.mount                   = mount;
  cfg.compile                 = compile;
  cfg.compile_ctx             = nullptr;
  cfg.scratch                 = s_scratch;
  cfg.scratch_cap             = (uint32_t)k_scratch_cap;
  cfg.format_version          = (uint32_t)k_test_format_version;
  cfg.importer_version        = importer_version;
  return cfg;
}

static void teardown(ra8_fs_mount_t* mount)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
}

/* --- Tests ----------------------------------------------------------------- */

/**
 * @test test_miss_then_hit
 * @brief First open compiles + caches; the second open is a cache hit and does
 *        NOT recompile (the compile-count spy stays at 1).
 *
 * @par MC/DC:
 * The freshness gate is a single `memcmp` of the whole stamp (no compound
 * boolean), so there is no compound decision to vector here; this exercises the
 * happy compiled -> hit transition end to end.
 */
static void test_miss_then_hit(void)
{
  TEST_BEGIN("ra8_rabook_import: miss -> compile -> cache -> hit");
  build_epub("<html><body><p>Hello there.</p></body></html>");
  ra8_fs_mount_t* mount = fresh_volume();
  put_source(mount, "BOOK.EPB");

  ra8_rabook_import_cfg_t     cfg = make_cfg(mount, (uint32_t)k_importer_version_a, spy_compile);
  ra8_rabook_import_outcome_t out = k_ra8_rabook_import_hit;
  s_compile_calls                 = 0;

  /* Miss -> compiled. */
  char path1[k_path_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_import_open(&cfg, "BOOK.EPB", path1, (uint32_t)sizeof(path1), &out));
  TEST_ASSERT_EQ(k_ra8_rabook_import_compiled, out);
  TEST_ASSERT_EQ(1, s_compile_calls);
  /* The cache is named after the source's own name, not a CRC-32 hex string. */
  TEST_ASSERT_EQ(0, strcmp(path1, "BOOK.rabook"));
  uint8_t v1[k_spy_len];
  memcpy(v1, s_last_payload, sizeof(v1));
  assert_cache_bytes(mount, path1, v1, (uint32_t)k_spy_len);

  /* Hit -> NOT recompiled, same path + bytes. */
  char path2[k_path_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_import_open(&cfg, "BOOK.EPB", path2, (uint32_t)sizeof(path2), &out));
  TEST_ASSERT_EQ(k_ra8_rabook_import_hit, out);
  TEST_ASSERT_EQ(1, s_compile_calls);
  TEST_ASSERT_EQ(0, strcmp(path1, path2));
  assert_cache_bytes(mount, path2, v1, (uint32_t)k_spy_len);

  teardown(mount);
  TEST_END("ra8_rabook_import: miss -> compile -> cache -> hit");
}

/**
 * @test test_stale_version_recompiles
 * @brief A bumped importer version invalidates the marker, forcing a re-derive
 *        that overwrites the cache with fresh bytes.
 *
 * @par MC/DC:
 * No compound decision under test (single-`memcmp` freshness gate); this proves
 * the version field participates in the stamp identity.
 */
static void test_stale_version_recompiles(void)
{
  TEST_BEGIN("ra8_rabook_import: bumped version re-derives");
  build_epub("<html><body><p>Versioned.</p></body></html>");
  ra8_fs_mount_t* mount = fresh_volume();
  put_source(mount, "BOOK.EPB");
  s_compile_calls = 0;

  ra8_rabook_import_outcome_t out              = k_ra8_rabook_import_hit;
  char                        path[k_path_cap] = {};

  ra8_rabook_import_cfg_t cfg_a = make_cfg(mount, (uint32_t)k_importer_version_a, spy_compile);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_import_open(&cfg_a, "BOOK.EPB", path, (uint32_t)sizeof(path), &out));
  TEST_ASSERT_EQ(k_ra8_rabook_import_compiled, out);
  TEST_ASSERT_EQ(1, s_compile_calls);

  ra8_rabook_import_cfg_t cfg_b = make_cfg(mount, (uint32_t)k_importer_version_b, spy_compile);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_import_open(&cfg_b, "BOOK.EPB", path, (uint32_t)sizeof(path), &out));
  TEST_ASSERT_EQ(k_ra8_rabook_import_compiled, out);
  TEST_ASSERT_EQ(2, s_compile_calls);
  uint8_t v2[k_spy_len];
  memcpy(v2, s_last_payload, sizeof(v2));
  assert_cache_bytes(mount, path, v2, (uint32_t)k_spy_len);

  teardown(mount);
  TEST_END("ra8_rabook_import: bumped version re-derives");
}

/**
 * @test test_content_change_rederives
 * @brief Editing the source bytes changes the freshness stamp's CRC-32, so the
 *        same-named cache is re-derived (fresh compile) and holds the new bytes.
 *
 * @par MC/DC:
 * No compound decision under test; proves the CRC-32 content key still gates
 * freshness now that it no longer names the file (the name follows the source).
 */
static void test_content_change_rederives(void)
{
  TEST_BEGIN("ra8_rabook_import: changed content -> same name, re-derived");
  ra8_fs_mount_t* mount = fresh_volume();
  s_compile_calls       = 0;

  ra8_rabook_import_cfg_t     cfg = make_cfg(mount, (uint32_t)k_importer_version_a, spy_compile);
  ra8_rabook_import_outcome_t out = k_ra8_rabook_import_hit;

  build_epub("<html><body><p>First edition.</p></body></html>");
  put_source(mount, "BOOK.EPB");
  char path_a[k_path_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_import_open(&cfg, "BOOK.EPB", path_a, (uint32_t)sizeof(path_a), &out));
  TEST_ASSERT_EQ(k_ra8_rabook_import_compiled, out);
  TEST_ASSERT_EQ(1, s_compile_calls);

  build_epub("<html><body><p>Second, longer edition with more text.</p></body></html>");
  put_source(mount, "BOOK.EPB");
  char path_b[k_path_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_import_open(&cfg, "BOOK.EPB", path_b, (uint32_t)sizeof(path_b), &out));
  TEST_ASSERT_EQ(k_ra8_rabook_import_compiled, out);
  TEST_ASSERT_EQ(2, s_compile_calls);
  /* Same source name -> same cache name; the changed content forced a compile. */
  TEST_ASSERT_EQ(0, strcmp(path_a, path_b));
  uint8_t v2[k_spy_len];
  memcpy(v2, s_last_payload, sizeof(v2));
  assert_cache_bytes(mount, path_b, v2, (uint32_t)k_spy_len);

  teardown(mount);
  TEST_END("ra8_rabook_import: changed content -> same name, re-derived");
}

/**
 * @test test_compile_error_leaves_no_cache
 * @brief A failing compile is propagated, the source is untouched, and no cache
 *        is published -- a subsequent good open is still a miss.
 *
 * @par MC/DC:
 * No compound decision under test; exercises the error-propagation arm of the
 * miss path.
 */
static void test_compile_error_leaves_no_cache(void)
{
  TEST_BEGIN("ra8_rabook_import: compile error leaves no cache");
  build_epub("<html><body><p>Doomed compile.</p></body></html>");
  ra8_fs_mount_t* mount = fresh_volume();
  put_source(mount, "BOOK.EPB");
  s_compile_calls = 0;

  ra8_rabook_import_outcome_t out              = k_ra8_rabook_import_hit;
  char                        path[k_path_cap] = {};

  ra8_rabook_import_cfg_t cfg_fail =
    make_cfg(mount, (uint32_t)k_importer_version_a, spy_compile_fail);
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_rabook_import_open(&cfg_fail, "BOOK.EPB", path, (uint32_t)sizeof(path), &out));
  TEST_ASSERT_EQ(1, s_compile_calls);

  /* Source still readable, unmodified. */
  ra8_fs_file_t* src = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "BOOK.EPB", k_ra8_fs_mode_read, &src));
  uint32_t src_size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(src, &src_size));
  TEST_ASSERT_EQ(s_epub_len, src_size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(src));

  /* The failed run left no valid cache: a good open is still a miss. */
  ra8_rabook_import_cfg_t cfg_ok = make_cfg(mount, (uint32_t)k_importer_version_a, spy_compile);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_import_open(&cfg_ok, "BOOK.EPB", path, (uint32_t)sizeof(path), &out));
  TEST_ASSERT_EQ(k_ra8_rabook_import_compiled, out);
  TEST_ASSERT_EQ(2, s_compile_calls);

  teardown(mount);
  TEST_END("ra8_rabook_import: compile error leaves no cache");
}

/**
 * @test test_guards
 * @brief NULL-argument and capacity guards reject bad input without compiling.
 *
 * @par MC/DC:
 * Each `RA8_CHECK_NULL_PTR` is a single-condition guard (not a compound `||`),
 * so independent influence is shown by flipping one pointer to NULL per call;
 * no N-way compound vector is required.
 */
static void test_guards(void)
{
  TEST_BEGIN("ra8_rabook_import: NULL + capacity guards");
  build_epub("<html><body><p>Guarded.</p></body></html>");
  ra8_fs_mount_t* mount = fresh_volume();
  put_source(mount, "BOOK.EPB");
  s_compile_calls = 0;

  ra8_rabook_import_cfg_t     cfg = make_cfg(mount, (uint32_t)k_importer_version_a, spy_compile);
  ra8_rabook_import_outcome_t out = k_ra8_rabook_import_hit;
  char                        path[k_path_cap] = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_open(nullptr, "BOOK.EPB", path, (uint32_t)sizeof(path), &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_open(&cfg, nullptr, path, (uint32_t)sizeof(path), &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_open(&cfg, "BOOK.EPB", nullptr, (uint32_t)sizeof(path), &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_open(&cfg, "BOOK.EPB", path, (uint32_t)sizeof(path), nullptr));

  /* Missing injected dependencies. */
  ra8_rabook_import_cfg_t bad = cfg;
  bad.mount                   = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_open(&bad, "BOOK.EPB", path, (uint32_t)sizeof(path), &out));
  bad         = cfg;
  bad.compile = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_open(&bad, "BOOK.EPB", path, (uint32_t)sizeof(path), &out));
  bad         = cfg;
  bad.scratch = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_import_open(&bad, "BOOK.EPB", path, (uint32_t)sizeof(path), &out));

  /* Output buffer too small for the derived cache name. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_rabook_import_open(&cfg, "BOOK.EPB", path, 1U, &out));

  /* Missing source -> propagated open error, never a compile. */
  TEST_ASSERT(ra8_rabook_import_open(&cfg, "NOPE.EPB", path, (uint32_t)sizeof(path), &out) !=
              k_ra8_ok);
  TEST_ASSERT_EQ(0, s_compile_calls);

  teardown(mount);
  TEST_END("ra8_rabook_import: NULL + capacity guards");
}

/**
 * @test test_name_derivation_edges
 * @brief The cache name is derived from the source's basename before any
 *        compile: a directory prefix is stripped, an empty basename is refused,
 *        and a name too long for the fixed buffer is refused rather than
 *        truncated.
 *
 * @par MC/DC:
 * No compound decision under test; this drives the single-condition branches of
 * the name derivation (path-separator seen, empty stem, over-cap stem) so each
 * is exercised independently.
 */
static void test_name_derivation_edges(void)
{
  TEST_BEGIN("ra8_rabook_import: name derivation edges");
  build_epub("<html><body><p>Edge.</p></body></html>");
  ra8_fs_mount_t* mount = fresh_volume();
  put_source(mount, "BOOK.EPB");
  s_compile_calls = 0;

  ra8_rabook_import_cfg_t     cfg = make_cfg(mount, (uint32_t)k_importer_version_a, spy_compile);
  ra8_rabook_import_outcome_t out = k_ra8_rabook_import_hit;
  char                        path[k_path_cap] = {};

  /* A directory prefix is stripped to the basename before the source is read;
   * the source under that subdir does not exist, so the source-key pass fails
   * cleanly AFTER the stem is derived -- and never compiles. */
  TEST_ASSERT(ra8_rabook_import_open(&cfg, "sub/BOOK.EPB", path, (uint32_t)sizeof(path), &out) !=
              k_ra8_ok);
  TEST_ASSERT_EQ(0, s_compile_calls);

  /* A path that ends in '/' has no usable stem. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rabook_import_open(&cfg, "sub/", path, (uint32_t)sizeof(path), &out));

  /* A basename longer than the bounded name buffer is refused, not truncated. */
  static const char k_long[] =
    "this_source_basename_is_deliberately_far_longer_than_the_bounded_128_byte_"
    "cache_name_buffer_so_the_derivation_must_refuse_it_rather_than_truncate.epub";
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_import_open(&cfg, k_long, path, (uint32_t)sizeof(path), &out));
  TEST_ASSERT_EQ(0, s_compile_calls);

  teardown(mount);
  TEST_END("ra8_rabook_import: name derivation edges");
}

int32_t main(void)
{
  ra8_log_set_byte_sink(test_log_sink, nullptr);
  test_miss_then_hit();
  test_stale_version_recompiles();
  test_content_change_rederives();
  test_compile_error_leaves_no_cache();
  test_guards();
  test_name_derivation_edges();
  return 0;
}
