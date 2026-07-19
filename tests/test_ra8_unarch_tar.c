/**
 * @file test_ra8_unarch_tar.c
 * @brief Tests for the clean-room streaming tar walker (ustar / pax / GNU).
 *
 * @details
 * Builds real tar archives block-by-block in memory (ustar headers with
 * correct checksums, pax extended headers, GNU longname members, base-256
 * size fields) and walks them through `ra8_unarch_tar_*`, proving:
 *
 *   1. honest members enumerate with exact names (ustar prefix join, pax
 *      path, GNU longname), sizes (octal, base-256, pax override), data
 *      offsets, and byte-exact extraction,
 *   2. every hostile shape is rejected fail-closed with the specific
 *      error: corrupt checksum, truncated block/data, lying size fields,
 *      overflowing numerics, malformed pax records, meta floods, and
 *      forged entries handed to the reader,
 *   3. the walk is policy-bounded end-to-end: entry cap, iteration budget,
 *      and the per-member output cap each fire,
 *   4. the promoted field parsers (`ra8_unarch_tar_internal.h`) hit every
 *      rejection branch, with MC/DC vectors for each compound decision.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "ra8_unarch_io.h"
#include "ra8_unarch_tar.h"
#include "ra8_unarch_tar_internal.h"
#include "unity_minimal.h"

/**
 * @enum unarch_tar_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_unarch_tar_block_ff     = 0xFFU,
  k_unarch_tar_f_b256_80    = 0x80U,
  k_unarch_tar_f_neg_c0     = 0xC0U,
  k_unarch_tar_f_pay_81     = 0x81U,
  k_unarch_tar_i_5          = 5U,
  k_tt_len_name      = 100U,
  k_unarch_tar_val_10       = 10,
  k_unarch_tar_val_11       = 11,
  k_unarch_tar_val_12       = 12,
  k_unarch_tar_val_135      = 135,
  k_unarch_tar_val_24       = 24,
} unarch_tar_uint8_const_t;

/**
 * @enum unarch_tar_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_tt_tar_block         = 512U,
  k_unarch_tar_s_arc_len_700 = 700U,
  k_tt_off_magic       = 257,
  k_tt_off_magic_nul       = 262,
  k_tt_off_version       = 263,
  k_unarch_tar_val_264       = 264,
  k_unarch_tar_val_300       = 300,
  k_unarch_tar_val_4096      = 4096,
  k_unarch_tar_val_512       = 512,
  k_unarch_tar_val_600       = 600,
} unarch_tar_uint16_const_t;

/**
 * @enum unarch_tar_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_unarch_tar_tb_header_100000 = 100000U,
} unarch_tar_uint32_const_t;

/** @brief Named double constant used by this file. */
/**
 * @enum unarch_tar_tb_octal_0644_t
 * @brief Named octal mode bits used by this file.
 */
typedef enum : uint16_t {
  k_unarch_tar_tb_octal_0644 = 0644U, /**< tar member mode: rw-r--r--. */
} unarch_tar_tb_octal_0644_t;

/**
 * @enum tt_tar_layout_t
 * @brief POSIX ustar header field offsets and widths, within one 512-byte block.
 *
 * @details
 * Offsets and widths are fixed by the ustar format, so they are named by the
 * FIELD they address rather than by their value. Two pairs share a value but
 * not a role and must not be confused: 100 is both the name width and the mode
 * offset, and 155 is both the prefix width and the offset of the checksum
 * field's trailing space.
 *
 * @see tb_header  Builds a header block using these offsets.
 * @see tb_finish  Computes the checksum over the completed block.
 */
typedef enum : uint16_t {
  k_tt_tar_block        = 512U,  /**< Tar block size, bytes.                  */
  k_tt_end_marker_bytes = 1024U, /**< End-of-archive marker: two zero blocks. */
  k_tt_off_name         = 0U,    /**< name field offset.                      */
  k_tt_len_name         = 100U,  /**< name field width.                       */
  k_tt_off_mode         = 100U,  /**< mode field offset.                      */
  k_tt_off_uid          = 108U,  /**< uid field offset.                       */
  k_tt_off_gid          = 116U,  /**< gid field offset.                       */
  k_tt_len_id           = 8U,    /**< mode/uid/gid field width.               */
  k_tt_off_size         = 124U,  /**< size field offset.                      */
  k_tt_len_size         = 12U,   /**< size field width.                       */
  k_tt_off_mtime        = 136U,  /**< mtime field offset.                     */
  k_tt_off_chksum       = 148U,  /**< checksum field offset.                  */
  k_tt_len_chksum       = 8U,    /**< checksum field width.                   */
  k_tt_chksum_digits    = 7U,    /**< Octal digits written into the checksum. */
  k_tt_off_chksum_pad   = 155U,  /**< Trailing space of the checksum field.   */
  k_tt_off_type         = 156U,  /**< typeflag field offset.                  */
  k_tt_off_magic        = 257U,  /**< "ustar" magic offset.                   */
  k_tt_off_magic_nul    = 262U,  /**< Magic terminator byte offset.           */
  k_tt_off_version      = 263U,  /**< version field offset.                   */
  k_tt_off_prefix       = 345U,  /**< prefix field offset.                    */
  k_tt_len_prefix       = 155U,  /**< prefix field width.                     */
} tt_tar_layout_t;

/** @brief ustar magic field, 5 bytes at offset 257 (no string terminator). */
static const uint8_t k_tt_ustar_magic[] = {'u', 's', 't', 'a', 'r'};
/** @brief ustar version field, the two ASCII digits "00" at offset 263. */
static const uint8_t k_tt_ustar_version[] = {'0', '0'};

/**
 * @enum tt_dim_t
 * @brief Archive / buffer budgets (tests are magic-number exempt).
 */
typedef enum : uint32_t {
  k_tt_arc_cap  = 64U * 1024U, /**< In-memory archive build buffer. */
  k_tt_name_cap = 128U,        /**< Per-query name buffer.          */
  k_tt_out_cap  = 4096U,       /**< Extraction buffer.              */
} tt_dim_t;

/** @brief The archive under test. */
static uint8_t s_arc[k_tt_arc_cap];
/** @brief Length of the built archive. */
static size_t s_arc_len = 0U;
/** @brief Extraction buffer. */
static uint8_t s_out[k_tt_out_cap];
/** @brief Name buffer for the walk. */
static char s_name[k_tt_name_cap];

/** @brief Write a NUL-terminated octal numeric field. */
static void tb_octal(uint8_t* f, size_t len, uint64_t v)
{
  memset(f, 0, len);
  size_t i  = len - 2U; /* last byte stays NUL */
  f[i + 1U] = 0U;
  do {
    f[i] = (uint8_t)('0' + (v % 8U));
    v /= 8U;
    if (i == 0U) {
      break;
    }
    i -= 1U;
  } while (true);
}

/** @brief Compute and store the header checksum ("%06o\0 " form). */
static void tb_finish(uint8_t* block)
{
  memset(&block[k_tt_off_chksum], (int)' ', k_tt_len_chksum);
  uint32_t sum = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_tt_tar_block; ++i) {
    sum += block[i];
  }
  tb_octal(&block[k_tt_off_chksum], k_tt_chksum_digits, (uint64_t)sum);
  block[k_tt_off_chksum_pad] = (uint8_t)' ';
}

/** @brief Build one header block (ustar magic, octal size, checksum). */
static void
tb_header(uint8_t* block, const char* name, const char* prefix, uint8_t type, uint64_t dsize)
{
  memset(block, 0, (size_t)k_tt_tar_block);
  strncpy((char*)&block[k_tt_off_name], name, (size_t)k_tt_len_name);
  tb_octal(&block[k_tt_off_mode], k_tt_len_id, k_unarch_tar_tb_octal_0644);  /* mode  */
  tb_octal(&block[k_tt_off_uid], k_tt_len_id, 0U);                          /* uid   */
  tb_octal(&block[k_tt_off_gid], k_tt_len_id, 0U);                          /* gid   */
  tb_octal(&block[k_tt_off_size], k_tt_len_size, dsize); /* size  */
  tb_octal(&block[k_tt_off_mtime], k_tt_len_size, 0U);    /* mtime */
  block[k_tt_off_type] = type;
  memcpy(&block[k_tt_off_magic], k_tt_ustar_magic, sizeof(k_tt_ustar_magic));
  block[k_tt_off_magic_nul] = 0U;
  memcpy(&block[k_tt_off_version], k_tt_ustar_version, sizeof(k_tt_ustar_version));
  if (prefix != nullptr) {
    strncpy((char*)&block[k_tt_off_prefix], prefix, (size_t)k_tt_len_prefix);
  }
  tb_finish(block);
}

/** @brief Append one member (header + padded data); returns the new offset. */
static size_t tb_add(size_t      off,
                     const char* name,
                     const char* prefix,
                     uint8_t     type,
                     const void* data,
                     size_t      dsize)
{
  tb_header(&s_arc[off], name, prefix, type, (uint64_t)dsize);
  off += k_tt_tar_block;
  if (dsize > 0U) {
    memcpy(&s_arc[off], data, dsize);
    const size_t padded = ((dsize + 511U) / 512U) * 512U;
    memset(&s_arc[off + dsize], 0, padded - dsize);
    off += padded;
  }
  return off;
}

/** @brief Append the end-of-archive marker (two zero blocks). */
static size_t tb_end(size_t off)
{
  memset(&s_arc[off], 0, (size_t)k_tt_end_marker_bytes);
  return off + (size_t)k_tt_end_marker_bytes;
}

/** @brief Open a walker over the built archive with optional limits. */
static ra8_err_t tt_open(ra8_unarch_tar_t* t, ra8_unarch_mem_t* mem, const ra8_decomp_limits_t* lim)
{
  mem->base = s_arc;
  mem->len  = s_arc_len;
  return ra8_unarch_tar_open(t, ra8_unarch_mem_read, mem, (uint64_t)s_arc_len, lim);
}

/**
 * @test test_tar_num_fields
 * @brief The numeric field parser accepts both dialects and rejects abuse.
 *
 * @par MC/DC:
 * Decision: octal digit guard `c < '0'` / `c > '7'` (independent single
 * conditions in sequence, both driven true and false here). The base-256
 * fit checks are likewise independent single conditions, each driven both
 * ways below.
 */
static void test_tar_num_fields(void)
{
  TEST_BEGIN("tar: numeric field parser");
  uint64_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_unarch_tar_num(nullptr, 8U, &v));
  const uint8_t f_ok[12] = "00000000644";
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_unarch_tar_num(f_ok, sizeof(f_ok), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_unarch_tar_num(f_ok, 0U, &v));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_num(f_ok, sizeof(f_ok), &v));
  TEST_ASSERT(v == 0644U);
  const uint8_t f_sp[12] = "     644   "; /* leading spaces + space term */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_num(f_sp, sizeof(f_sp), &v));
  TEST_ASSERT(v == 0644U);
  const uint8_t f_empty[9] = "        "; /* eight spaces: no digits */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_unarch_tar_num(f_empty, 8U, &v));
  const uint8_t f_bad8[12] = "00000000648";
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_unarch_tar_num(f_bad8, sizeof(f_bad8), &v));
  const uint8_t f_alpha[12] = "0000000064a";
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_unarch_tar_num(f_alpha, sizeof(f_alpha), &v));
  uint8_t f_over[k_unarch_tar_val_24];
  memset(f_over, (int)'7', sizeof(f_over));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_unarch_tar_num(f_over, sizeof(f_over), &v));

  /* GNU base-256: 0x80 marker, big-endian payload in the last 8 bytes. */
  uint8_t f_b256[k_unarch_tar_val_12] = {};
  f_b256[0]                           = k_unarch_tar_f_b256_80;
  f_b256[k_unarch_tar_val_10]         = 0x01U;
  f_b256[k_unarch_tar_val_11]         = 0x02U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_num(f_b256, sizeof(f_b256), &v));
  TEST_ASSERT(v == 0x0102U);
  uint8_t f_neg[k_unarch_tar_val_12] = {};
  f_neg[0]                           = k_unarch_tar_f_neg_c0; /* negative two's complement */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_unarch_tar_num(f_neg, sizeof(f_neg), &v));
  uint8_t f_pay[k_unarch_tar_val_12] = {};
  f_pay[0] = k_unarch_tar_f_pay_81; /* payload bits in byte 0: astronomically large */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_unarch_tar_num(f_pay, sizeof(f_pay), &v));
  uint8_t f_high[k_unarch_tar_val_12] = {};
  f_high[0]                           = k_unarch_tar_f_b256_80;
  f_high[2]                           = 0x01U; /* over 64 bits via a high byte */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_unarch_tar_num(f_high, sizeof(f_high), &v));
  TEST_END("tar: numeric field parser");
}

/**
 * @test test_tar_block_primitives
 * @brief Zero-block detection, checksum verification, magic, classify.
 *
 * @par MC/DC:
 * Decision: `(term == 0) || (term == ' ')` in `ra8_unarch_tar_magic_ok`
 * (2 conditions)
 * - Vector 1: POSIX NUL terminator     -> true  (T,-)
 * - Vector 2: old-GNU space terminator -> true  (F,T)
 * - Vector 3: 'X' terminator           -> false (F,F)
 * Vectors 1+3 prove the NUL leg, 2+3 the space leg: minimal MC/DC.
 * Decision: chksum-span test `(i >= start) && (i < end)` in
 * `ra8_unarch_tar_checksum_ok`: a single call sweeps i across before /
 * inside / after the span, producing all three MC/DC vectors.
 */
static void test_tar_block_primitives(void)
{
  TEST_BEGIN("tar: block primitives");
  uint8_t block[k_unarch_tar_val_512] = {};
  TEST_ASSERT(ra8_unarch_tar_block_zero(block));
  block[k_unarch_tar_val_300] = 1U;
  TEST_ASSERT(!ra8_unarch_tar_block_zero(block));

  tb_header(block, "a.png", nullptr, (uint8_t)'0', 4U);
  TEST_ASSERT(ra8_unarch_tar_checksum_ok(block));
  TEST_ASSERT(ra8_unarch_tar_magic_ok(block));
  block[0] ^= k_unarch_tar_block_ff; /* corrupt a name byte: sum shifts */
  TEST_ASSERT(!ra8_unarch_tar_checksum_ok(block));
  block[0] ^= k_unarch_tar_block_ff;
  memset(&block[k_tt_off_chksum], 0, k_tt_len_chksum); /* undecodable chksum field */
  TEST_ASSERT(!ra8_unarch_tar_checksum_ok(block));

  tb_header(block, "a.png", nullptr, (uint8_t)'0', 4U);
  block[k_tt_off_magic_nul] = (uint8_t)' '; /* old-GNU magic terminator */
  TEST_ASSERT(ra8_unarch_tar_magic_ok(block));
  block[k_tt_off_magic_nul] = (uint8_t)'X';
  TEST_ASSERT(!ra8_unarch_tar_magic_ok(block));
  block[k_tt_off_magic] = (uint8_t)'v'; /* not ustar at all */
  TEST_ASSERT(!ra8_unarch_tar_magic_ok(block));

  TEST_ASSERT_EQ(k_ra8_tar_type_file, ra8_unarch_tar_classify((uint8_t)'0'));
  TEST_ASSERT_EQ(k_ra8_tar_type_file, ra8_unarch_tar_classify(0U));
  TEST_ASSERT_EQ(k_ra8_tar_type_dir, ra8_unarch_tar_classify((uint8_t)'5'));
  TEST_ASSERT_EQ(k_ra8_tar_type_pax, ra8_unarch_tar_classify((uint8_t)'x'));
  TEST_ASSERT_EQ(k_ra8_tar_type_meta, ra8_unarch_tar_classify((uint8_t)'g'));
  TEST_ASSERT_EQ(k_ra8_tar_type_meta, ra8_unarch_tar_classify((uint8_t)'K'));
  TEST_ASSERT_EQ(k_ra8_tar_type_longname, ra8_unarch_tar_classify((uint8_t)'L'));
  TEST_ASSERT_EQ(k_ra8_tar_type_other, ra8_unarch_tar_classify((uint8_t)'2'));
  TEST_END("tar: block primitives");
}

/**
 * @brief Parse a malformed pax record and assert fail-closed rejection.
 *
 * @details Runs `ra8_unarch_tar_pax_parse` over @p d with fresh output sinks
 *          (the malformed cases only assert the return code, never the outputs).
 *
 * @param[in] d Encoded pax extended-header data.
 * @param[in] n Byte length of @p d.
 * @return None.
 * @pre @p d is non-null.
 * @post `ra8_unarch_tar_pax_parse` returned k_ra8_err_validation_failed.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void tpx_expect_bad(const uint8_t* d, size_t n)
{
  uint16_t nlen   = 0U;
  bool     hpath  = false;
  uint64_t sz     = 0U;
  bool     hsize  = false;
  char     nb[32] = {};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_unarch_tar_pax_parse(d, n, nb, 32U, &nlen, &hpath, &sz, &hsize));
}

/**
 * @brief Reject every malformed pax-record shape fail-closed.
 * @return None.
 * @pre None.
 * @post Each malformed record returned k_ra8_err_validation_failed.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void tpx_malformed(void)
{
  const uint8_t d_nodig[]   = " path=x\n";
  const uint8_t d_nospace[] = "15path=x/y.png\n";
  const uint8_t d_runout[]  = "123456";
  const uint8_t d_toolong[] = "12345678 a=b\n";
  const uint8_t d_short[]   = "2 a=b\n";     /* reclen cannot cover its prefix */
  const uint8_t d_overrun[] = "99 path=x\n"; /* reclen past the data end       */
  const uint8_t d_nonl[]    = "9 path=xy";   /* no trailing newline            */
  const uint8_t d_noeq[]    = "8 pathx\n";   /* no key/value delimiter         */
  const uint8_t d_badsz[]   = "12 size=12x\n";
  const uint8_t d_emptysz[] = "8 size=\n";
  const uint8_t d_ovsz[]    = "29 size=99999999999999999999\n";
  tpx_expect_bad(d_nodig, sizeof(d_nodig) - 1U);
  tpx_expect_bad(d_nospace, sizeof(d_nospace) - 1U);
  tpx_expect_bad(d_runout, sizeof(d_runout) - 1U);
  tpx_expect_bad(d_toolong, sizeof(d_toolong) - 1U);
  tpx_expect_bad(d_short, sizeof(d_short) - 1U);
  tpx_expect_bad(d_overrun, sizeof(d_overrun) - 1U);
  tpx_expect_bad(d_nonl, sizeof(d_nonl) - 1U);
  tpx_expect_bad(d_noeq, sizeof(d_noeq) - 1U);
  tpx_expect_bad(d_badsz, sizeof(d_badsz) - 1U);
  tpx_expect_bad(d_emptysz, sizeof(d_emptysz) - 1U);
  tpx_expect_bad(d_ovsz, sizeof(d_ovsz) - 1U);
}

/**
 * @test test_tar_pax_records
 * @brief pax record parsing: overrides applied, every malformation rejected.
 *
 * @par MC/DC:
 * Decision: `(key_len == 4) && (memcmp(key, "path", 4) == 0)` (2 conditions,
 * mirrored for "size")
 * - Vector 1: key "path" (len 4, match)    -> true  (T,T)
 * - Vector 2: key "abcd" (len 4, no match) -> false (T,F)
 * - Vector 3: key "mtime" (len 5)          -> false (F,-)
 * Vectors 1+2 prove the memcmp leg, 1+3 the length leg: minimal MC/DC for
 * both the path and the size compounds (the "abcd"/"mtime" records drive
 * both).
 */
static void test_tar_pax_records(void)
{
  TEST_BEGIN("tar: pax record parser");
  uint16_t nlen   = 0U;
  bool     hpath  = false;
  uint64_t sz     = 0U;
  bool     hsize  = false;
  char     nb[32] = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_unarch_tar_pax_parse(nullptr, 4U, nb, 32U, &nlen, &hpath, &sz, &hsize));
  const uint8_t d_ok[] = "16 path=x/y.png\n13 size=1234\n15 abcd=ignore\n16 mtime=123456\n";
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_unarch_tar_pax_parse(d_ok, sizeof(d_ok) - 1U, nullptr, 32U, &nlen, &hpath, &sz, &hsize));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_unarch_tar_pax_parse(d_ok, sizeof(d_ok) - 1U, nb, 32U, &nlen, &hpath, &sz, &hsize));
  TEST_ASSERT(hpath);
  TEST_ASSERT_EQ(7U, nlen);
  TEST_ASSERT_EQ(0, memcmp(nb, "x/y.png", 7U));
  TEST_ASSERT(hsize);
  TEST_ASSERT(sz == 1234U);

  /* Path clamped to a tiny name buffer. */
  nlen  = 0U;
  hpath = false;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_unarch_tar_pax_parse(d_ok, sizeof(d_ok) - 1U, nb, 3U, &nlen, &hpath, &sz, &hsize));
  TEST_ASSERT_EQ(3U, nlen);

  /* Malformed shapes, each rejected fail-closed. */
  tpx_malformed();
  TEST_END("tar: pax record parser");
}

/**
 * @brief Walk and verify the pax member, the symlink entry and the end marker.
 *
 * @param[in,out] t  Open tar walker.
 * @param[in,out] e  Entry cursor positioned at the longname member (updated).
 * @param[in]     d3 Expected pax-member payload for the byte-exact check.
 *
 * @return None.
 * @pre @p e references the entry just before the pax member.
 * @post The pax overrides, the symlink classification and the end marker all
 *       matched.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void tar_walk_tail(ra8_unarch_tar_t* t, ra8_unarch_tar_entry_t* e, const char* d3)
{
  size_t got = 0U;
  /* 4: the pax member (path + size overrides applied). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(t, e->next_off, s_name, sizeof(s_name), e));
  TEST_ASSERT_EQ(1U, e->is_file);
  TEST_ASSERT_EQ(0, memcmp(s_name, "pax/page2.png", 13U));
  TEST_ASSERT(e->size == (uint64_t)(strlen(d3)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_read(t, e, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(0, memcmp(s_out, d3, got));
  /* 5: the symlink -- enumerated, neither file nor dir. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(t, e->next_off, s_name, sizeof(s_name), e));
  TEST_ASSERT_EQ(0U, e->is_file);
  TEST_ASSERT_EQ(0U, e->is_dir);
  /* 6: the end marker. */
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_unarch_tar_next(t, e->next_off, s_name, sizeof(s_name), e));
}

/**
 * @test test_tar_walk_honest_archive
 * @brief A mixed honest archive enumerates and extracts byte-exactly.
 *
 * @par MC/DC:
 * Decision: `(type == k_ra8_tar_type_pax) || (type == k_ra8_tar_type_longname)`
 * in libs/ra8_unarch/src/ra8_unarch_tar.c@ra8_unarch_tar_next (2 conditions)
 * - Vector 1: pax member       -> true  (T,-)
 * - Vector 2: longname member  -> true  (F,T)
 * - Vector 3: regular file     -> false (F,F)
 * Vectors 1+3 prove the pax leg, 2+3 the longname leg: minimal MC/DC.
 */
static void test_tar_walk_honest_archive(void)
{
  TEST_BEGIN("tar: honest archive walk + extract");
  const char d1[] = "PAGE-ONE-BYTES";
  const char d2[] = "page two content, longer than one";
  const char d3[] = "pax-sized";
  size_t     off  = 0U;
  off             = tb_add(off, "dir/", nullptr, (uint8_t)'5', nullptr, 0U);
  off             = tb_add(off, "b.png", "vol1/ch1", (uint8_t)'0', d1, sizeof(d1) - 1U);
  /* GNU longname member: 'L' data carries the real name. */
  const char lname[] = "a-very/long/nested/path/page0001.png";
  off                = tb_add(off, "ignored", nullptr, (uint8_t)'L', lname, sizeof(lname));
  off                = tb_add(off, "short", nullptr, (uint8_t)'0', d2, sizeof(d2) - 1U);
  /* pax member: path + size records override the header fields. */
  const char pax[] = "22 path=pax/page2.png\n10 size=9\n";
  off              = tb_add(off, "ignored2", nullptr, (uint8_t)'x', pax, sizeof(pax) - 1U);
  off              = tb_add(off, "hdrname", nullptr, (uint8_t)'0', d3, sizeof(d3) - 1U);
  /* A symlink member: enumerated, not a file. */
  off       = tb_add(off, "link", nullptr, (uint8_t)'2', nullptr, 0U);
  off       = tb_end(off);
  s_arc_len = off;

  ra8_unarch_tar_t t   = {};
  ra8_unarch_mem_t mem = {};
  TEST_ASSERT_EQ(k_ra8_ok, tt_open(&t, &mem, nullptr));

  ra8_unarch_tar_entry_t e = {};
  /* 1: the directory. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(1U, e.is_dir);
  TEST_ASSERT_EQ(0U, e.is_file);
  TEST_ASSERT_EQ(4U, e.name_len);
  TEST_ASSERT_EQ(0, memcmp(s_name, "dir/", 4U));
  /* 2: prefix-joined file. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, e.next_off, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(1U, e.is_file);
  TEST_ASSERT_EQ(0, memcmp(s_name, "vol1/ch1/b.png", 14U));
  TEST_ASSERT(e.size == (uint64_t)(sizeof(d1) - 1U));
  size_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_read(&t, &e, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(sizeof(d1) - 1U, got);
  TEST_ASSERT_EQ(0, memcmp(s_out, d1, got));
  /* 3: the longname member (its 'L' prelude was consumed transparently). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, e.next_off, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(1U, e.is_file);
  TEST_ASSERT_EQ(sizeof(lname) - 1U, e.name_len);
  TEST_ASSERT_EQ(0, memcmp(s_name, lname, e.name_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_read(&t, &e, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(0, memcmp(s_out, d2, got));
  tar_walk_tail(&t, &e, d3);
  TEST_END("tar: honest archive walk + extract");
}

/**
 * @test test_tar_open_probe_guards
 * @brief Probe / open argument validation and non-tar rejection.
 *
 * @par MC/DC:
 * (no compound decisions under test -- probe and open are chains of
 * independent single-condition guards.)
 */
static void test_tar_open_probe_guards(void)
{
  TEST_BEGIN("tar: probe + open guards");
  s_arc_len = (size_t)tb_end(0U);
  TEST_ASSERT(!ra8_unarch_tar_probe(nullptr, 512U));
  TEST_ASSERT(!ra8_unarch_tar_probe(s_arc, 511U));
  TEST_ASSERT(!ra8_unarch_tar_probe(s_arc, 512U)); /* zero block: no magic */
  uint8_t good[k_unarch_tar_val_512] = {};
  tb_header(good, "x.png", nullptr, (uint8_t)'0', 0U);
  TEST_ASSERT(ra8_unarch_tar_probe(good, sizeof(good)));
  good[k_tt_off_magic_nul] = (uint8_t)'X'; /* magic passes "ustar", term fails */
  TEST_ASSERT(!ra8_unarch_tar_probe(good, sizeof(good)));

  ra8_unarch_tar_t t   = {};
  ra8_unarch_mem_t mem = {.base = s_arc, .len = s_arc_len};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_unarch_tar_open(nullptr, ra8_unarch_mem_read, &mem, 1024U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_unarch_tar_open(&t, nullptr, &mem, 1024U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_unarch_tar_open(&t, ra8_unarch_mem_read, &mem, 511U, nullptr));
  ra8_decomp_limits_t bad = ra8_decomp_limits_default();
  bad.max_entries         = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_unarch_tar_open(&t, ra8_unarch_mem_read, &mem, 1024U, &bad));
  /* Zero blocks are not a tar header: refused at open. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_unarch_tar_open(&t, ra8_unarch_mem_read, &mem, (uint64_t)s_arc_len, nullptr));
  /* A backing that cannot serve one whole block. */
  mem.len = k_tt_len_name;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_unarch_tar_open(&t, ra8_unarch_mem_read, &mem, 1024U, nullptr));

  /* next/read on a dead walker. */
  ra8_unarch_tar_entry_t e = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));
  size_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_unarch_tar_read(&t, &e, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_unarch_tar_next(nullptr, 0U, s_name, sizeof(s_name), &e));
  TEST_END("tar: probe + open guards");
}

/**
 * @brief Reject pax meta-floods, oversize/malformed pax, and handle clean EOF.
 *
 * @param[in,out] t    Tar walker handle (reused per case).
 * @param[in,out] mem  Backing-memory descriptor (reused per case).
 * @param[in,out] e    Entry cursor (reused per case).
 * @param[in]     data Small regular-file payload for the trailing member.
 *
 * @return None.
 * @pre The shared archive buffer is available.
 * @post Every hostile pax shape was rejected and the marker-less EOF returned
 *       not_found cleanly.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void tar_hostile_pax_floods(ra8_unarch_tar_t*       t,
                                   ra8_unarch_mem_t*       mem,
                                   ra8_unarch_tar_entry_t* e,
                                   const char*             data)
{
  const size_t dlen = strlen(data);

  /* Meta flood: more pax blocks than the per-member cap. */
  const char paxr[] = "11 size=5\n";
  size_t     off    = 0U;
  for (uint32_t i = 0U; i < k_unarch_tar_i_5; ++i) {
    off = tb_add(off, "x", nullptr, (uint8_t)'x', paxr, sizeof(paxr) - 1U);
  }
  off       = tb_add(off, "a.png", nullptr, (uint8_t)'0', data, dlen);
  s_arc_len = tb_end(off);
  TEST_ASSERT_EQ(k_ra8_ok, tt_open(t, mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_unarch_tar_next(t, 0U, s_name, sizeof(s_name), e));

  /* Oversized pax data (over k_ra8_unarch_tar_pax_max). */
  uint8_t big[k_unarch_tar_val_4096];
  memset(big, (int)'a', sizeof(big));
  off       = tb_add(0U, "x", nullptr, (uint8_t)'x', big, sizeof(big));
  off       = tb_add(off, "a.png", nullptr, (uint8_t)'0', data, dlen);
  s_arc_len = tb_end(off);
  TEST_ASSERT_EQ(k_ra8_ok, tt_open(t, mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_unarch_tar_next(t, 0U, s_name, sizeof(s_name), e));

  /* Malformed pax data inside a well-formed member. */
  const char paxbad[] = "99 path=x\n";
  off                 = tb_add(0U, "x", nullptr, (uint8_t)'x', paxbad, sizeof(paxbad) - 1U);
  off                 = tb_add(off, "a.png", nullptr, (uint8_t)'0', data, dlen);
  s_arc_len           = tb_end(off);
  TEST_ASSERT_EQ(k_ra8_ok, tt_open(t, mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_unarch_tar_next(t, 0U, s_name, sizeof(s_name), e));

  /* EOF without an end marker: a clean not_found, not a crash. */
  s_arc_len = tb_add(0U, "a.png", nullptr, (uint8_t)'0', data, dlen);
  TEST_ASSERT_EQ(k_ra8_ok, tt_open(t, mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(t, 0U, s_name, sizeof(s_name), e));
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_unarch_tar_next(t, e->next_off, s_name, sizeof(s_name), e));
}

/**
 * @test test_tar_hostile_headers
 * @brief Corrupt, lying, and flooding headers are rejected fail-closed.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each hostile shape drives one
 * single-condition rejection in the walker.)
 */
static void test_tar_hostile_headers(void)
{
  TEST_BEGIN("tar: hostile headers rejected");
  const char             data[] = "0123456789";
  ra8_unarch_tar_t       t      = {};
  ra8_unarch_mem_t       mem    = {};
  ra8_unarch_tar_entry_t e      = {};

  /* Corrupt checksum on the second member. */
  size_t       off    = tb_add(0U, "a.png", nullptr, (uint8_t)'0', data, sizeof(data) - 1U);
  const size_t second = off;
  off                 = tb_add(off, "b.png", nullptr, (uint8_t)'0', data, sizeof(data) - 1U);
  s_arc_len           = tb_end(off);
  s_arc[second] ^= k_unarch_tar_block_ff;
  TEST_ASSERT_EQ(k_ra8_ok, tt_open(&t, &mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_unarch_tar_next(&t, e.next_off, s_name, sizeof(s_name), &e));

  /* Lying size: data area overruns the archive. */
  s_arc_len = tb_add(0U, "a.png", nullptr, (uint8_t)'0', data, sizeof(data) - 1U);
  tb_header(&s_arc[0],
            "a.png",
            nullptr,
            (uint8_t)'0',
            k_unarch_tar_tb_header_100000); /* size lies */
  TEST_ASSERT_EQ(k_ra8_ok, tt_open(&t, &mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));

  /* Truncated data: the archive ends inside the member's data area. */
  uint8_t big_member[k_unarch_tar_val_600];
  memset(big_member, (int)'m', sizeof(big_member));
  s_arc_len = tb_add(0U, "a.png", nullptr, (uint8_t)'0', big_member, sizeof(big_member));
  s_arc_len -= k_unarch_tar_s_arc_len_700; /* cut into the 600-byte data area itself */
  TEST_ASSERT_EQ(k_ra8_ok, tt_open(&t, &mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));

  tar_hostile_pax_floods(&t, &mem, &e, data);

  TEST_END("tar: hostile headers rejected");
}

/**
 * @test test_tar_policy_bounds
 * @brief Entry cap, iteration budget, and per-member output cap each fire.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each axis is one single-condition
 * budget charge in `ra8_decomp_limits.c`, driven here through the walker.)
 */
static void test_tar_policy_bounds(void)
{
  TEST_BEGIN("tar: policy bounds fire");
  const char data[] = "0123456789";
  size_t     off    = 0U;
  for (uint32_t i = 0U; i < 4U; ++i) {
    off = tb_add(off, "p.png", nullptr, (uint8_t)'0', data, sizeof(data) - 1U);
  }
  s_arc_len = tb_end(off);

  ra8_unarch_tar_t       t   = {};
  ra8_unarch_mem_t       mem = {};
  ra8_unarch_tar_entry_t e   = {};

  /* Entry cap: the third member crosses a 2-entry policy. */
  ra8_decomp_limits_t lim = ra8_decomp_limits_default();
  lim.max_entries         = 2U;
  TEST_ASSERT_EQ(k_ra8_ok, tt_open(&t, &mem, &lim));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, e.next_off, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(k_ra8_err_decomp_entries,
                 ra8_unarch_tar_next(&t, e.next_off, s_name, sizeof(s_name), &e));

  /* Iteration budget: every examined block charges one turn. */
  lim                = ra8_decomp_limits_default();
  lim.max_iterations = 2U;
  TEST_ASSERT_EQ(k_ra8_ok, tt_open(&t, &mem, &lim));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, e.next_off, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(k_ra8_err_decomp_iterations,
                 ra8_unarch_tar_next(&t, e.next_off, s_name, sizeof(s_name), &e));

  /* Per-member output cap: a member larger than the policy allows. */
  lim                  = ra8_decomp_limits_default();
  lim.max_output_bytes = 4U;
  TEST_ASSERT_EQ(k_ra8_ok, tt_open(&t, &mem, &lim));
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap,
                 ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));
  TEST_END("tar: policy bounds fire");
}

/**
 * @test test_tar_read_guards
 * @brief The extractor re-validates entries and buffers fail-closed.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the extractor is a chain of
 * independent single-condition guards, each driven both ways here.)
 */
static void test_tar_read_guards(void)
{
  TEST_BEGIN("tar: extractor guards");
  const char data[] = "0123456789";
  size_t     off    = tb_add(0U, "dir/", nullptr, (uint8_t)'5', nullptr, 0U);
  off               = tb_add(off, "a.png", nullptr, (uint8_t)'0', data, sizeof(data) - 1U);
  off               = tb_add(off, "empty.png", nullptr, (uint8_t)'0', nullptr, 0U);
  s_arc_len         = tb_end(off);

  ra8_unarch_tar_t t   = {};
  ra8_unarch_mem_t mem = {};
  TEST_ASSERT_EQ(k_ra8_ok, tt_open(&t, &mem, nullptr));
  ra8_unarch_tar_entry_t dir = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &dir));
  ra8_unarch_tar_entry_t file = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, dir.next_off, s_name, sizeof(s_name), &file));
  ra8_unarch_tar_entry_t empty = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, file.next_off, s_name, sizeof(s_name), &empty));

  size_t got = 1U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_unarch_tar_read(nullptr, &file, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_unarch_tar_read(&t, nullptr, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_unarch_tar_read(&t, &file, nullptr, sizeof(s_out), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_unarch_tar_read(&t, &file, s_out, sizeof(s_out), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_unarch_tar_read(&t, &dir, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_unarch_tar_read(&t, &file, s_out, 4U, &got));

  /* Forged entries: data area outside / overrunning the archive. */
  ra8_unarch_tar_entry_t forged = file;
  forged.data_off               = (uint64_t)s_arc_len + k_tt_tar_block;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_unarch_tar_read(&t, &forged, s_out, sizeof(s_out), &got));
  forged      = file;
  forged.size = (uint64_t)s_arc_len; /* overruns from a valid offset */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_unarch_tar_read(&t, &forged, s_out, (size_t)forged.size + 1U, &got));

  /* Empty member: k_ra8_ok with zero bytes. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_read(&t, &empty, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(0U, got);

  /* Honest read still works after the hostile probes. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_read(&t, &file, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(sizeof(data) - 1U, got);
  TEST_ASSERT_EQ(0, memcmp(s_out, data, got));

  /* A backing that shrinks under the reader (short read). */
  mem.len = (size_t)file.data_off + 2U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_unarch_tar_read(&t, &file, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_END("tar: extractor guards");
}

/**
 * @test test_tar_base256_and_gnu_magic
 * @brief A base-256 size field and old-GNU magic walk end-to-end.
 *
 * @par MC/DC:
 * (no compound decisions under test -- integration vector for the
 * base-256 leg of `ra8_unarch_tar_num` and the space-terminated magic.)
 */
static void test_tar_base256_and_gnu_magic(void)
{
  TEST_BEGIN("tar: base-256 size + old-GNU magic");
  const char data[] = "binary-size-member";
  size_t     off    = tb_add(0U, "b256.png", nullptr, (uint8_t)'0', data, sizeof(data) - 1U);
  /* Rewrite the size field as base-256 and re-checksum. */
  memset(&s_arc[k_tt_off_size], 0, k_tt_len_size);
  s_arc[k_tt_off_size] = k_unarch_tar_f_b256_80;
  s_arc[k_unarch_tar_val_135]      = (uint8_t)(sizeof(data) - 1U);
  /* Old-GNU magic variant: "ustar" + ' ' + ' '. */
  s_arc[k_tt_off_magic_nul] = (uint8_t)' ';
  s_arc[k_tt_off_version] = (uint8_t)' ';
  s_arc[k_unarch_tar_val_264] = 0U;
  tb_finish(&s_arc[0]);
  s_arc_len = tb_end(off);

  ra8_unarch_tar_t t   = {};
  ra8_unarch_mem_t mem = {};
  TEST_ASSERT_EQ(k_ra8_ok, tt_open(&t, &mem, nullptr));
  ra8_unarch_tar_entry_t e = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));
  TEST_ASSERT(e.size == (uint64_t)(sizeof(data) - 1U));
  size_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_read(&t, &e, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(0, memcmp(s_out, data, got));
  TEST_END("tar: base-256 size + old-GNU magic");
}

/**
 * @brief Test entry point -- runs the tar walker suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on the first failure.
 */
int32_t main(void)
{
  test_tar_num_fields();
  test_tar_block_primitives();
  test_tar_pax_records();
  test_tar_walk_honest_archive();
  test_tar_open_probe_guards();
  test_tar_hostile_headers();
  test_tar_policy_bounds();
  test_tar_read_guards();
  test_tar_base256_and_gnu_magic();
  (void)fprintf(stderr, "[OK  ] test_ra8_unarch_tar.c\n");
  return 0;
}
