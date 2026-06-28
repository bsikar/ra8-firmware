/**
 * @file test_ra_rabook_import_m33.c
 * @brief Host test for the M33-offload import adapter (ra_rabook_import, #149).
 *
 * @details
 * Exercises @ref ra_rabook_import_compile_adapter_m33 -- the import seam binding
 * that dispatches a compile to the Cortex-M33 instead of compiling in-core --
 * with a MOCK @ref ra_dual_core_compile_dispatch_fn, so the adapter's read ->
 * dispatch -> validate -> write logic is covered without a second core:
 *
 *  - Happy path: a mock that returns the known-good parity golden blob -> the
 *    adapter validates it and writes it to the out path; the file reads back
 *    byte-identical and passes @ref ra_book_validate.
 *  - Reject path: a mock that returns a corrupted blob -> @ref ra_book_validate
 *    fails inside the adapter, it returns an error, and NO output file is left
 *    (the manager would fall back without poisoning the cache).
 *  - Propagate path: a mock that returns a dispatch error -> the adapter returns
 *    that error verbatim and writes nothing.
 *  - Null-guard path: a NULL cookie field -> @ref k_ra_err_null_ptr.
 *
 * @par MC/DC:
 * The adapter has no compound boolean decision -- every branch is a
 * single-condition `if (err != k_ra_ok)` or an `RA_CHECK_NULL_PTR` guard, so
 * each is covered by driving its one condition true (the corrupt / error / null
 * cases) and false (the happy path). No N+1 vector set is required.
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

#include "ra_book.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_log.h"
#include "ra_rabook_import_compiler.h"
#include "rabook_parity_fixture.h"
#include "unity_minimal.h"

/* -------------------------------------------------------------------------- */
/* Sizing + storage */
/* -------------------------------------------------------------------------- */

/**
 * @enum import_m33_cap_t
 * @brief RAM-disk geometry and adapter buffer capacities.
 */
typedef enum : uint32_t {
  k_disk_block_size = 512U,        /**< Bytes per sector (the only size ra_fs accepts). */
  k_disk_blocks     = 8192U,       /**< 4 MiB volume -> count_of_clusters lands FAT16.  */
  k_epub_load_cap   = 4U * 1024U,  /**< Source `.epub` read buffer (bytes).             */
  k_blob_cap        = 16U * 1024U, /**< Dispatched blob buffer (bytes).                 */
  k_readback_cap    = 16U * 1024U, /**< `.rabook` read-back buffer (bytes).             */
} import_m33_cap_t;

static uint8_t s_epub_load[k_epub_load_cap];
static uint8_t s_blob[k_blob_cap];
static uint8_t s_readback[k_readback_cap];

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

/**
 * @brief Format a fresh FAT16 RAM volume with a dummy source `.epub` on it.
 * @param[in] src_path Root-level 8.3 path to seed with placeholder `.epub` bytes.
 * @return Mounted volume handle.
 * @pre @p src_path is a valid 8.3 name.
 * @pre Any prior volume was unmounted.
 * @post A formatted, mounted FAT16 volume holds @p src_path.
 * @post @p s_disk.bytes owns a fresh zeroed backing store.
 * @note Not thread-safe.
 */
static ra_fs_mount_t* fresh_volume_with_epub(const char* src_path)
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

  /* The mock dispatch ignores the bytes; the adapter only needs the read to
   * succeed, so seed an arbitrary non-empty payload. */
  static const uint8_t k_dummy_epub[] = "PK\x03\x04 not-a-real-epub, the M33 mock ignores it";
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_fs_write_file(mount, src_path, k_dummy_epub, (uint32_t)sizeof(k_dummy_epub)));
  return mount;
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
static void teardown(ra_fs_mount_t* mount)
{
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(mount));
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
 * @return @c k_ra_ok, or @c k_ra_err_no_mem if @p out_cap is too small.
 * @pre @p out_buf and @p out_len are non-NULL.
 * @pre @p out_cap is the caller's blob buffer capacity.
 * @post On success @p out_buf holds the golden blob and @p *out_len its length.
 * @post @p ctx / @p epub are untouched.
 * @note Stateless; safe to call repeatedly.
 */
static ra_err_t mock_dispatch_golden(void*          ctx,
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
    return k_ra_err_no_mem;
  }
  memcpy(out_buf, s_parity_golden, (size_t)k_parity_golden_len);
  *out_len = (uint32_t)k_parity_golden_len;
  return k_ra_ok;
}

/**
 * @brief Mock dispatch: return the golden blob with one body byte flipped.
 * @param[in]  ctx      Unused mock context.
 * @param[in]  epub     Unused source bytes.
 * @param[in]  epub_len Unused source length.
 * @param[out] out_buf  Receives the corrupted blob.
 * @param[in]  out_cap  Capacity of @p out_buf.
 * @param[out] out_len  Receives the blob length.
 * @return @c k_ra_ok (a successful dispatch of a bad blob), or @c k_ra_err_no_mem.
 * @pre @p out_buf and @p out_len are non-NULL.
 * @pre @p out_cap holds at least the golden length.
 * @post @p out_buf holds a blob whose body CRC no longer matches its header.
 * @post @p ctx / @p epub are untouched.
 * @note Models a cross-core transfer slip the adapter must catch via validation.
 */
static ra_err_t mock_dispatch_corrupt(void*          ctx,
                                      const uint8_t* epub,
                                      uint32_t       epub_len,
                                      uint8_t*       out_buf,
                                      uint32_t       out_cap,
                                      uint32_t*      out_len)
{
  ra_err_t err = mock_dispatch_golden(ctx, epub, epub_len, out_buf, out_cap, out_len);
  if (err != k_ra_ok) {
    return err;
  }
  out_buf[*out_len - 1U] ^= 0xFFU; /* flip the last body byte -> CRC mismatch */
  return k_ra_ok;
}

/**
 * @brief Mock dispatch: report a worker failure (the M33 never produced a blob).
 * @param[in]  ctx      Unused mock context.
 * @param[in]  epub     Unused source bytes.
 * @param[in]  epub_len Unused source length.
 * @param[out] out_buf  Untouched.
 * @param[in]  out_cap  Unused capacity.
 * @param[out] out_len  Untouched.
 * @return Always @c k_ra_err_hw_error.
 * @pre None beyond the seam contract.
 * @pre Bound as the cookie's dispatch for the propagate-path test.
 * @post No output is written.
 * @post @p ctx / @p epub / @p out_buf are untouched.
 * @note Models the M33 stalling or faulting before completion.
 */
static ra_err_t mock_dispatch_error(void*          ctx,
                                    const uint8_t* epub,
                                    uint32_t       epub_len,
                                    uint8_t*       out_buf,
                                    uint32_t       out_cap,
                                    uint32_t*      out_len)
{
  (void)ctx;
  (void)epub;
  (void)epub_len;
  (void)out_buf;
  (void)out_cap;
  (void)out_len;
  return k_ra_err_hw_error;
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
static void make_cookie(ra_rabook_import_compiler_m33_ctx_t* ctx,
                        ra_dual_core_compile_dispatch_fn     dispatch)
{
  *ctx = (ra_rabook_import_compiler_m33_ctx_t){
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
 */
static void test_m33_adapter_writes_validated_blob(void)
{
  TEST_BEGIN("ra_rabook_import_m33: golden dispatch -> validated .rabook written");
  ra_fs_mount_t* mount = fresh_volume_with_epub("SRC.EPB");

  ra_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_golden);
  TEST_ASSERT_EQ(k_ra_ok, ra_rabook_import_compile_adapter_m33(&ctx, mount, "SRC.EPB", "OUT.RAB"));

  ra_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(mount, "OUT.RAB", k_ra_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(file));

  TEST_ASSERT_EQ((uint32_t)k_parity_golden_len, got);
  TEST_ASSERT_EQ(0, memcmp(s_readback, s_parity_golden, (size_t)got));
  TEST_ASSERT_EQ(k_ra_ok, ra_book_validate(s_readback, (size_t)got));

  teardown(mount);
  TEST_END("ra_rabook_import_m33: golden dispatch -> validated .rabook written");
}

/**
 * @test A corrupt dispatched blob is rejected by validation and not written.
 */
static void test_m33_adapter_rejects_corrupt_blob(void)
{
  TEST_BEGIN("ra_rabook_import_m33: corrupt blob -> rejected, no output");
  ra_fs_mount_t* mount = fresh_volume_with_epub("SRC.EPB");

  ra_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_corrupt);
  TEST_ASSERT(ra_rabook_import_compile_adapter_m33(&ctx, mount, "SRC.EPB", "OUT.RAB") != k_ra_ok);

  /* The validation failure must leave no output file behind. */
  ra_fs_file_t* file = nullptr;
  TEST_ASSERT(ra_fs_open(mount, "OUT.RAB", k_ra_fs_mode_read, &file) != k_ra_ok);

  teardown(mount);
  TEST_END("ra_rabook_import_m33: corrupt blob -> rejected, no output");
}

/**
 * @test A dispatch error propagates verbatim and writes nothing.
 */
static void test_m33_adapter_propagates_dispatch_error(void)
{
  TEST_BEGIN("ra_rabook_import_m33: dispatch error -> propagated, no output");
  ra_fs_mount_t* mount = fresh_volume_with_epub("SRC.EPB");

  ra_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_error);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 ra_rabook_import_compile_adapter_m33(&ctx, mount, "SRC.EPB", "OUT.RAB"));

  ra_fs_file_t* file = nullptr;
  TEST_ASSERT(ra_fs_open(mount, "OUT.RAB", k_ra_fs_mode_read, &file) != k_ra_ok);

  teardown(mount);
  TEST_END("ra_rabook_import_m33: dispatch error -> propagated, no output");
}

/**
 * @test NULL arguments and a NULL cookie field are rejected with null-ptr.
 */
static void test_m33_adapter_null_guards(void)
{
  TEST_BEGIN("ra_rabook_import_m33: null guards");
  ra_fs_mount_t* mount = fresh_volume_with_epub("SRC.EPB");

  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_rabook_import_compile_adapter_m33(nullptr, mount, "SRC.EPB", "OUT.RAB"));

  ra_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_golden);
  ctx.dispatch = nullptr; /* a NULL cookie field must be caught */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_rabook_import_compile_adapter_m33(&ctx, mount, "SRC.EPB", "OUT.RAB"));

  teardown(mount);
  TEST_END("ra_rabook_import_m33: null guards");
}

/**
 * @test A missing source `.epub` surfaces the read error and writes nothing.
 */
static void test_m33_adapter_handles_missing_source(void)
{
  TEST_BEGIN("ra_rabook_import_m33: missing source -> read error, no output");
  ra_fs_mount_t* mount = fresh_volume_with_epub("SRC.EPB");

  ra_rabook_import_compiler_m33_ctx_t ctx = {};
  make_cookie(&ctx, mock_dispatch_golden);
  /* NOPE.EPB does not exist -> ra_fs_open fails inside s_read_whole_file. */
  TEST_ASSERT(ra_rabook_import_compile_adapter_m33(&ctx, mount, "NOPE.EPB", "OUT.RAB") != k_ra_ok);

  ra_fs_file_t* file = nullptr;
  TEST_ASSERT(ra_fs_open(mount, "OUT.RAB", k_ra_fs_mode_read, &file) != k_ra_ok);

  teardown(mount);
  TEST_END("ra_rabook_import_m33: missing source -> read error, no output");
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
  test_m33_adapter_writes_validated_blob();
  test_m33_adapter_rejects_corrupt_blob();
  test_m33_adapter_propagates_dispatch_error();
  test_m33_adapter_handles_missing_source();
  test_m33_adapter_null_guards();
  (void)fprintf(stderr, "[OK ] test_ra_rabook_import_m33.c\n");
  return 0;
}
