/**
 * @file test_ra8_cbz_container.c
 * @brief Tests for the per-page "RCBZ" comic-container reader (ra8_cbz_container.c).
 *
 * @details
 * Exercises `ra8_cbz_open()` / `ra8_cbz_page_info()` / `ra8_cbz_page_bind()` /
 * `ra8_cbz_page_read()` against an in-memory container built with *real* zlib
 * streams (miniz `mz_compress2`, the same RFC 1950 wrapping the host producer
 * `tools/epub_compile/cbz_container.py` emits) and inflated with a tinfl
 * wrapper shaped like the shipping shelf inflater. The three fixture pages have
 * distinct sizes so the variable-length manifest is stressed.
 *
 * The acceptance bar for #209 is the equivalence test: reading each page a page
 * at a time through the container -- both directly and through the
 * `ra8_vsource_add_paged` + `ra8_vsource_loader` seam the issue names -- yields
 * bytes identical to the source page rasters (paged == whole-file). The rest
 * covers the open path's header / table / capacity guards, the manifest
 * accessors, and every read-path error leg (unbound, bad offset, wrong span,
 * corrupt stream, wrong-length inflate, file-read passthrough).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_book.h"
#include "ra8_cbz_container.h"
#include "ra8_err.h"
#include "ra8_vsource.h"
#include "support/cbz_test_util.h"
#include "unity_minimal.h"

/**
 * @test test_ra8_cbz_open_happy
 * @brief A well-formed container binds with the parsed geometry exposed.
 *
 * @par Targeted code:
 * `ra8_cbz_open`'s success path end-to-end: header read + parse, both table
 * reads, offset monotonic walk, staging-fit + raster-length checks, and the
 * bound `offsets` / `meta` / `payload_off` publication; plus the manifest
 * accessors `ra8_cbz_page_count` / `ra8_cbz_is_rtl`.
 *
 * @par MC/DC:
 * (no compound decisions under test -- every guard in the open path is an
 * independent single-condition check)
 */
static void test_ra8_cbz_open_happy(void)
{
  TEST_BEGIN("ra8_cbz_open happy path");
  ccx_fill_pages();
  static uint8_t s_container[k_ccx_file_cap];
  const uint64_t file_len = ccx_pack(s_container, 0U, 0U);

  ra8_cbz_t      cbz                            = {};
  ccx_file_t     file                           = {};
  uint64_t       offsets_buf[k_ccx_offsets_cap] = {};
  static uint8_t s_meta_buf[k_ccx_meta_cap];
  static uint8_t s_staging[k_ccx_staging_cap];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ccx_open(&cbz, &file, s_container, file_len, offsets_buf, s_meta_buf, s_staging));

  TEST_ASSERT_EQ(k_ccx_page_count, cbz.page_count);
  TEST_ASSERT_EQ(k_ccx_page_count, ra8_cbz_page_count(&cbz));
  TEST_ASSERT(!ra8_cbz_is_rtl(&cbz));
  TEST_ASSERT(cbz.offsets == offsets_buf);
  TEST_ASSERT(cbz.meta == s_meta_buf);
  const uint64_t want_payload = (uint64_t)k_ra8_cbz_header_len +
                                ((uint64_t)(k_ccx_page_count + 1U) * k_ra8_cbz_offset_len) +
                                ((uint64_t)k_ccx_page_count * k_ra8_cbz_meta_len);
  TEST_ASSERT_EQ(want_payload, cbz.payload_off);
  TEST_ASSERT_EQ(0U, cbz.offsets[0]);

  /* RTL container: exactly the RTL flag bit is decoded. */
  static uint8_t s_rtl_container[k_ccx_file_cap];
  const uint64_t rtl_len  = ccx_pack(s_rtl_container, (uint32_t)k_ra8_book_flag_rtl, 0U);
  ra8_cbz_t      rtl_cbz  = {};
  ccx_file_t     rtl_file = {};
  uint64_t       rtl_off[k_ccx_offsets_cap] = {};
  static uint8_t s_rtl_meta[k_ccx_meta_cap];
  static uint8_t s_rtl_stage[k_ccx_staging_cap];
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ccx_open(&rtl_cbz, &rtl_file, s_rtl_container, rtl_len, rtl_off, s_rtl_meta, s_rtl_stage));
  TEST_ASSERT(ra8_cbz_is_rtl(&rtl_cbz));

  TEST_END("ra8_cbz_open happy path");
}

/**
 * @struct ccx_guard_ctx_t
 * @brief Shared buffers and container image for the open-path guard helpers.
 *
 * @details Bundles the caller-provided offset / metadata / staging buffers and
 *          the packed container image so each `ccx_open_guards_*` helper takes a
 *          single context pointer instead of the eleven-argument `ra8_cbz_open`
 *          list. Only the guard-sequence helpers consume it.
 *
 * @invariant All pointers reference caller-owned storage that outlives the
 *            guard sequence; @c file_len is the packed length of @c container.
 *
 * @see test_ra8_cbz_open_guards
 */
typedef struct {
  uint8_t*  container; /**< Packed RCBZ container image under test.         */
  uint64_t  file_len;  /**< Byte length of the packed container.            */
  uint64_t* offsets;   /**< Caller offset-table buffer (k_ccx_offsets_cap). */
  uint8_t*  meta;      /**< Caller metadata buffer (k_ccx_meta_cap).        */
  uint8_t*  staging;   /**< Caller staging buffer (k_ccx_staging_cap).      */
} ccx_guard_ctx_t;

/**
 * @enum ccx_fail_call_t
 * @brief 1-based file-read call indices used to inject a read failure.
 */
typedef enum : uint32_t {
  k_ccx_fail_header  = 1U, /**< Fail the fixed-header read.   */
  k_ccx_fail_offsets = 2U, /**< Fail the offset-table read.   */
  k_ccx_fail_meta    = 3U, /**< Fail the metadata-table read. */
} ccx_fail_call_t;

/**
 * @brief Assert that a single null `ra8_cbz_open` argument yields null_ptr.
 *
 * @details Drives `ra8_cbz_open` at the fixture capacities with every argument
 *          supplied by the caller, so a single nulled slot exercises exactly one
 *          of the six `RA8_CHECK_NULL_PTR` guards.
 *
 * @param[out] cbz      Container handle (nullptr exercises the cbz guard).
 * @param[in]  read     File reader (nullptr exercises the file_read guard).
 * @param[in]  file     File context passed to @p read.
 * @param[in]  inflate  Decompressor (nullptr exercises the inflate guard).
 * @param[out] offs     Offset buffer (nullptr exercises the offsets guard).
 * @param[out] meta     Metadata buffer (nullptr exercises the meta guard).
 * @param[out] staging  Staging buffer (nullptr exercises the staging guard).
 * @param[in]  file_len Packed container length in bytes.
 *
 * @pre Exactly one pointer argument is nullptr.
 * @post `ra8_cbz_open` returned k_ra8_err_null_ptr.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void ccx_expect_null_open(ra8_cbz_t*          cbz,
                                 ra8_vsource_read_fn read,
                                 void*               file,
                                 ra8_book_inflate_fn inflate,
                                 uint64_t*           offs,
                                 uint8_t*            meta,
                                 uint8_t*            staging,
                                 uint64_t            file_len)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_cbz_open(cbz,
                              read,
                              file,
                              file_len,
                              inflate,
                              offs,
                              k_ccx_offsets_cap,
                              meta,
                              k_ccx_meta_cap,
                              staging,
                              k_ccx_staging_cap));
}

/**
 * @brief Open with explicit capacities and assert the expected error.
 *
 * @details Rewinds @p file over the container image then opens with the given
 *          per-table capacities so a single undersized budget exercises one of
 *          the capacity-return legs.
 *
 * @param[in]     want      Expected `ra8_cbz_open` result.
 * @param[out]    cbz       Container handle under test.
 * @param[in,out] file      File context, reset to the container image.
 * @param[in]     g         Shared buffers and container image.
 * @param[in]     off_cap   Offset-table entry budget.
 * @param[in]     meta_cap  Metadata-table byte budget.
 * @param[in]     stage_cap Staging-buffer byte budget.
 *
 * @pre @p g fields reference valid buffers.
 * @post `ra8_cbz_open` returned @p want.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void ccx_expect_open_err(ra8_err_t              want,
                                ra8_cbz_t*             cbz,
                                ccx_file_t*            file,
                                const ccx_guard_ctx_t* g,
                                uint32_t               off_cap,
                                uint32_t               meta_cap,
                                uint32_t               stage_cap)
{
  *file = (ccx_file_t){.data = g->container, .len = g->file_len, .fail_at = 0U, .calls = 0U};
  TEST_ASSERT_EQ(want,
                 ra8_cbz_open(cbz,
                              ccx_file_read,
                              file,
                              g->file_len,
                              ccx_inflate,
                              g->offsets,
                              off_cap,
                              g->meta,
                              meta_cap,
                              g->staging,
                              stage_cap));
}

/**
 * @brief Exercise the six null-pointer guards of `ra8_cbz_open`.
 *
 * @param[out]    cbz  Container handle under test.
 * @param[in,out] file File context bound to the container image.
 * @param[in]     g    Shared buffers and container image.
 *
 * @pre @p g fields reference valid buffers.
 * @post Each guarded argument returned k_ra8_err_null_ptr.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void ccx_open_guards_null(ra8_cbz_t* cbz, ccx_file_t* file, const ccx_guard_ctx_t* g)
{
  ccx_expect_null_open(nullptr,
                       ccx_file_read,
                       file,
                       ccx_inflate,
                       g->offsets,
                       g->meta,
                       g->staging,
                       g->file_len);
  ccx_expect_null_open(cbz,
                       nullptr,
                       file,
                       ccx_inflate,
                       g->offsets,
                       g->meta,
                       g->staging,
                       g->file_len);
  ccx_expect_null_open(cbz,
                       ccx_file_read,
                       file,
                       nullptr,
                       g->offsets,
                       g->meta,
                       g->staging,
                       g->file_len);
  ccx_expect_null_open(cbz,
                       ccx_file_read,
                       file,
                       ccx_inflate,
                       nullptr,
                       g->meta,
                       g->staging,
                       g->file_len);
  ccx_expect_null_open(cbz,
                       ccx_file_read,
                       file,
                       ccx_inflate,
                       g->offsets,
                       nullptr,
                       g->staging,
                       g->file_len);
  ccx_expect_null_open(cbz,
                       ccx_file_read,
                       file,
                       ccx_inflate,
                       g->offsets,
                       g->meta,
                       nullptr,
                       g->file_len);
}

/**
 * @brief Exercise the short-file and header-field rejections of `s_parse_header`.
 *
 * @param[out]    cbz  Container handle under test.
 * @param[in,out] file File context bound to the container image.
 * @param[in]     g    Shared buffers and container image.
 *
 * @pre @p g fields reference valid buffers.
 * @post Every malformed header returned its rejection code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void ccx_open_guards_header(ra8_cbz_t* cbz, ccx_file_t* file, const ccx_guard_ctx_t* g)
{
  /* File shorter than the fixed header. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ccx_open(cbz, file, g->container, k_ccx_short_file, g->offsets, g->meta, g->staging));
  TEST_ASSERT(cbz->offsets == nullptr);

  /* Bad magic. */
  static uint8_t s_bad_magic[k_ccx_file_cap];
  memcpy(s_bad_magic, g->container, (size_t)g->file_len);
  s_bad_magic[0] = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ccx_open(cbz, file, s_bad_magic, g->file_len, g->offsets, g->meta, g->staging));

  /* Zero page count. */
  static uint8_t s_zero_pages[k_ccx_file_cap];
  memcpy(s_zero_pages, g->container, (size_t)g->file_len);
  const uint32_t zero = 0U;
  memcpy(&s_zero_pages[k_ra8_cbz_hdr_off_page_count], &zero, sizeof(zero));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ccx_open(cbz, file, s_zero_pages, g->file_len, g->offsets, g->meta, g->staging));

  /* Non-zero reserved word. */
  static uint8_t s_bad_rsv[k_ccx_file_cap];
  memcpy(s_bad_rsv, g->container, (size_t)g->file_len);
  const uint32_t one = 1U;
  memcpy(&s_bad_rsv[k_ra8_cbz_hdr_off_reserved], &one, sizeof(one));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ccx_open(cbz, file, s_bad_rsv, g->file_len, g->offsets, g->meta, g->staging));

  /* Unknown feature-flag bit. */
  static uint8_t s_bad_flag[k_ccx_file_cap];
  memcpy(s_bad_flag, g->container, (size_t)g->file_len);
  const uint32_t unknown_flag = (uint32_t)k_ra8_book_flag_mask_known + 1U;
  memcpy(&s_bad_flag[k_ra8_cbz_hdr_off_flags], &unknown_flag, sizeof(unknown_flag));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ccx_open(cbz, file, s_bad_flag, g->file_len, g->offsets, g->meta, g->staging));
}

/**
 * @brief Exercise the offset / metadata / file-bound / staging capacity returns.
 *
 * @param[out]    cbz  Container handle under test.
 * @param[in,out] file File context bound to the container image.
 * @param[in]     g    Shared buffers and container image.
 *
 * @pre @p g fields reference valid buffers.
 * @post Every undersized budget returned k_ra8_err_invalid_size.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void ccx_open_guards_capacity(ra8_cbz_t* cbz, ccx_file_t* file, const ccx_guard_ctx_t* g)
{
  /* Offset table needs more entries than the caller budgeted. */
  ccx_expect_open_err(k_ra8_err_invalid_size,
                      cbz,
                      file,
                      g,
                      k_ccx_tiny_off,
                      k_ccx_meta_cap,
                      k_ccx_staging_cap);
  /* Metadata table needs more bytes than the caller budgeted. */
  ccx_expect_open_err(k_ra8_err_invalid_size,
                      cbz,
                      file,
                      g,
                      k_ccx_offsets_cap,
                      k_ccx_tiny_meta,
                      k_ccx_staging_cap);
  /* File ends inside the offset table. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ccx_open(cbz, file, g->container, k_ccx_table_cut, g->offsets, g->meta, g->staging));
  /* A compressed page larger than the staging budget. */
  ccx_expect_open_err(k_ra8_err_invalid_size,
                      cbz,
                      file,
                      g,
                      k_ccx_offsets_cap,
                      k_ccx_meta_cap,
                      k_ccx_tiny_stage);
}

/**
 * @brief Exercise the offset-table and zero-raster metadata rejections.
 *
 * @param[out]    cbz  Container handle under test.
 * @param[in,out] file File context bound to the container image.
 * @param[in]     g    Shared buffers and container image.
 *
 * @pre @p g fields reference valid buffers.
 * @post Every corrupt table returned k_ra8_err_invalid_arg.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void ccx_open_guards_table(ra8_cbz_t* cbz, ccx_file_t* file, const ccx_guard_ctx_t* g)
{
  const uint32_t zero = 0U;

  /* Corrupt offset table: force a non-monotonic entry (offset[1] == offset[0]). */
  static uint8_t s_bad_offs[k_ccx_file_cap];
  memcpy(s_bad_offs, g->container, (size_t)g->file_len);
  const uint64_t zero64 = 0U;
  memcpy(&s_bad_offs[k_ra8_cbz_header_len + k_ra8_cbz_offset_len], &zero64, sizeof(zero64));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ccx_open(cbz, file, s_bad_offs, g->file_len, g->offsets, g->meta, g->staging));

  /* Offset table whose first entry is not zero. */
  static uint8_t s_bad_off0[k_ccx_file_cap];
  memcpy(s_bad_off0, g->container, (size_t)g->file_len);
  const uint64_t one64 = 1U;
  memcpy(&s_bad_off0[k_ra8_cbz_header_len], &one64, sizeof(one64));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ccx_open(cbz, file, s_bad_off0, g->file_len, g->offsets, g->meta, g->staging));

  /* File one byte short: the offset table's end disagrees with the payload. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ccx_open(cbz, file, g->container, g->file_len - 1U, g->offsets, g->meta, g->staging));

  /* Zero raster length in the metadata table (page 0). */
  static uint8_t s_bad_raw[k_ccx_file_cap];
  memcpy(s_bad_raw, g->container, (size_t)g->file_len);
  const size_t meta0 =
    (size_t)k_ra8_cbz_header_len + ((size_t)(k_ccx_page_count + 1U) * k_ra8_cbz_offset_len);
  memcpy(&s_bad_raw[meta0 + k_ra8_cbz_meta_off_raw], &zero, sizeof(zero));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ccx_open(cbz, file, s_bad_raw, g->file_len, g->offsets, g->meta, g->staging));
}

/**
 * @brief Assert a file-read failure at call @p fail_at propagates verbatim.
 *
 * @param[out]    cbz     Container handle under test.
 * @param[in,out] file    File context bound to the container image.
 * @param[in]     g       Shared buffers and container image.
 * @param[in]     fail_at 1-based call index to fail on.
 *
 * @pre @p g fields reference valid buffers.
 * @post `ra8_cbz_open` returned k_ra8_err_hw_timeout.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void
ccx_expect_readfail(ra8_cbz_t* cbz, ccx_file_t* file, const ccx_guard_ctx_t* g, uint32_t fail_at)
{
  *file = (ccx_file_t){.data = g->container, .len = g->file_len, .fail_at = fail_at, .calls = 0U};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_cbz_open(cbz,
                              ccx_file_read,
                              file,
                              g->file_len,
                              ccx_inflate,
                              g->offsets,
                              k_ccx_offsets_cap,
                              g->meta,
                              k_ccx_meta_cap,
                              g->staging,
                              k_ccx_staging_cap));
}

/**
 * @brief Exercise the three file-read error passthroughs of `ra8_cbz_open`.
 *
 * @param[out]    cbz  Container handle under test.
 * @param[in,out] file File context bound to the container image.
 * @param[in]     g    Shared buffers and container image.
 *
 * @pre @p g fields reference valid buffers.
 * @post Each of the header / offset / metadata reads propagated its error.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void ccx_open_guards_readfail(ra8_cbz_t* cbz, ccx_file_t* file, const ccx_guard_ctx_t* g)
{
  ccx_expect_readfail(cbz, file, g, k_ccx_fail_header);
  ccx_expect_readfail(cbz, file, g, k_ccx_fail_offsets);
  ccx_expect_readfail(cbz, file, g, k_ccx_fail_meta);
}

/**
 * @test test_ra8_cbz_open_guards
 * @brief Every open-path guard rejects its malformed input.
 *
 * @par Targeted code:
 * The six `RA8_CHECK_NULL_PTR` guards, the short-file return, the bad-magic /
 * zero-page-count / non-zero-reserved / unknown-flag rejections in
 * `s_parse_header`, `s_load_tables`' offset-cap / meta-cap / file-bound
 * returns, the staging-capacity return, a corrupt offset table, a zero raster
 * length, and the two file-read error passthroughs (header + meta reads).
 *
 * @par MC/DC:
 * (no compound decisions under test -- every open guard is an independent
 * single-condition check)
 */
static void test_ra8_cbz_open_guards(void)
{
  TEST_BEGIN("ra8_cbz_open guards");
  ccx_fill_pages();
  static uint8_t s_container[k_ccx_file_cap];
  const uint64_t file_len = ccx_pack(s_container, 0U, 0U);

  ra8_cbz_t      cbz                            = {};
  ccx_file_t     file                           = {};
  uint64_t       offsets_buf[k_ccx_offsets_cap] = {};
  static uint8_t s_meta_buf[k_ccx_meta_cap];
  static uint8_t s_staging[k_ccx_staging_cap];
  file = (ccx_file_t){.data = s_container, .len = file_len, .fail_at = 0U, .calls = 0U};

  const ccx_guard_ctx_t g = {.container = s_container,
                             .file_len  = file_len,
                             .offsets   = offsets_buf,
                             .meta      = s_meta_buf,
                             .staging   = s_staging};

  ccx_open_guards_null(&cbz, &file, &g);
  ccx_open_guards_header(&cbz, &file, &g);
  ccx_open_guards_capacity(&cbz, &file, &g);
  ccx_open_guards_table(&cbz, &file, &g);
  ccx_open_guards_readfail(&cbz, &file, &g);

  TEST_END("ra8_cbz_open guards");
}

/**
 * @test test_ra8_cbz_page_info
 * @brief The manifest exposes each page's dimensions, format and raster length.
 *
 * @par Targeted code:
 * `ra8_cbz_page_info` happy path over all three pages plus its null / unbound /
 * out-of-range guards, and the unbound / null / out-of-range accessors on the
 * inline `ra8_cbz_page_count` / `ra8_cbz_is_rtl`.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each info guard is an independent
 * single-condition check)
 */
static void test_ra8_cbz_page_info(void)
{
  TEST_BEGIN("ra8_cbz_page_info manifest");
  ccx_fill_pages();
  static uint8_t s_container[k_ccx_file_cap];
  const uint64_t file_len = ccx_pack(s_container, 0U, 0U);

  ra8_cbz_t      cbz                            = {};
  ccx_file_t     file                           = {};
  uint64_t       offsets_buf[k_ccx_offsets_cap] = {};
  static uint8_t s_meta_buf[k_ccx_meta_cap];
  static uint8_t s_staging[k_ccx_staging_cap];

  /* Unbound / null accessors return their safe defaults. */
  TEST_ASSERT_EQ(0U, ra8_cbz_page_count(nullptr));
  TEST_ASSERT_EQ(0U, ra8_cbz_page_count(&cbz));
  TEST_ASSERT(!ra8_cbz_is_rtl(nullptr));
  TEST_ASSERT(!ra8_cbz_is_rtl(&cbz));

  uint16_t w   = 0U;
  uint16_t h   = 0U;
  uint8_t  fmt = 0U;
  uint32_t raw = 0U;
  /* Unbound-container guard (meta == NULL). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_cbz_page_info(&cbz, 0U, &w, &h, &fmt, &raw));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ccx_open(&cbz, &file, s_container, file_len, offsets_buf, s_meta_buf, s_staging));

  for (uint32_t p = 0U; p < k_ccx_page_count; ++p) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_cbz_page_info(&cbz, p, &w, &h, &fmt, &raw));
    TEST_ASSERT_EQ(s_pages[p].width, w);
    TEST_ASSERT_EQ(s_pages[p].height, h);
    TEST_ASSERT_EQ(s_pages[p].format, fmt);
    TEST_ASSERT_EQ(s_pages[p].raw_size, raw);
  }

  /* Null-argument guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cbz_page_info(nullptr, 0U, &w, &h, &fmt, &raw));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cbz_page_info(&cbz, 0U, nullptr, &h, &fmt, &raw));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cbz_page_info(&cbz, 0U, &w, nullptr, &fmt, &raw));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cbz_page_info(&cbz, 0U, &w, &h, nullptr, &raw));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cbz_page_info(&cbz, 0U, &w, &h, &fmt, nullptr));
  /* Out-of-range page. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_cbz_page_info(&cbz, k_ccx_page_count, &w, &h, &fmt, &raw));

  TEST_END("ra8_cbz_page_info manifest");
}

/**
 * @brief Read one page's raster through the ra8_vsource paged seam.
 * @details Registers the bound cursor as a paged object and drives one
 *          ::ra8_vsource_loader fill (frame >= page raster), exactly as the #147
 *          cache would on a miss; copies the in-range prefix into @p out.
 * @param[in]  cur   Bound page cursor.
 * @param[out] out   Destination for the page raster (>= cur->raw_size bytes).
 * @return ra8_err_t The loader result.
 */
static ra8_err_t ccx_read_via_vsource(ra8_cbz_page_t* cur, uint8_t* out)
{
  ra8_vsource_obj_t objs[1];
  ra8_vsource_t     vs  = {};
  ra8_err_t         err = ra8_vsource_init(&vs, objs, 1U);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t oid = 0U;
  err = ra8_vsource_add_paged(&vs, ra8_cbz_page_read, cur, 0U, (uint64_t)cur->raw_size, &oid);
  if (err != k_ra8_ok) {
    return err;
  }
  static uint8_t s_frame[k_ccx_frame_bytes];
  err = ra8_vsource_loader(&vs, oid, 0U, s_frame, (uint32_t)k_ccx_frame_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  memcpy(out, s_frame, (size_t)cur->raw_size);
  return k_ra8_ok;
}

/**
 * @test test_ra8_cbz_page_read_equivalence
 * @brief Per-page paged reads reconstruct the source rasters byte-for-byte.
 *
 * @par Targeted code:
 * The #209 acceptance: `ra8_cbz_page_bind` + `ra8_cbz_page_read` for every page,
 * read two ways -- directly, and through `ra8_vsource_add_paged` +
 * `ra8_vsource_loader` (the seam the issue names) -- each byte-identical to the
 * packed source raster. This is the multi-GB per-page read path in miniature.
 *
 * @par MC/DC:
 * (no compound decisions under test -- end-to-end byte-identity through the
 * per-page reader; every read guard is an independent single-condition check)
 */
static void test_ra8_cbz_page_read_equivalence(void)
{
  TEST_BEGIN("ra8_cbz_page_read paged == whole-file");
  ccx_fill_pages();
  static uint8_t s_container[k_ccx_file_cap];
  const uint64_t file_len = ccx_pack(s_container, (uint32_t)k_ra8_book_flag_rtl, 0U);

  ra8_cbz_t      cbz                            = {};
  ccx_file_t     file                           = {};
  uint64_t       offsets_buf[k_ccx_offsets_cap] = {};
  static uint8_t s_meta_buf[k_ccx_meta_cap];
  static uint8_t s_staging[k_ccx_staging_cap];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ccx_open(&cbz, &file, s_container, file_len, offsets_buf, s_meta_buf, s_staging));
  TEST_ASSERT(ra8_cbz_is_rtl(&cbz));

  static uint8_t s_direct[k_ccx_max_raw];
  static uint8_t s_viasrc[k_ccx_max_raw];
  for (uint32_t p = 0U; p < k_ccx_page_count; ++p) {
    ra8_cbz_page_t cur = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_cbz_page_bind(&cbz, p, &cur));
    TEST_ASSERT_EQ(s_pages[p].raw_size, cur.raw_size);

    /* (A) direct paged read. */
    memset(s_direct, 0, sizeof(s_direct));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_cbz_page_read(&cur, 0U, s_direct, cur.raw_size));
    TEST_ASSERT_EQ(0, memcmp(s_direct, s_pages[p].raw, s_pages[p].raw_size));

    /* (B) through the ra8_vsource paged seam. */
    memset(s_viasrc, 0, sizeof(s_viasrc));
    TEST_ASSERT_EQ(k_ra8_ok, ccx_read_via_vsource(&cur, s_viasrc));
    TEST_ASSERT_EQ(0, memcmp(s_viasrc, s_pages[p].raw, s_pages[p].raw_size));
  }

  TEST_END("ra8_cbz_page_read paged == whole-file");
}

/**
 * @brief Exercise the bind / read guards reachable before a container is opened.
 *
 * @details Covers the two `ra8_cbz_page_bind` null guards, its invalid-state
 *          return on an unopened handle, the two `ra8_cbz_page_read` null
 *          guards, its invalid-state return on an unbound cursor, and a cursor
 *          whose container was never bound (offsets == NULL).
 *
 * @param[in]  cbz An unopened container handle.
 * @param[out] cur A zero-initialised page cursor used as the read subject.
 * @param[out] out Raster output buffer, at least k_ccx_p0_raw bytes.
 *
 * @pre @p cbz is zero-initialised (unopened).
 * @post Every pre-open guard returned its documented error.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void ccx_page_read_guards_unbound(ra8_cbz_t* cbz, ra8_cbz_page_t* cur, uint8_t* out)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cbz_page_bind(nullptr, 0U, cur));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cbz_page_bind(cbz, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_cbz_page_bind(cbz, 0U, cur));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cbz_page_read(nullptr, 0U, out, k_ccx_p0_raw));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cbz_page_read(cur, 0U, nullptr, k_ccx_p0_raw));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_cbz_page_read(cur, 0U, out, k_ccx_p0_raw));

  /* Cursor whose container was never bound (offsets == NULL) -> invalid_state. */
  ra8_cbz_t      unbound_cbz = {};
  ra8_cbz_page_t orphan_cur  = {.cbz = &unbound_cbz, .page = 0U, .raw_size = k_ccx_p0_raw};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_cbz_page_read(&orphan_cur, 0U, out, k_ccx_p0_raw));
}

/**
 * @brief Assert an inflated manifest raster length is rejected on read.
 *
 * @details Packs a fresh container whose last page's stored raw_size is one byte
 *          too long, opens and binds the last page, confirms the inflated
 *          raw_size, then reads and expects k_ra8_err_invalid_size because the
 *          produced length no longer matches the manifest.
 *
 * @pre The page fixtures are already populated (ccx_fill_pages()).
 * @post `ra8_cbz_page_read` returned k_ra8_err_invalid_size.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void ccx_page_read_guards_badraw(void)
{
  static uint8_t s_badraw_container[k_ccx_file_cap];
  const uint64_t badraw_len              = ccx_pack(s_badraw_container, 0U, 1U);
  ra8_cbz_t      brd                     = {};
  ccx_file_t     bfile                   = {};
  uint64_t       boff[k_ccx_offsets_cap] = {};
  static uint8_t s_bmeta[k_ccx_meta_cap];
  static uint8_t s_bstage[k_ccx_staging_cap];
  static uint8_t s_bout[k_ccx_max_raw + 1U];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ccx_open(&brd, &bfile, s_badraw_container, badraw_len, boff, s_bmeta, s_bstage));
  ra8_cbz_page_t bcur = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cbz_page_bind(&brd, k_ccx_page_count - 1U, &bcur));
  TEST_ASSERT_EQ(k_ccx_p2_raw + 1U, bcur.raw_size);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_cbz_page_read(&bcur, 0U, s_bout, bcur.raw_size));
}

/**
 * @test test_ra8_cbz_page_read_guards
 * @brief Every bind / read guard rejects its malformed request.
 *
 * @par Targeted code:
 * `ra8_cbz_page_bind`'s null / unbound / out-of-range guards, and
 * `ra8_cbz_page_read`'s null / unbound-cursor / unbound-container / bad-offset /
 * wrong-span guards, the file-read passthrough, the corrupt-stream inflater
 * passthrough, and the valid-stream-wrong-length return.
 *
 * @par MC/DC:
 * (no compound decisions under test -- every bind / read guard is an
 * independent single-condition check)
 */
static void test_ra8_cbz_page_read_guards(void)
{
  TEST_BEGIN("ra8_cbz_page_read guards");
  ccx_fill_pages();
  static uint8_t s_container[k_ccx_file_cap];
  const uint64_t file_len = ccx_pack(s_container, 0U, 0U);

  ra8_cbz_t      cbz                            = {};
  ccx_file_t     file                           = {};
  uint64_t       offsets_buf[k_ccx_offsets_cap] = {};
  static uint8_t s_meta_buf[k_ccx_meta_cap];
  static uint8_t s_staging[k_ccx_staging_cap];
  static uint8_t s_out[k_ccx_max_raw];

  /* Bind / read guards reachable before the container is opened. */
  ra8_cbz_page_t cur = {};
  ccx_page_read_guards_unbound(&cbz, &cur, s_out);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ccx_open(&cbz, &file, s_container, file_len, offsets_buf, s_meta_buf, s_staging));

  /* Out-of-range bind. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_cbz_page_bind(&cbz, k_ccx_page_count, &cur));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cbz_page_bind(&cbz, 0U, &cur));

  /* Non-zero offset and wrong span are rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_cbz_page_read(&cur, 1U, s_out, k_ccx_p0_raw));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_cbz_page_read(&cur, 0U, s_out, k_ccx_p0_raw - 1U));

  /* File-read failure while staging the stream propagates verbatim. */
  file.fail_at = file.calls + 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cbz_page_read(&cur, 0U, s_out, k_ccx_p0_raw));
  file.fail_at = 0U;

  /* A corrupt stream fails in the inflater and propagates. */
  const uint64_t p0_at    = cbz.payload_off + cbz.offsets[0] + 4U;
  const uint8_t  saved    = s_container[p0_at];
  s_container[p0_at]      = (uint8_t)~saved;
  const ra8_err_t corrupt = ra8_cbz_page_read(&cur, 0U, s_out, k_ccx_p0_raw);
  s_container[p0_at]      = saved;
  TEST_ASSERT(corrupt != k_ra8_ok);

  ccx_page_read_guards_badraw();

  TEST_END("ra8_cbz_page_read guards");
}

int32_t main(void)
{
  test_ra8_cbz_open_happy();
  test_ra8_cbz_open_guards();
  test_ra8_cbz_page_info();
  test_ra8_cbz_page_read_equivalence();
  test_ra8_cbz_page_read_guards();
  (void)fprintf(stderr, "[OK ] test_ra8_cbz_container.c\n");
  return 0;
}
