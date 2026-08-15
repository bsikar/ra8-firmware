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

#include "ra8_attributes.h"
#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "ra8_unarch_io.h"
#include "ra8_unarch_tar.h"
#include "ra8_unarch_tar_internal.h"
#include "support/ra8_unarch_tar_fixture.h"
#include "unity_minimal.h"

/**
 * @test internal_test_tar_num_fields
 * @brief The numeric field parser accepts both dialects and rejects abuse.
 *
 * @par MC/DC:
 * Decision: octal digit guard `c < '0'` / `c > '7'` (independent single
 * conditions in sequence, both driven true and false here). The base-256
 * fit checks are likewise independent single conditions, each driven both
 * ways below.

 *
 * @details Covers octal and GNU base-256 numeric decoding, including empty, malformed, negative, and overflowing fields.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_tar_num_fields(void)
{
  TEST_BEGIN("tar: numeric field parser");
  uint64_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_unarch_tar_num(nullptr, 8U, &v));
  const uint8_t f_ok[12] = "00000000644";
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_unarch_tar_num(f_ok, sizeof(f_ok), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_unarch_tar_num(f_ok, 0U, &v));

  TEST_ASSERT_EQ(k_ra8_ok, priv_unarch_tar_num(f_ok, sizeof(f_ok), &v));
  TEST_ASSERT(v == 0644U);
  const uint8_t f_sp[12] = "     644   "; /* leading spaces + space term */
  TEST_ASSERT_EQ(k_ra8_ok, priv_unarch_tar_num(f_sp, sizeof(f_sp), &v));
  TEST_ASSERT(v == 0644U);
  const uint8_t f_empty[9] = "        "; /* eight spaces: no digits */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_unarch_tar_num(f_empty, 8U, &v));
  const uint8_t f_bad8[12] = "00000000648";
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_unarch_tar_num(f_bad8, sizeof(f_bad8), &v));
  const uint8_t f_alpha[12] = "0000000064a";
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_unarch_tar_num(f_alpha, sizeof(f_alpha), &v));
  uint8_t f_over[k_t_num_overlong];
  memset(f_over, (int)'7', sizeof(f_over));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_unarch_tar_num(f_over, sizeof(f_over), &v));

  /* GNU base-256: 0x80 marker, big-endian payload in the last 8 bytes. */
  uint8_t f_b256[k_tt_len_size] = {};
  f_b256[0]                     = k_t_b256_positive;
  f_b256[k_t_b256_hi_byte]      = 0x01U;
  f_b256[k_t_b256_lo_byte]      = 0x02U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_unarch_tar_num(f_b256, sizeof(f_b256), &v));
  TEST_ASSERT(v == 0x0102U);
  uint8_t f_neg[k_tt_len_size] = {};
  f_neg[0]                     = k_t_b256_negative; /* negative two's complement */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_unarch_tar_num(f_neg, sizeof(f_neg), &v));
  uint8_t f_pay[k_tt_len_size] = {};
  f_pay[0] = k_t_b256_payload; /* payload bits in byte 0: astronomically large */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_unarch_tar_num(f_pay, sizeof(f_pay), &v));
  uint8_t f_high[k_tt_len_size] = {};
  f_high[0]                     = k_t_b256_positive;
  f_high[2]                     = 0x01U; /* over 64 bits via a high byte */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_unarch_tar_num(f_high, sizeof(f_high), &v));
  TEST_END("tar: numeric field parser");
}

/**
 * @test internal_test_tar_block_primitives
 * @brief Zero-block detection, checksum verification, magic, classify.
 *
 * @par MC/DC:
 * Decision: `(term == 0) || (term == ' ')` in `priv_unarch_tar_magic_ok`
 * (2 conditions)
 * - Vector 1: POSIX NUL terminator     -> true  (T,-)
 * - Vector 2: old-GNU space terminator -> true  (F,T)
 * - Vector 3: 'X' terminator           -> false (F,F)
 * Vectors 1+3 prove the NUL leg, 2+3 the space leg: minimal MC/DC.
 * Decision: chksum-span test `(i >= start) && (i < end)` in
 * `priv_unarch_tar_checksum_ok`: a single call sweeps i across before /
 * inside / after the span, producing all three MC/DC vectors.

 *
 * @details Checks zero-block detection, checksums, ustar/GNU magic, and exhaustive typeflag normalization.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_tar_block_primitives(void)
{
  TEST_BEGIN("tar: block primitives");
  uint8_t block[k_tt_tar_block] = {};
  TEST_ASSERT(priv_unarch_tar_block_zero(block));
  block[k_t_nonzero_off] = 1U;
  TEST_ASSERT(!priv_unarch_tar_block_zero(block));

  internal_tb_header(block, "a.png", nullptr, (uint8_t)'0', 4U);
  TEST_ASSERT(priv_unarch_tar_checksum_ok(block));
  TEST_ASSERT(priv_unarch_tar_magic_ok(block));
  block[0] ^= k_t_corrupt_mask; /* corrupt a name byte: sum shifts */
  TEST_ASSERT(!priv_unarch_tar_checksum_ok(block));
  block[0] ^= k_t_corrupt_mask;
  memset(&block[k_tt_off_chksum], 0, k_tt_len_chksum); /* undecodable chksum field */
  TEST_ASSERT(!priv_unarch_tar_checksum_ok(block));

  internal_tb_header(block, "a.png", nullptr, (uint8_t)'0', 4U);
  block[k_tt_off_magic_nul] = (uint8_t)' '; /* old-GNU magic terminator */
  TEST_ASSERT(priv_unarch_tar_magic_ok(block));
  block[k_tt_off_magic_nul] = (uint8_t)'X';
  TEST_ASSERT(!priv_unarch_tar_magic_ok(block));
  block[k_tt_off_magic] = (uint8_t)'v'; /* not ustar at all */
  TEST_ASSERT(!priv_unarch_tar_magic_ok(block));

  TEST_ASSERT_EQ(k_ra8_tar_type_file, priv_unarch_tar_classify((uint8_t)'0'));
  TEST_ASSERT_EQ(k_ra8_tar_type_file, priv_unarch_tar_classify(0U));
  TEST_ASSERT_EQ(k_ra8_tar_type_dir, priv_unarch_tar_classify((uint8_t)'5'));
  TEST_ASSERT_EQ(k_ra8_tar_type_pax, priv_unarch_tar_classify((uint8_t)'x'));
  TEST_ASSERT_EQ(k_ra8_tar_type_meta, priv_unarch_tar_classify((uint8_t)'g'));
  TEST_ASSERT_EQ(k_ra8_tar_type_meta, priv_unarch_tar_classify((uint8_t)'K'));
  TEST_ASSERT_EQ(k_ra8_tar_type_longname, priv_unarch_tar_classify((uint8_t)'L'));
  TEST_ASSERT_EQ(k_ra8_tar_type_other, priv_unarch_tar_classify((uint8_t)'2'));
  TEST_END("tar: block primitives");
}

/**
 * @brief Parse a malformed pax record and assert fail-closed rejection.
 *
 * @details Runs `priv_unarch_tar_pax_parse` over @p d with fresh output sinks
 *          (the malformed cases only assert the return code, never the outputs).
 *
 * @param[in] d Encoded pax extended-header data.
 * @param[in] n Byte length of @p d.
 * @pre @p d is non-null.
 * @post `priv_unarch_tar_pax_parse` returned k_ra8_err_validation_failed.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0

 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @post All writes remain within caller-owned fixture storage.
 */
RA8_INTERNAL
static void internal_tpx_expect_bad(const uint8_t* d, size_t n)
{
  uint16_t nlen   = 0U;
  bool     hpath  = false;
  uint64_t sz     = 0U;
  bool     hsize  = false;
  char     nb[32] = {};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 priv_unarch_tar_pax_parse(d, n, nb, 32U, &nlen, &hpath, &sz, &hsize));
}

/**
 * @brief Reject every malformed pax-record shape fail-closed.
 * @pre None.
 * @post Each malformed record returned k_ra8_err_validation_failed.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0

 *
 * @details Enumerates malformed pax length, delimiter, newline, overflow, and record-boundary shapes.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @post All writes remain within caller-owned fixture storage.
 */
RA8_INTERNAL
static void internal_tpx_malformed(void)
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
  internal_tpx_expect_bad(d_nodig, sizeof(d_nodig) - 1U);
  internal_tpx_expect_bad(d_nospace, sizeof(d_nospace) - 1U);
  internal_tpx_expect_bad(d_runout, sizeof(d_runout) - 1U);
  internal_tpx_expect_bad(d_toolong, sizeof(d_toolong) - 1U);
  internal_tpx_expect_bad(d_short, sizeof(d_short) - 1U);
  internal_tpx_expect_bad(d_overrun, sizeof(d_overrun) - 1U);
  internal_tpx_expect_bad(d_nonl, sizeof(d_nonl) - 1U);
  internal_tpx_expect_bad(d_noeq, sizeof(d_noeq) - 1U);
  internal_tpx_expect_bad(d_badsz, sizeof(d_badsz) - 1U);
  internal_tpx_expect_bad(d_emptysz, sizeof(d_emptysz) - 1U);
  internal_tpx_expect_bad(d_ovsz, sizeof(d_ovsz) - 1U);
}

/**
 * @test internal_test_tar_pax_records
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

 *
 * @details Validates honest path and size overrides, unknown-key skipping, truncation, and zero-capacity path handling.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_tar_pax_records(void)
{
  TEST_BEGIN("tar: pax record parser");
  uint16_t nlen   = 0U;
  bool     hpath  = false;
  uint64_t sz     = 0U;
  bool     hsize  = false;
  char     nb[32] = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_unarch_tar_pax_parse(nullptr, 4U, nb, 32U, &nlen, &hpath, &sz, &hsize));
  const uint8_t d_ok[] = "16 path=x/y.png\n13 size=1234\n15 abcd=ignore\n16 mtime=123456\n";
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    priv_unarch_tar_pax_parse(d_ok, sizeof(d_ok) - 1U, nullptr, 32U, &nlen, &hpath, &sz, &hsize));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_unarch_tar_pax_parse(d_ok, sizeof(d_ok) - 1U, nb, 32U, &nlen, &hpath, &sz, &hsize));
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
    priv_unarch_tar_pax_parse(d_ok, sizeof(d_ok) - 1U, nb, 3U, &nlen, &hpath, &sz, &hsize));
  TEST_ASSERT_EQ(3U, nlen);

  /* Malformed shapes, each rejected fail-closed. */
  internal_tpx_malformed();
  TEST_END("tar: pax record parser");
}

/**
 * @brief Walk and verify the pax member, the symlink entry and the end marker.
 *
 * @param[in,out] t  Open tar walker.
 * @param[in,out] e  Entry cursor positioned at the longname member (updated).
 * @param[in]     d3 Expected pax-member payload for the byte-exact check.
 *
 * @pre @p e references the entry just before the pax member.
 * @post The pax overrides, the symlink classification and the end marker all
 *       matched.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0

 *
 * @details Checks the final GNU-longname member, byte-exact extraction, and clean end-of-archive transition.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @post All writes remain within caller-owned fixture storage.
 */
RA8_INTERNAL
static void internal_tar_walk_tail(ra8_unarch_tar_t* t, ra8_unarch_tar_entry_t* e, const char* d3)
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

/** @brief First file member's payload; short enough to sit in one block. */
static const char s_honest_d1[] = "PAGE-ONE-BYTES";
/** @brief Longname member's payload; deliberately longer than @ref s_honest_d1. */
static const char s_honest_d2[] = "page two content, longer than one";
/** @brief Pax member's payload; its length must match the pax `size` record. */
static const char s_honest_d3[] = "pax-sized";
/** @brief Real path the GNU 'L' prelude carries for the longname member. */
static const char s_honest_lname[] = "a-very/long/nested/path/page0001.png";
/** @brief Pax extended records overriding the following header's path and size. */
static const char s_honest_pax[] = "22 path=pax/page2.png\n10 size=9\n";

/**
 * @brief Build the mixed "honest" tar fixture into the archive buffer.
 *
 * @details
 * Lays out one member of each kind the walker must handle, in an order that
 * makes each one's handling observable: a directory, a file whose name is
 * split across the header's prefix and name fields, a GNU longname member
 * whose 'L' prelude carries the real path, a pax member whose extended records
 * override the header that follows it, and a symlink. The two prelude members
 * ('L' and 'x') are given throwaway header names, so a walker that failed to
 * consume the prelude would surface those names instead of the real ones.
 *
 * The 'L' payload is written including its terminating NUL, which is how GNU
 * writes it; the others are written without.
 *
 *
 * @pre The fixture buffer is large enough for seven members plus the end blocks.
 * @pre No walk is in progress over the previous fixture contents.
 * @post `s_arc_len` gives the archive's total length.
 * @post The archive ends with the two zero blocks a reader stops on.
 *
 * @note Not thread-safe; the fixture buffer is file-scope state.
 *
 * @see internal_tb_add()  Appends one member.
 * @see internal_tb_end()  Writes the terminating blocks.

 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_tb_build_honest_archive(void)
{
  size_t off = 0U;
  off        = internal_tb_add(off, "dir/", nullptr, (uint8_t)'5', nullptr, 0U);
  off =
    internal_tb_add(off, "b.png", "vol1/ch1", (uint8_t)'0', s_honest_d1, sizeof(s_honest_d1) - 1U);
  /* GNU longname member: 'L' data carries the real name. */
  off =
    internal_tb_add(off, "ignored", nullptr, (uint8_t)'L', s_honest_lname, sizeof(s_honest_lname));
  off = internal_tb_add(off, "short", nullptr, (uint8_t)'0', s_honest_d2, sizeof(s_honest_d2) - 1U);
  /* pax member: path + size records override the header fields. */
  off = internal_tb_add(off,
                        "ignored2",
                        nullptr,
                        (uint8_t)'x',
                        s_honest_pax,
                        sizeof(s_honest_pax) - 1U);
  off =
    internal_tb_add(off, "hdrname", nullptr, (uint8_t)'0', s_honest_d3, sizeof(s_honest_d3) - 1U);
  /* A symlink member: enumerated, not a file. */
  off       = internal_tb_add(off, "link", nullptr, (uint8_t)'2', nullptr, 0U);
  off       = internal_tb_end(off);
  s_arc_len = off;
}

/**
 * @test internal_test_tar_walk_honest_archive
 * @brief A mixed honest archive enumerates and extracts byte-exactly.
 *
 * @par MC/DC:
 * Decision: `(type == k_ra8_tar_type_pax) || (type == k_ra8_tar_type_longname)`
 * in libs/ra8_unarch/src/ra8_unarch_tar.c@ra8_unarch_tar_next (2 conditions)
 * - Vector 1: pax member       -> true  (T,-)
 * - Vector 2: longname member  -> true  (F,T)
 * - Vector 3: regular file     -> false (F,F)
 * Vectors 1+3 prove the pax leg, 2+3 the longname leg: minimal MC/DC.

 *
 * @details Walks the canonical archive and validates each name, size, offset, type, and extracted payload.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_tar_walk_honest_archive(void)
{
  TEST_BEGIN("tar: honest archive walk + extract");
  internal_tb_build_honest_archive();

  ra8_unarch_tar_t t   = {};
  ra8_unarch_mem_t mem = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_tt_open(&t, &mem, nullptr));

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
  TEST_ASSERT(e.size == (uint64_t)(sizeof(s_honest_d1) - 1U));
  size_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_read(&t, &e, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(sizeof(s_honest_d1) - 1U, got);
  TEST_ASSERT_EQ(0, memcmp(s_out, s_honest_d1, got));
  /* 3: the longname member (its 'L' prelude was consumed transparently). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, e.next_off, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(1U, e.is_file);
  TEST_ASSERT_EQ(sizeof(s_honest_lname) - 1U, e.name_len);
  TEST_ASSERT_EQ(0, memcmp(s_name, s_honest_lname, e.name_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_read(&t, &e, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(0, memcmp(s_out, s_honest_d2, got));
  internal_tar_walk_tail(&t, &e, s_honest_d3);
  TEST_END("tar: honest archive walk + extract");
}

/**
 * @test internal_test_tar_open_probe_guards
 * @brief Probe / open argument validation and non-tar rejection.
 *
 * @par MC/DC:
 * (no compound decisions under test -- probe and open are chains of
 * independent single-condition guards.)

 *
 * @details Exercises TAR probe/open null, truncation, size, and invalid-limit guards without mutating caller outputs.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_tar_open_probe_guards(void)
{
  TEST_BEGIN("tar: probe + open guards");
  s_arc_len = (size_t)internal_tb_end(0U);
  TEST_ASSERT(!ra8_unarch_tar_probe(nullptr, 512U));
  TEST_ASSERT(!ra8_unarch_tar_probe(s_arc, 511U));
  TEST_ASSERT(!ra8_unarch_tar_probe(s_arc, 512U)); /* zero block: no magic */
  uint8_t good[k_tt_tar_block] = {};
  internal_tb_header(good, "x.png", nullptr, (uint8_t)'0', 0U);
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
 * @pre The shared archive buffer is available.
 * @post Every hostile pax shape was rejected and the marker-less EOF returned
 *       not_found cleanly.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0

 *
 * @details Constructs repeated and oversized pax metadata to prove per-member metadata budgets terminate the walk.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @post All writes remain within caller-owned fixture storage.
 */
RA8_INTERNAL
static void internal_tar_hostile_pax_floods(ra8_unarch_tar_t*       t,
                                            ra8_unarch_mem_t*       mem,
                                            ra8_unarch_tar_entry_t* e,
                                            const char*             data)
{
  const size_t dlen = strlen(data);

  /* Meta flood: more pax blocks than the per-member cap. */
  const char paxr[] = "11 size=5\n";
  size_t     off    = 0U;
  for (uint32_t i = 0U; i < k_t_pax_flood; ++i) {
    off = internal_tb_add(off, "x", nullptr, (uint8_t)'x', paxr, sizeof(paxr) - 1U);
  }
  off       = internal_tb_add(off, "a.png", nullptr, (uint8_t)'0', data, dlen);
  s_arc_len = internal_tb_end(off);
  TEST_ASSERT_EQ(k_ra8_ok, internal_tt_open(t, mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_unarch_tar_next(t, 0U, s_name, sizeof(s_name), e));

  /* Oversized pax data (over k_ra8_unarch_tar_pax_max). */
  uint8_t big[k_t_pax_oversize];
  memset(big, (int)'a', sizeof(big));
  off       = internal_tb_add(0U, "x", nullptr, (uint8_t)'x', big, sizeof(big));
  off       = internal_tb_add(off, "a.png", nullptr, (uint8_t)'0', data, dlen);
  s_arc_len = internal_tb_end(off);
  TEST_ASSERT_EQ(k_ra8_ok, internal_tt_open(t, mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_unarch_tar_next(t, 0U, s_name, sizeof(s_name), e));

  /* Malformed pax data inside a well-formed member. */
  const char paxbad[] = "99 path=x\n";
  off       = internal_tb_add(0U, "x", nullptr, (uint8_t)'x', paxbad, sizeof(paxbad) - 1U);
  off       = internal_tb_add(off, "a.png", nullptr, (uint8_t)'0', data, dlen);
  s_arc_len = internal_tb_end(off);
  TEST_ASSERT_EQ(k_ra8_ok, internal_tt_open(t, mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_unarch_tar_next(t, 0U, s_name, sizeof(s_name), e));

  /* EOF without an end marker: a clean not_found, not a crash. */
  s_arc_len = internal_tb_add(0U, "a.png", nullptr, (uint8_t)'0', data, dlen);
  TEST_ASSERT_EQ(k_ra8_ok, internal_tt_open(t, mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(t, 0U, s_name, sizeof(s_name), e));
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_unarch_tar_next(t, e->next_off, s_name, sizeof(s_name), e));
}

/**
 * @test internal_test_tar_hostile_headers
 * @brief Corrupt, lying, and flooding headers are rejected fail-closed.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each hostile shape drives one
 * single-condition rejection in the walker.)

 *
 * @details Mutates checksum, magic, size, type metadata, and archive extent to cover hostile header rejection.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_tar_hostile_headers(void)
{
  TEST_BEGIN("tar: hostile headers rejected");
  const char             data[] = "0123456789";
  ra8_unarch_tar_t       t      = {};
  ra8_unarch_mem_t       mem    = {};
  ra8_unarch_tar_entry_t e      = {};

  /* Corrupt checksum on the second member. */
  size_t       off = internal_tb_add(0U, "a.png", nullptr, (uint8_t)'0', data, sizeof(data) - 1U);
  const size_t second = off;
  off       = internal_tb_add(off, "b.png", nullptr, (uint8_t)'0', data, sizeof(data) - 1U);
  s_arc_len = internal_tb_end(off);
  s_arc[second] ^= k_t_corrupt_mask;
  TEST_ASSERT_EQ(k_ra8_ok, internal_tt_open(&t, &mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_unarch_tar_next(&t, e.next_off, s_name, sizeof(s_name), &e));

  /* Lying size: data area overruns the archive. */
  s_arc_len = internal_tb_add(0U, "a.png", nullptr, (uint8_t)'0', data, sizeof(data) - 1U);
  internal_tb_header(&s_arc[0], "a.png", nullptr, (uint8_t)'0', k_t_size_lie); /* size lies */
  TEST_ASSERT_EQ(k_ra8_ok, internal_tt_open(&t, &mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));

  /* Truncated data: the archive ends inside the member's data area. */
  uint8_t big_member[k_t_member_len];
  memset(big_member, (int)'m', sizeof(big_member));
  s_arc_len = internal_tb_add(0U, "a.png", nullptr, (uint8_t)'0', big_member, sizeof(big_member));
  s_arc_len -= k_t_truncate_by; /* cut into the 600-byte data area itself */
  TEST_ASSERT_EQ(k_ra8_ok, internal_tt_open(&t, &mem, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));

  internal_tar_hostile_pax_floods(&t, &mem, &e, data);

  TEST_END("tar: hostile headers rejected");
}

/**
 * @test internal_test_tar_policy_bounds
 * @brief Entry cap, iteration budget, and per-member output cap each fire.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each axis is one single-condition
 * budget charge in `ra8_decomp_limits.c`, driven here through the walker.)

 *
 * @details Isolates entry-count and iteration limits while preserving otherwise valid archive bytes.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_tar_policy_bounds(void)
{
  TEST_BEGIN("tar: policy bounds fire");
  const char data[] = "0123456789";
  size_t     off    = 0U;
  for (uint32_t i = 0U; i < 4U; ++i) {
    off = internal_tb_add(off, "p.png", nullptr, (uint8_t)'0', data, sizeof(data) - 1U);
  }
  s_arc_len = internal_tb_end(off);

  ra8_unarch_tar_t       t   = {};
  ra8_unarch_mem_t       mem = {};
  ra8_unarch_tar_entry_t e   = {};

  /* Entry cap: the third member crosses a 2-entry policy. */
  ra8_decomp_limits_t lim = ra8_decomp_limits_default();
  lim.max_entries         = 2U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_tt_open(&t, &mem, &lim));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, e.next_off, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(k_ra8_err_decomp_entries,
                 ra8_unarch_tar_next(&t, e.next_off, s_name, sizeof(s_name), &e));

  /* Iteration budget: every examined block charges one turn. */
  lim                = ra8_decomp_limits_default();
  lim.max_iterations = 2U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_tt_open(&t, &mem, &lim));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_unarch_tar_next(&t, e.next_off, s_name, sizeof(s_name), &e));
  TEST_ASSERT_EQ(k_ra8_err_decomp_iterations,
                 ra8_unarch_tar_next(&t, e.next_off, s_name, sizeof(s_name), &e));

  /* Per-member output cap: a member larger than the policy allows. */
  lim                  = ra8_decomp_limits_default();
  lim.max_output_bytes = 4U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_tt_open(&t, &mem, &lim));
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap,
                 ra8_unarch_tar_next(&t, 0U, s_name, sizeof(s_name), &e));
  TEST_END("tar: policy bounds fire");
}

/**
 * @test internal_test_tar_read_guards
 * @brief The extractor re-validates entries and buffers fail-closed.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the extractor is a chain of
 * independent single-condition guards, each driven both ways here.)

 *
 * @details Forges entry extents and hostile readers to prove extraction clamps reads and preserves destination bounds.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_tar_read_guards(void)
{
  TEST_BEGIN("tar: extractor guards");
  const char data[] = "0123456789";
  size_t     off    = internal_tb_add(0U, "dir/", nullptr, (uint8_t)'5', nullptr, 0U);
  off               = internal_tb_add(off, "a.png", nullptr, (uint8_t)'0', data, sizeof(data) - 1U);
  off               = internal_tb_add(off, "empty.png", nullptr, (uint8_t)'0', nullptr, 0U);
  s_arc_len         = internal_tb_end(off);

  ra8_unarch_tar_t t   = {};
  ra8_unarch_mem_t mem = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_tt_open(&t, &mem, nullptr));
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
 * @test internal_test_tar_base256_and_gnu_magic
 * @brief A base-256 size field and old-GNU magic walk end-to-end.
 *
 * @par MC/DC:
 * (no compound decisions under test -- integration vector for the
 * base-256 leg of `priv_unarch_tar_num` and the space-terminated magic.)

 *
 * @details Proves accepted positive base-256 sizes and old-GNU magic while rejecting signed or oversized encodings.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_tar_base256_and_gnu_magic(void)
{
  TEST_BEGIN("tar: base-256 size + old-GNU magic");
  const char data[] = "binary-size-member";
  size_t     off = internal_tb_add(0U, "b256.png", nullptr, (uint8_t)'0', data, sizeof(data) - 1U);
  /* Rewrite the size field as base-256 and re-checksum. */
  memset(&s_arc[k_tt_off_size], 0, k_tt_len_size);
  s_arc[k_tt_off_size]                      = k_t_b256_positive;
  s_arc[k_tt_off_size + k_tt_len_size - 1U] = (uint8_t)(sizeof(data) - 1U);
  /* Old-GNU magic variant: "ustar" + ' ' + ' '. */
  s_arc[k_tt_off_magic_nul]    = (uint8_t)' ';
  s_arc[k_tt_off_version]      = (uint8_t)' ';
  s_arc[k_tt_off_version + 1U] = 0U;
  internal_tb_finish(&s_arc[0]);
  s_arc_len = internal_tb_end(off);

  ra8_unarch_tar_t t   = {};
  ra8_unarch_mem_t mem = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_tt_open(&t, &mem, nullptr));
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
  internal_test_tar_num_fields();
  internal_test_tar_block_primitives();
  internal_test_tar_pax_records();
  internal_test_tar_walk_honest_archive();
  internal_test_tar_open_probe_guards();
  internal_test_tar_hostile_headers();
  internal_test_tar_policy_bounds();
  internal_test_tar_read_guards();
  internal_test_tar_base256_and_gnu_magic();
  return 0;
}
