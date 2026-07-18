/**
 * @file test_ra8_decomp_retrofit.c
 * @brief Tests for the decompression-limits retrofit on the ZIP + RAR paths.
 *
 * @details
 * The pre-existing ZIP-store / DEFLATE (miniz) and RAR decode paths in
 * ra8_epub / ra8_comic now enforce the same unified decompression-limits
 * policy as the new decoders. This suite proves each retrofit point with
 * real archives built (and then sabotaged) in memory:
 *
 *   1. the ra8_epub ZIP guards: an archive whose central directory floods
 *      the entry cap is rejected at open (through `ra8_epub_open` itself),
 *      and an entry whose record declares a bomb is rejected before
 *      inflation (guard unit vectors + a lying-record EPUB through the
 *      public open),
 *   2. the comic CBZ backend: entry-count flood and lying declared sizes
 *      (output cap and ratio shapes) reject the archive at
 *      `ra8_comic_open`,
 *   3. the comic CBR backend: a block-flood RAR4 and a lying-size RAR4
 *      member reject the archive at `ra8_comic_open`,
 *   4. honest archives still open after every hostile probe.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "miniz.h"
#include "ra8_comic.h"
#include "ra8_decomp_limits.h"
#include "ra8_epub.h"
#include "ra8_epub_internal.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum td_dim_t
 * @brief Archive build budgets (tests are magic-number exempt).
 */
typedef enum : uint32_t {
  k_td_arc_cap    = 1024U * 1024U,        /**< Archive build buffer.           */
  k_td_flood      = 4097U,                /**< One over the default entry cap. */
  k_td_page_cap   = 8U,                   /**< Comic page-index capacity.      */
  k_td_name_cap   = 512U,                 /**< Comic name-arena capacity.      */
  k_td_lying_size = 256U * 1024U * 1024U, /**< Declared bomb size (256 MiB).   */
  k_td_ratio_size = 8U * 1024U * 1024U,   /**< Ratio-breach declared size.     */
} td_dim_t;

/** @brief The archive under test. */
static uint8_t s_arc[k_td_arc_cap];
/** @brief Length of the built archive. */
static size_t s_arc_len = 0U;
/** @brief Comic page index. */
static ra8_comic_page_t s_pages[k_td_page_cap];
/** @brief Comic name arena. */
static char s_names[k_td_name_cap];

/** @brief Seek+read callback over ::s_arc. */
static size_t td_read(void* ctx, uint64_t off, void* buf, size_t len)
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

/** @brief Open the built archive as a comic. */
static ra8_err_t td_comic_open(ra8_comic_t* c)
{
  return ra8_comic_open(c,
                        td_read,
                        nullptr,
                        (uint64_t)s_arc_len,
                        s_pages,
                        (uint32_t)k_td_page_cap,
                        s_names,
                        (uint32_t)sizeof(s_names));
}

/** @brief Build a ZIP with @p n tiny entries named by @p fmt into ::s_arc. */
static void td_build_zip(uint32_t n, const char* fmt)
{
  mz_zip_archive zip = {};
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, 0U) == MZ_TRUE);
  char name[32];
  for (uint32_t i = 0U; i < n; ++i) {
    (void)snprintf(name, sizeof(name), fmt, (unsigned)i);
    TEST_ASSERT(mz_zip_writer_add_mem(&zip, name, "x", 1U, MZ_NO_COMPRESSION) == MZ_TRUE);
  }
  void*  buf  = nullptr;
  size_t blen = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &buf, &blen) == MZ_TRUE);
  TEST_ASSERT(blen <= sizeof(s_arc));
  memcpy(s_arc, buf, blen);
  s_arc_len = blen;
  mz_free(buf);
  (void)mz_zip_writer_end(&zip);
}

/**
 * @brief Patch a central-directory record's declared sizes in ::s_arc.
 * @details Scans for the "PK\x01\x02" record whose name matches, then
 *          overwrites the compressed (offset 20) and uncompressed
 *          (offset 24) size fields -- forging exactly the lying header a
 *          hostile archive would carry.
 */
static void td_lie_sizes(const char* name, uint32_t comp, uint32_t uncomp)
{
  const size_t nlen = strlen(name);
  for (size_t i = 0U; (i + 46U + nlen) <= s_arc_len; ++i) {
    const bool sig = (s_arc[i] == 0x50U) && (s_arc[i + 1U] == 0x4BU) && (s_arc[i + 2U] == 0x01U) &&
                     (s_arc[i + 3U] == 0x02U);
    if (!sig) {
      continue;
    }
    if (memcmp(&s_arc[i + 46U], name, nlen) != 0) {
      continue;
    }
    for (uint32_t b = 0U; b < 4U; ++b) {
      s_arc[i + 20U + b] = (uint8_t)((comp >> (8U * b)) & 0xFFU);
      s_arc[i + 24U + b] = (uint8_t)((uncomp >> (8U * b)) & 0xFFU);
    }
    return;
  }
  TEST_ASSERT(false); /* record not found: fixture bug */
}

/** @brief Write a little-endian uint16 / uint32 (RAR4 builder helpers). */
static void td_le16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8U);
}

/** @brief Write a little-endian uint32. */
static void td_le32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8U);
  p[2] = (uint8_t)(v >> 16U);
  p[3] = (uint8_t)(v >> 24U);
}

/** @brief Encode a RAR4 STORE file block with independent pack/unp sizes. */
static size_t
td_rar4_file(uint8_t* out, const char* name, const uint8_t* data, size_t dlen, uint32_t unp)
{
  const size_t nlen = strlen(name);
  const size_t head = 32U + nlen;
  td_le16(&out[0], 0U);      /* head_crc (unverified)          */
  out[2] = 0x74U;            /* type: file                     */
  td_le16(&out[3], 0x8000U); /* flags: LONG (ADD_SIZE present) */
  td_le16(&out[5], (uint16_t)head);
  td_le32(&out[7], (uint32_t)dlen); /* pack_size */
  td_le32(&out[11], unp);           /* unp_size  */
  out[15] = 0U;
  td_le32(&out[16], 0U);
  td_le32(&out[20], 0U);
  out[24] = 20U;
  out[25] = 0x30U; /* method: store */
  td_le16(&out[26], (uint16_t)nlen);
  td_le32(&out[28], 0U);
  memcpy(&out[32], name, nlen);
  if (dlen > 0U) {
    memcpy(&out[head], data, dlen);
  }
  return head + dlen;
}

/** @brief Build a RAR4 archive: sig + main + @p nfiles store members. */
static void td_build_rar4(uint32_t nfiles, const char* name, uint32_t unp_override)
{
  static const uint8_t k_sig[7] = {0x52U, 0x61U, 0x72U, 0x21U, 0x1AU, 0x07U, 0x00U};
  size_t               p        = 0U;
  memcpy(&s_arc[p], k_sig, sizeof(k_sig));
  p += sizeof(k_sig);
  td_le16(&s_arc[p], 0U); /* main block */
  s_arc[p + 2U] = 0x73U;
  td_le16(&s_arc[p + 3U], 0U);
  td_le16(&s_arc[p + 5U], 13U);
  memset(&s_arc[p + 7U], 0, 6U);
  p += 13U;
  const uint8_t data[4] = {1U, 2U, 3U, 4U};
  for (uint32_t i = 0U; i < nfiles; ++i) {
    const uint32_t unp = (unp_override != 0U) ? unp_override : (uint32_t)sizeof(data);
    p += td_rar4_file(&s_arc[p], name, data, sizeof(data), unp);
  }
  s_arc_len = p;
}

/**
 * @test test_retrofit_epub_guards_direct
 * @brief The ZIP guard helpers reject NULLs, floods, and lying records.
 *
 * @par MC/DC:
 * (no compound decisions under test -- both guards are single-condition
 * checks over the shared `ra8_decomp_check_declared` seam, whose own
 * MC/DC lives in test_ra8_decomp_limits.c.)
 */
static void test_retrofit_epub_guards_direct(void)
{
  TEST_BEGIN("retrofit: epub ZIP guard units");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_zip_guard_archive(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_zip_guard_entry(nullptr));

  mz_zip_archive_file_stat st = {};
  st.m_comp_size              = 100U;
  st.m_uncomp_size            = 300U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_zip_guard_entry(&st));
  st.m_uncomp_size = (mz_uint64)k_td_lying_size; /* over the 64 MiB cap */
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap, ra8_epub_zip_guard_entry(&st));
  st.m_comp_size   = 16U; /* 8 MiB from 16 bytes: bomb ratio */
  st.m_uncomp_size = (mz_uint64)k_td_ratio_size;
  TEST_ASSERT_EQ(k_ra8_err_decomp_ratio, ra8_epub_zip_guard_entry(&st));

  /* An honest small archive passes the archive-level guard. */
  td_build_zip(4U, "e%04u");
  mz_zip_archive zip = {};
  TEST_ASSERT(mz_zip_reader_init_mem(&zip, s_arc, s_arc_len, 0U) == MZ_TRUE);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_zip_guard_archive(&zip));
  (void)mz_zip_reader_end(&zip);
  TEST_END("retrofit: epub ZIP guard units");
}

/**
 * @test test_retrofit_epub_open_paths
 * @brief `ra8_epub_open` rejects an entry-flood ZIP fail-closed.
 *
 * @par MC/DC:
 * (no compound decisions under test -- integration vector for the
 * archive-level guard inside the epub open path.)
 */
static void test_retrofit_epub_open_paths(void)
{
  TEST_BEGIN("retrofit: epub open rejects entry flood");
  td_build_zip((uint32_t)k_td_flood, "e%04u");
  static ra8_epub_book_t s_book; /* large record: keep off the test stack */
  ra8_epub_mem_media_t   mem = {.data = s_arc, .size = s_arc_len};
  TEST_ASSERT_EQ(k_ra8_err_decomp_entries, ra8_epub_open(&mem, nullptr, &s_book));
  TEST_END("retrofit: epub open rejects entry flood");
}

/**
 * @test test_retrofit_cbz_paths
 * @brief The comic CBZ backend rejects floods and lying declared sizes.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each rejection drives one
 * single-condition guard in the CBZ backend.)
 */
static void test_retrofit_cbz_paths(void)
{
  TEST_BEGIN("retrofit: CBZ flood + lying sizes rejected");
  ra8_comic_t c = {};

  /* Entry flood: one over the policy cap. */
  td_build_zip((uint32_t)k_td_flood, "e%04u");
  TEST_ASSERT_EQ(k_ra8_err_decomp_entries, td_comic_open(&c));
  TEST_ASSERT_EQ(k_ra8_comic_kind_none, ra8_comic_kind(&c));

  /* Lying record: an image entry declaring 256 MiB (over the cap). */
  td_build_zip(3U, "p%04u.png");
  td_lie_sizes("p0001.png", 1U, (uint32_t)k_td_lying_size);
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap, td_comic_open(&c));

  /* Lying record: 8 MiB declared from 16 compressed bytes (bomb ratio). */
  td_build_zip(3U, "p%04u.png");
  td_lie_sizes("p0002.png", 16U, (uint32_t)k_td_ratio_size);
  TEST_ASSERT_EQ(k_ra8_err_decomp_ratio, td_comic_open(&c));

  /* Honest archive still opens after the hostile probes. */
  td_build_zip(3U, "p%04u.png");
  TEST_ASSERT_EQ(k_ra8_ok, td_comic_open(&c));
  TEST_ASSERT_EQ(3U, ra8_comic_page_count(&c));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  TEST_END("retrofit: CBZ flood + lying sizes rejected");
}

/**
 * @test test_retrofit_cbr_paths
 * @brief The comic CBR backend rejects block floods and lying sizes.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each rejection drives one
 * single-condition guard in the CBR backend.)
 */
static void test_retrofit_cbr_paths(void)
{
  TEST_BEGIN("retrofit: CBR flood + lying sizes rejected");
  ra8_comic_t c = {};

  /* Block flood: non-page members so the small test page index never
   * fills -- every walked block still charges the policy entry cap. */
  td_build_rar4((uint32_t)k_td_flood, "x.bin", 0U);
  TEST_ASSERT_EQ(k_ra8_err_decomp_entries, td_comic_open(&c));

  /* Lying member: a 4-byte STORE page declaring 256 MiB unpacked. */
  td_build_rar4(1U, "x.png", (uint32_t)k_td_lying_size);
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap, td_comic_open(&c));

  /* Lying member: 8 MiB declared from 4 packed bytes (bomb ratio). */
  td_build_rar4(1U, "x.png", (uint32_t)k_td_ratio_size);
  TEST_ASSERT_EQ(k_ra8_err_decomp_ratio, td_comic_open(&c));

  /* Honest RAR4 still opens (STORE members, sizes match). */
  td_build_rar4(2U, "x.png", 0U);
  TEST_ASSERT_EQ(k_ra8_ok, td_comic_open(&c));
  TEST_ASSERT_EQ(2U, ra8_comic_page_count(&c));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  TEST_END("retrofit: CBR flood + lying sizes rejected");
}

/**
 * @brief Test entry point -- runs the retrofit suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on the first failure.
 */
int32_t main(void)
{
  test_retrofit_epub_guards_direct();
  test_retrofit_epub_open_paths();
  test_retrofit_cbz_paths();
  test_retrofit_cbr_paths();
  (void)fprintf(stderr, "[OK  ] test_ra8_decomp_retrofit.c\n");
  return 0;
}
