/**
 * @file test_ra8_comic_cbt.c
 * @brief Tests for the CBT (tar) comic backend and the wrapped (gz/xz) open.
 *
 * @details
 * Builds real tar comics block-by-block in memory and drives them through
 * the public comic facade, proving:
 *
 *   1. a bare `.cbt` opens via the magic-less tar probe: pages enumerate in
 *      sorted order, extract byte-exactly, and non-page members are skipped,
 *   2. a gzip-wrapped comic (`.cbt.gz` / `.tar.gz`, built in-memory around
 *      a tdefl DEFLATE body) opens through `ra8_comic_open_wrapped` into a
 *      caller arena,
 *   3. an XZ-wrapped comic (`.tar.xz`, the committed `k_fx_xz_cbt` fixture)
 *      opens likewise through the XZ session scratch,
 *   4. a bare container passes through the wrapped open unchanged
 *      (ZIP -> CBZ), and wrapper-in-wrapper nesting bombs (gzip-of-gzip,
 *      gzip-of-xz) are rejected as `k_ra8_err_decomp_depth`,
 *   5. the wrapped-open argument guards (alignment, arena size, NULL XZ
 *      scratch) fail closed.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_comic.h"
#include "ra8_comic_internal.h"
#include "ra8_err.h"
#include "unarch_xz_fixture.h"
#include "unity_minimal.h"

/**
 * @enum comic_cbt_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_comic_cbt_s_arc_1f      = 0x1FU,
  k_comic_cbt_s_arc_8b      = 0x8BU,
  k_comic_cbt_tcb_octal_100 = 100,
  k_comic_cbt_tcb_octal_108 = 108,
  k_comic_cbt_tcb_octal_116 = 116,
  k_comic_cbt_tcb_octal_12  = 12U,
  k_comic_cbt_tcb_octal_124 = 124,
  k_comic_cbt_tcb_octal_136 = 136,
  k_comic_cbt_tcb_octal_148 = 148,
  k_comic_cbt_tcb_octal_7   = 7U,
  k_comic_cbt_val_155       = 155,
  k_comic_cbt_val_156       = 156,
  k_comic_cbt_val_64        = 64,
  k_comic_cbt_val_ff        = 0xFFU,
} comic_cbt_uint8_const_t;

/**
 * @enum comic_cbt_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_comic_cbt_i_512         = 512U,
  k_comic_cbt_off_1024      = 1024U,
  k_comic_cbt_s_arc_len_600 = 600U,
} comic_cbt_uint16_const_t;

/** @brief Named double constant used by this file. */
/**
 * @enum comic_cbt_tcb_octal_0644_t
 * @brief Named octal mode bits used by this file.
 */
typedef enum : uint16_t {
  k_comic_cbt_tcb_octal_0644 = 0644U, /**< tar member mode: rw-r--r--. */
} comic_cbt_tcb_octal_0644_t;

/**
 * @enum tcb_dim_t
 * @brief Archive / buffer budgets (tests are magic-number exempt).
 */
typedef enum : uint32_t {
  k_tcb_arc_cap  = 64U * 1024U,  /**< Outer archive build buffer. */
  k_tcb_arena    = 64U * 1024U,  /**< Wrapped-open unwrap arena.  */
  k_tcb_scratch  = 192U * 1024U, /**< XZ session scratch.         */
  k_tcb_page_cap = 8U,           /**< Page-index capacity.        */
  k_tcb_name_cap = 512U,         /**< Name-arena capacity.        */
  k_tcb_out_cap  = 4096U,        /**< Page extraction buffer.     */
} tcb_dim_t;

/** @brief The outer file under test (bare tar or wrapped member). */
static uint8_t s_arc[k_tcb_arc_cap];
/** @brief Length of the outer file. */
static size_t s_arc_len = 0U;
/** @brief Wrapped-open unwrap arena (8-aligned per the contract). */
alignas(8) static uint8_t s_arena[k_tcb_arena];
/** @brief XZ session scratch. */
alignas(8) static uint8_t s_scratch[k_tcb_scratch];
/** @brief Comic page index. */
static ra8_comic_page_t s_pages[k_tcb_page_cap];
/** @brief Comic name arena. */
static char s_names[k_tcb_name_cap];
/** @brief Page extraction buffer. */
static uint8_t s_out[k_tcb_out_cap];
/** @brief Scratch for the inner tar when building wrapped members. */
static uint8_t s_inner[k_tcb_arc_cap];

/** @brief Seek+read callback over ::s_arc. */
static size_t tcb_read(void* ctx, uint64_t off, void* buf, size_t len)
{
  (void)ctx;
  if (off >= (uint64_t)s_arc_len) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s_arc_len - off;
  const size_t   n     = (len > (size_t)avail) ? (size_t)avail : len;
  memcpy(buf, &s_arc[off], n);
  return n;
}

/** @brief Write a NUL-terminated octal field (tar builder helper). */
static void tcb_octal(uint8_t* f, size_t len, uint64_t v)
{
  memset(f, 0, len);
  size_t i = len - 2U;
  do {
    f[i] = (uint8_t)('0' + (v % 8U));
    v /= 8U;
    if (i == 0U) {
      break;
    }
    i -= 1U;
  } while (true);
}

/** @brief Append one ustar member to @p dst; returns the new offset. */
static size_t
tcb_tar_add(uint8_t* dst, size_t off, const char* name, uint8_t type, const void* data, size_t n)
{
  uint8_t* b = &dst[off];
  memset(b, 0, 512U);
  strncpy((char*)&b[0], name, 100U);
  tcb_octal(&b[k_comic_cbt_tcb_octal_100], 8U, k_comic_cbt_tcb_octal_0644);
  tcb_octal(&b[k_comic_cbt_tcb_octal_108], 8U, 0U);
  tcb_octal(&b[k_comic_cbt_tcb_octal_116], 8U, 0U);
  tcb_octal(&b[k_comic_cbt_tcb_octal_124], k_comic_cbt_tcb_octal_12, (uint64_t)n);
  tcb_octal(&b[k_comic_cbt_tcb_octal_136], k_comic_cbt_tcb_octal_12, 0U);
  b[k_comic_cbt_val_156] = type;
  memcpy(&b[257], "ustar", 5U);
  memcpy(&b[263], "00", 2U);
  memset(&b[148], (int)' ', 8U);
  uint32_t sum = 0U;
  for (uint32_t i = 0U; i < k_comic_cbt_i_512; ++i) {
    sum += b[i];
  }
  tcb_octal(&b[k_comic_cbt_tcb_octal_148], k_comic_cbt_tcb_octal_7, (uint64_t)sum);
  b[k_comic_cbt_val_155] = (uint8_t)' ';
  off += k_comic_cbt_i_512;
  if (n > 0U) {
    memcpy(&dst[off], data, n);
    const size_t padded = ((n + 511U) / 512U) * 512U;
    memset(&dst[off + n], 0, padded - n);
    off += padded;
  }
  return off;
}

/** @brief Build the canonical two-page tar comic into @p dst. */
static size_t tcb_build_tar(uint8_t* dst)
{
  const char p1[] = "PAGE-ONE";
  const char p2[] = "PAGE-TWO";
  size_t     off  = 0U;
  off             = tcb_tar_add(dst, off, "dir/", (uint8_t)'5', nullptr, 0U);
  off             = tcb_tar_add(dst, off, "page2.png", (uint8_t)'0', p2, sizeof(p2) - 1U);
  off = tcb_tar_add(dst, off, "notes.txt", (uint8_t)'0', "skip me", k_comic_cbt_tcb_octal_7);
  off = tcb_tar_add(dst, off, "page1.png", (uint8_t)'0', p1, sizeof(p1) - 1U);
  memset(&dst[off], 0, 1024U);
  return off + k_comic_cbt_off_1024;
}

/** @brief Wrap @p payload as a gzip member into ::s_arc. */
static void tcb_gzip_wrap(const uint8_t* payload, size_t n)
{
  size_t off   = 0U;
  s_arc[off++] = k_comic_cbt_s_arc_1f;
  s_arc[off++] = k_comic_cbt_s_arc_8b;
  s_arc[off++] = 8U;
  s_arc[off++] = 0U;
  memset(&s_arc[off], 0, 6U);
  off += 6U;
  const size_t comp = tdefl_compress_mem_to_mem(&s_arc[off],
                                                sizeof(s_arc) - off - 8U,
                                                payload,
                                                n,
                                                (int)TDEFL_DEFAULT_MAX_PROBES);
  TEST_ASSERT(comp > 0U);
  off += comp;
  const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, payload, n);
  for (uint32_t i = 0U; i < 4U; ++i) {
    s_arc[off + i]      = (uint8_t)((crc >> (8U * i)) & k_comic_cbt_val_ff);
    s_arc[off + 4U + i] = (uint8_t)(((uint32_t)n >> (8U * i)) & k_comic_cbt_val_ff);
  }
  s_arc_len = off + 8U;
}

/** @brief Open ::s_arc through the wrapped facade with the shared buffers. */
static ra8_err_t tcb_open_wrapped(ra8_comic_t* c)
{
  return ra8_comic_open_wrapped(c,
                                tcb_read,
                                nullptr,
                                (uint64_t)s_arc_len,
                                s_pages,
                                (uint32_t)k_tcb_page_cap,
                                s_names,
                                (uint32_t)sizeof(s_names),
                                s_arena,
                                sizeof(s_arena),
                                s_scratch,
                                (uint32_t)sizeof(s_scratch));
}

/** @brief Assert the canonical two-page comic opened correctly. */
static void tcb_assert_two_pages(ra8_comic_t* c, ra8_comic_kind_t kind)
{
  TEST_ASSERT_EQ(kind, ra8_comic_kind(c));
  TEST_ASSERT_EQ(2U, ra8_comic_page_count(c));
  char     nb[k_comic_cbt_val_64] = {};
  uint16_t nl                     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_info(c, 0U, nb, sizeof(nb), &nl, nullptr, nullptr));
  TEST_ASSERT_EQ(0, memcmp(nb, "page1.png", 9U)); /* sorted: page1 first */
  size_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_read(c, 0U, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(8U, got);
  TEST_ASSERT_EQ(0, memcmp(s_out, "PAGE-ONE", 8U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_read(c, 1U, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(8U, got);
  TEST_ASSERT_EQ(0, memcmp(s_out, "PAGE-TWO", 8U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(c));
}

/**
 * @test test_cbt_bare_tar_open
 * @brief A bare `.cbt` opens via the tar probe: sorted pages, exact bytes.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the facade detect and CBT backend
 * are single-condition dispatch chains.)
 */
static void test_cbt_bare_tar_open(void)
{
  TEST_BEGIN("cbt: bare tar comic opens + extracts");
  s_arc_len     = tcb_build_tar(s_arc);
  ra8_comic_t c = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&c,
                                tcb_read,
                                nullptr,
                                (uint64_t)s_arc_len,
                                s_pages,
                                (uint32_t)k_tcb_page_cap,
                                s_names,
                                (uint32_t)sizeof(s_names)));
  tcb_assert_two_pages(&c, k_ra8_comic_kind_cbt);

  /* Non-archive bytes are still cleanly refused by every backend. */
  memset(s_arc, 0xA5, 600U);
  s_arc_len = k_comic_cbt_s_arc_len_600;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_comic_open(&c,
                                tcb_read,
                                nullptr,
                                (uint64_t)s_arc_len,
                                s_pages,
                                (uint32_t)k_tcb_page_cap,
                                s_names,
                                (uint32_t)sizeof(s_names)));
  TEST_END("cbt: bare tar comic opens + extracts");
}

/**
 * @test test_cbt_wrapped_gzip_and_xz
 * @brief `.tar.gz` and `.tar.xz` open through the wrapped facade.
 *
 * @par MC/DC:
 * Decision: `!is_gzip && !is_xz` in `ra8_comic_open_wrapped` (2 conditions)
 * - Vector 1: plain ZIP  -> true  (T,T: passthrough)
 * - Vector 2: gzip magic -> false (F,-)
 * - Vector 3: xz magic   -> false (T,F)
 * Vectors 1+2 prove the gzip leg, 1+3 the xz leg: minimal MC/DC.
 */
static void test_cbt_wrapped_gzip_and_xz(void)
{
  TEST_BEGIN("cbt: gzip- and xz-wrapped comics open");
  ra8_comic_t c = {};

  /* .tar.gz: gzip member around the canonical tar. */
  const size_t tar_len = tcb_build_tar(s_inner);
  tcb_gzip_wrap(s_inner, tar_len);
  TEST_ASSERT_EQ(k_ra8_ok, tcb_open_wrapped(&c));
  tcb_assert_two_pages(&c, k_ra8_comic_kind_cbt);

  /* .tar.xz: the committed fixture (same two pages). */
  memcpy(s_arc, k_fx_xz_cbt, sizeof(k_fx_xz_cbt));
  s_arc_len = sizeof(k_fx_xz_cbt);
  TEST_ASSERT_EQ(k_ra8_ok, tcb_open_wrapped(&c));
  tcb_assert_two_pages(&c, k_ra8_comic_kind_cbt);

  /* An XZ file with no XZ scratch provided: rejected, not crashed. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_comic_open_wrapped(&c,
                                        tcb_read,
                                        nullptr,
                                        (uint64_t)s_arc_len,
                                        s_pages,
                                        (uint32_t)k_tcb_page_cap,
                                        s_names,
                                        (uint32_t)sizeof(s_names),
                                        s_arena,
                                        sizeof(s_arena),
                                        nullptr,
                                        0U));

  /* Passthrough: a bare ZIP through the wrapped open lands in CBZ. */
  mz_zip_archive zip = {};
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, 0U) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip, "page1.png", "PAGE-ONE", 8U, MZ_NO_COMPRESSION) ==
              MZ_TRUE);
  void*  zbuf = nullptr;
  size_t zlen = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &zbuf, &zlen) == MZ_TRUE);
  memcpy(s_arc, zbuf, zlen);
  s_arc_len = zlen;
  mz_free(zbuf);
  (void)mz_zip_writer_end(&zip);
  TEST_ASSERT_EQ(k_ra8_ok, tcb_open_wrapped(&c));
  TEST_ASSERT_EQ(k_ra8_comic_kind_cbz, ra8_comic_kind(&c));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  TEST_END("cbt: gzip- and xz-wrapped comics open");
}

/**
 * @brief Open the wrapped fixture archive with the shared page / name / scratch
 *        buffers, varying only the handle, reader, size and arena.
 *
 * @param[out] c         Comic handle (nullptr exercises the c guard).
 * @param[in]  read      Archive reader (nullptr exercises the read guard).
 * @param[in]  file_len  Wrapped-archive size in bytes.
 * @param[in]  arena     Working arena (nullptr / misaligned exercise guards).
 * @param[in]  arena_len Arena size in bytes.
 *
 * @return The `ra8_comic_open_wrapped` result code.
 * @pre The shared fixture buffers are populated.
 * @post No state beyond @p c is modified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static ra8_err_t tcb_open_full(ra8_comic_t*      c,
                               ra8_comic_read_fn read,
                               uint64_t          file_len,
                               uint8_t*          arena,
                               size_t            arena_len)
{
  return ra8_comic_open_wrapped(c,
                                read,
                                nullptr,
                                file_len,
                                s_pages,
                                (uint32_t)k_tcb_page_cap,
                                s_names,
                                (uint32_t)sizeof(s_names),
                                arena,
                                arena_len,
                                s_scratch,
                                (uint32_t)sizeof(s_scratch));
}

/**
 * @brief Exercise the nesting-bomb rejections of the wrapped-open path.
 * @param[out] c Comic handle under test.
 * @return None.
 * @pre The shared fixture buffers are available.
 * @post Every nesting bomb returned its documented rejection code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void tcb_bombs(ra8_comic_t* c)
{
  /* gzip-of-gzip: the classic nesting bomb shape. */
  tcb_gzip_wrap((const uint8_t*)"payload", k_comic_cbt_tcb_octal_7);
  memcpy(s_inner, s_arc, s_arc_len);
  tcb_gzip_wrap(s_inner, s_arc_len);
  TEST_ASSERT_EQ(k_ra8_err_decomp_depth, tcb_open_wrapped(c));
  TEST_ASSERT_EQ(k_ra8_comic_kind_none, ra8_comic_kind(c));

  /* gzip-of-xz: the other wrapper inside a wrapper. */
  tcb_gzip_wrap(k_fx_xz_crc32, sizeof(k_fx_xz_crc32));
  TEST_ASSERT_EQ(k_ra8_err_decomp_depth, tcb_open_wrapped(c));

  /* gzip of non-container garbage: the inner open rejects it. */
  tcb_gzip_wrap((const uint8_t*)"not an archive at all, promise!!", 32U);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, tcb_open_wrapped(c));
}

/**
 * @brief Exercise the null / size / alignment argument guards of wrapped-open.
 * @param[out] c Comic handle under test.
 * @return None.
 * @pre The shared fixture buffers are available.
 * @post Every malformed argument returned its documented rejection code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void tcb_arg_guards(ra8_comic_t* c)
{
  const size_t tar_len = tcb_build_tar(s_inner);
  tcb_gzip_wrap(s_inner, tar_len);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 tcb_open_full(nullptr, tcb_read, (uint64_t)s_arc_len, s_arena, sizeof(s_arena)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 tcb_open_full(c, nullptr, (uint64_t)s_arc_len, s_arena, sizeof(s_arena)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 tcb_open_full(c, tcb_read, (uint64_t)s_arc_len, nullptr, sizeof(s_arena)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, tcb_open_full(c, tcb_read, 0U, s_arena, sizeof(s_arena)));
  /* Misaligned arena (the descriptor slot needs 8-byte alignment). */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    tcb_open_full(c, tcb_read, (uint64_t)s_arc_len, &s_arena[1], sizeof(s_arena) - 1U));
  /* Arena with no room past the descriptor slot. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 tcb_open_full(c, tcb_read, (uint64_t)s_arc_len, s_arena, 8U));
}

/**
 * @test test_cbt_wrapped_bombs_and_guards
 * @brief Nesting bombs and bad wrapped-open arguments fail closed.
 *
 * @par MC/DC:
 * Decision: `inner_gzip || inner_xz` in `ra8_comic_open_wrapped`
 * (2 conditions)
 * - Vector 1: gzip-of-gzip -> true  (T,-: depth bomb)
 * - Vector 2: gzip-of-xz   -> true  (F,T: depth bomb)
 * - Vector 3: gzip-of-tar  -> false (F,F: honest, opens)
 * Vectors 1+3 prove the gzip leg, 2+3 the xz leg: minimal MC/DC (vector 3
 * is the honest `.tar.gz` in test_cbt_wrapped_gzip_and_xz).
 */
static void test_cbt_wrapped_bombs_and_guards(void)
{
  TEST_BEGIN("cbt: nesting bombs + wrapped-open guards");
  ra8_comic_t c = {};
  tcb_bombs(&c);
  tcb_arg_guards(&c);
  TEST_END("cbt: nesting bombs + wrapped-open guards");
}

/**
 * @brief Both CBT entry points fail closed on a tar walker that never went live.
 *
 * @details
 * `ra8_comic_cbt_open` and `ra8_comic_cbt_extract` must reject a comic whose
 * embedded tar walker is not live (`tar.live == 0`) rather than read
 * uninitialised walker state -- the guard that fires when a probe misclassifies
 * a container or a caller reuses a comic across a failed open.
 */
static void test_cbt_dead_walker_guards(void)
{
  TEST_BEGIN("cbt: open/extract reject a dead tar walker");
  ra8_comic_t c = {}; /* zero-init leaves tar.live == 0 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_comic_cbt_open(&c));
  const ra8_comic_page_t p   = {};
  size_t                 got = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_comic_cbt_extract(&c, &p, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(0U, got); /* extract zeroes the out-count before the guard */
  TEST_END("cbt: open/extract reject a dead tar walker");
}

/**
 * @brief Test entry point -- runs the CBT / wrapped-open suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on the first failure.
 */
int32_t main(void)
{
  test_cbt_bare_tar_open();
  test_cbt_wrapped_gzip_and_xz();
  test_cbt_wrapped_bombs_and_guards();
  test_cbt_dead_walker_guards();
  (void)fprintf(stderr, "[OK  ] test_ra8_comic_cbt.c\n");
  return 0;
}
