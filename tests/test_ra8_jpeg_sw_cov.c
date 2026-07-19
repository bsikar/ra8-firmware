/**
 * @file test_ra8_jpeg_sw_cov.c
 * @brief Coverage supplement for ra8_jpeg_sw.c -- entropy-coder
 *        primitives and ra8_jpeg_sw_get_dimensions() error paths.
 *
 * @details
 * The main test_ra8_jpeg_sw.c exercises ra8_jpeg_sw.c via the full
 * encoder/decoder round-trip, which leaves several internal
 * primitives and guard-return arms uncovered because the pipeline
 * always provides well-formed inputs.  This file drives those paths
 * directly:
 *
 *   br_fill / ra8_jpeg_sw_br_get_bits:
 *     - Buffer exhausted before any byte is read (lines 104-105).
 *     - Buffer ends after a 0xFF marker-start byte (lines 111-112).
 *     - Refill branch taken but br_fill succeeds (lines 157-161).
 *     - Refill branch taken but br_fill cannot satisfy nbits < n
 *       (lines 157-159).
 *
 *   ra8_jpeg_sw_htab_decode:
 *     - Empty stream: br->nbits == 0 after br_fill (line 264).
 *     - valptr overrun: j >= h->total inside the match branch
 *       (line 276).
 *     - Stream underflow inside the greedy loop (line 282).
 *     - Table miss: loop exhausts all 16 code lengths (line 286).
 *     - (Line 270 is proven unreachable: see GCOVR_EXCL_LINE note
 *       in the source.)
 *
 *   ra8_jpeg_sw_get_dimensions:
 *     - jpeg_len < 4 (line 495).
 *     - Non-marker byte at the start of a segment (line 503).
 *     - Pad-byte run exhausts the buffer (line 511).
 *     - Marker read leaves fewer than 2 bytes for seglen (line 520).
 *     - SOF0 segment length field < 8 (line 528).
 *     - SOF0 precision byte != 8 (line 532).
 *
 * @par MC/DC:
 * Compound decision in br_fill while-loop condition:
 *   `br->nbits <= k_jpeg_reservoir_lo && !br->had_eoi` (2 conditions)
 * - V_TT: nbits=0, had_eoi=0 -> both T -> enter loop
 *         (all bit-reader tests start here)
 * - V_TF: nbits=0, had_eoi set mid-loop -> C1=T, C2=F -> exit loop
 *         (test_br_fill_empty_buffer: pos>=len sets had_eoi=1 inside
 *         the loop; the next condition check sees T,F)
 * - V_FX: nbits=32 > 24 -> C1=F -> exit immediately
 *         (test_br_get_bits_refill_and_succeed: after reading 4 bytes,
 *         nbits=32 and the loop exits without checking had_eoi)
 * V_TT + V_TF isolate C2; V_TT + V_FX isolate C1. N+1 = 3.
 *
 * All other decisions exercised in this file are single-condition;
 * each is covered by at least one true-branch and one false-branch
 * vector across the test suite (either here or in test_ra8_jpeg_sw.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_jpeg_sw.h"
#include "ra8_jpeg_sw_internal.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum jpeg_sw_cov_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_jpeg_huff_symbol =
    0x42U, /**< A Huffman table symbol; non-zero, so a table left cleared is distinguishable from one that was loaded. */
} jpeg_sw_cov_uint8_const_t;

/* ------------------------------------------------------------------ */
/* Bit-reader helpers: br_fill / ra8_jpeg_sw_br_get_bits */
/* ------------------------------------------------------------------ */

/**
 * @brief Verify br_fill sets had_eoi when the buffer is empty.
 *
 * @details
 * Calls ra8_jpeg_sw_br_get_bits() with a zero-length buffer and
 * nbits=0, which forces the first br_fill invocation to find
 * pos >= len immediately and set had_eoi = 1 (lines 104-105).
 * After br_fill returns with nbits still 0, the inner
 * `if (br->nbits < n)` is true (line 158) so the function
 * returns -1 (line 159).  Lines 104, 105, 157, 158, 159 covered.
 *
 * @par MC/DC:
 * Decision `if (br->pos >= br->len)` (line 103, single condition):
 * - V_T: pos=0, len=0 -> 0 >= 0 = true -> had_eoi=1 (this test).
 * - V_F: pos=0 < len=N -> false -> byte is read (tests 2 and 3).
 * N+1 = 2.
 * @since 0.1.0
 */
static void test_br_fill_empty_buffer(void)
{
  TEST_BEGIN("jpeg_sw_cov br_fill: empty buffer sets had_eoi");
  ra8_jpeg_bitreader_t br = {};
  int32_t              r  = ra8_jpeg_sw_br_get_bits(&br, 8U);
  TEST_ASSERT_EQ(-1, r);
  TEST_ASSERT_EQ(1, br.had_eoi);
  TEST_END("jpeg_sw_cov br_fill: empty buffer sets had_eoi");
}

/**
 * @brief Verify br_fill sets had_eoi when buffer ends after 0xFF.
 *
 * @details
 * A one-byte buffer containing 0xFF causes br_fill to consume the
 * byte as a potential marker-start, then find pos >= len before
 * reading the second byte, setting had_eoi = 1 (lines 111-112).
 * nbits stays 0, so ra8_jpeg_sw_br_get_bits returns -1.
 * Lines 111, 112, 157, 158, 159 covered.
 *
 * @par MC/DC:
 * Decision `if (br->pos >= br->len)` (line 110, single condition,
 * reached after consuming a 0xFF byte):
 * - V_T: single-byte buffer {0xFF}, pos becomes 1 == len=1 (this test).
 * - V_F: longer buffer where the byte after 0xFF is available
 *        (exercised by the full decode round-trip in test_ra8_jpeg_sw.c).
 * N+1 = 2.
 * @since 0.1.0
 */
static void test_br_fill_marker_at_end_of_stream(void)
{
  TEST_BEGIN("jpeg_sw_cov br_fill: 0xFF at end-of-stream sets had_eoi");
  static const uint8_t buf[] = {0xFFU};
  ra8_jpeg_bitreader_t br    = {};
  br.buf                     = buf;
  br.len                     = (uint32_t)sizeof buf;
  int32_t r                  = ra8_jpeg_sw_br_get_bits(&br, 1U);
  TEST_ASSERT_EQ(-1, r);
  TEST_ASSERT_EQ(1, br.had_eoi);
  TEST_END("jpeg_sw_cov br_fill: 0xFF at end-of-stream sets had_eoi");
}

/**
 * @brief Verify br_get_bits takes the refill path and succeeds.
 *
 * @details
 * Starts with nbits=0 and a 4-byte all-zero buffer.  The call to
 * ra8_jpeg_sw_br_get_bits(br, 4) finds nbits(0) < n(4), so the
 * outer `if` block is entered (line 156).  br_fill reads all four
 * bytes (32 bits), after which nbits(32) >= n(4).  The inner check
 * `if (br->nbits < n)` (line 158) is false, so execution falls
 * through to the closing brace (line 161) and the function extracts
 * four bits (all zero) and returns 0.
 * Lines 157, 158, 161 covered; line 159 intentionally NOT taken.
 *
 * @par MC/DC:
 * Decision `if (br->nbits < n)` at line 156 (outer) and line 158
 * (inner), same single condition:
 * - Outer V_T: nbits=0 < n=4 -> enter block (this test).
 * - Outer V_F: nbits >= n -> skip block (full decode path).
 * - Inner V_F: br_fill raised nbits to 32 >= 4 -> not taken (this).
 * - Inner V_T: br_fill cannot raise nbits -> return -1 (tests 1+2).
 * N+1 = 2 per condition.
 * @since 0.1.0
 */
static void test_br_get_bits_refill_and_succeed(void)
{
  TEST_BEGIN("jpeg_sw_cov br_get_bits: refill branch succeeds (line 161)");
  static const uint8_t buf[] = {0x00U, 0x00U, 0x00U, 0x00U};
  ra8_jpeg_bitreader_t br    = {};
  br.buf                     = buf;
  br.len                     = (uint32_t)sizeof buf;
  int32_t r                  = ra8_jpeg_sw_br_get_bits(&br, 4U);
  TEST_ASSERT_EQ(0, r);
  TEST_END("jpeg_sw_cov br_get_bits: refill branch succeeds (line 161)");
}

/* ------------------------------------------------------------------ */
/* ra8_jpeg_sw_htab_decode error paths */
/* ------------------------------------------------------------------ */

/**
 * @brief Verify htab_decode returns -1 when the stream is empty.
 *
 * @details
 * With a zero-length bit stream, br_fill at line 262 immediately
 * sets had_eoi=1 without adding bits, leaving nbits=0.  The check
 * at line 263 (`if (br->nbits == 0U)`) is therefore true and the
 * function returns -1 (line 264).
 *
 * @par MC/DC:
 * Decision `if (br->nbits == 0U)` (line 263, single condition):
 * - V_T: empty stream -> nbits=0 -> return -1 (this test).
 * - V_F: any non-empty stream -> nbits > 0 -> continue (tests 5-7).
 * N+1 = 2.
 * @since 0.1.0
 */
static void test_htab_decode_empty_stream(void)
{
  TEST_BEGIN("jpeg_sw_cov htab_decode: empty stream -> return -1 (line 264)");
  ra8_jpeg_htab_t      h  = {};
  ra8_jpeg_bitreader_t br = {};
  int32_t              r  = ra8_jpeg_sw_htab_decode(&br, &h);
  TEST_ASSERT_EQ(-1, r);
  TEST_END("jpeg_sw_cov htab_decode: empty stream -> return -1 (line 264)");
}

/**
 * @brief Verify htab_decode returns -1 when valptr + offset >= total.
 *
 * @details
 * Builds a minimal Huffman table with one 1-bit code (code = 0 ->
 * symbol 0x42), then corrupts h.total to 0.  With a stream of zeros,
 * htab_decode reads the first bit as code=0, finds code(0) <=
 * maxcode[0](0), computes j = valptr[0] + 0 = 0, and checks
 * j(0) >= h.total(0) -> true -> return -1 (line 276).
 *
 * @par MC/DC:
 * Decision `if (j >= h->total)` (line 275, single condition):
 * - V_T: j=0, total=0 -> 0 >= 0 = true -> return -1 (this test).
 * - V_F: j < total -> return symbol (full decode round-trip).
 * N+1 = 2.
 * @since 0.1.0
 */
static void test_htab_decode_valptr_overrun(void)
{
  TEST_BEGIN("jpeg_sw_cov htab_decode: j >= total -> return -1 (line 276)");
  ra8_jpeg_htab_t h = {};
  /* Build a 1-symbol, 1-bit table: bits[0]=1, vals[0]=0x42. */
  h.bits[0] = 1U;
  h.vals[0] = k_jpeg_huff_symbol;
  ra8_jpeg_sw_htab_build(&h);
  /* Corrupt total so that j = valptr[0] + 0 = 0 >= total = 0. */
  h.total = 0U;
  /* Stream: one zero byte.  First bit extracted as code=0. */
  static const uint8_t buf[] = {0x00U};
  ra8_jpeg_bitreader_t br    = {};
  br.buf                     = buf;
  br.len                     = (uint32_t)sizeof buf;
  int32_t r                  = ra8_jpeg_sw_htab_decode(&br, &h);
  TEST_ASSERT_EQ(-1, r);
  TEST_END("jpeg_sw_cov htab_decode: j >= total -> return -1 (line 276)");
}

/**
 * @brief Verify htab_decode returns -1 when stream underflows inside
 *        the greedy code-extension loop.
 *
 * @details
 * An all-empty Huffman table (all maxcode[i] = -1) forces the greedy
 * loop to request a new bit on every iteration.  A one-byte stream
 * (8 bits) provides the initial bit at line 268 (7 bits remain),
 * then 7 more bits are consumed at line 280 (i=0..6).  At i=7,
 * nbits=0 and br_get_bits calls br_fill which finds had_eoi=1 and
 * returns without adding bits; br_get_bits returns -1 (line 282).
 *
 * @par MC/DC:
 * Decision `if (bit < 0)` (line 281, single condition):
 * - V_T: stream exhausted mid-loop -> bit=-1 -> return -1 (this test).
 * - V_F: stream still has bits -> bit>=0 -> continue (tests 5, 7).
 * N+1 = 2.
 * @since 0.1.0
 */
static void test_htab_decode_underflow_in_loop(void)
{
  TEST_BEGIN("jpeg_sw_cov htab_decode: loop stream underflow (line 282)");
  /* Empty table: all bits[i]=0 so all maxcode[i]=-1.
   * ra8_jpeg_sw_htab_build leaves every maxcode entry at -1 when
   * the corresponding bits[i] is zero. */
  ra8_jpeg_htab_t h = {};
  ra8_jpeg_sw_htab_build(&h);
  /* One-byte stream: 8 bits total.  1 consumed at line 268, then
   * 7 more in the loop, exhausting the accumulator at iteration 7. */
  static const uint8_t buf[] = {0x80U};
  ra8_jpeg_bitreader_t br    = {};
  br.buf                     = buf;
  br.len                     = (uint32_t)sizeof buf;
  int32_t r                  = ra8_jpeg_sw_htab_decode(&br, &h);
  TEST_ASSERT_EQ(-1, r);
  TEST_END("jpeg_sw_cov htab_decode: loop stream underflow (line 282)");
}

/**
 * @brief Verify htab_decode returns -1 when all 16 code lengths miss.
 *
 * @details
 * An all-empty Huffman table (all maxcode[i] = -1) with a 3-byte
 * (24-bit) stream provides enough bits so the greedy loop completes
 * all 16 iterations without running out of bits.  After the loop
 * exits, the table-miss return at line 286 is reached.
 *
 * @par MC/DC:
 * Decision `if (code <= h->maxcode[i])` (line 273, single condition):
 * - V_F: maxcode[i]=-1 and code>=0 -> false -> extend code (this test
 *        and test_htab_decode_underflow_in_loop).
 * - V_T: code <= maxcode[i] -> enter match branch (test_htab_decode_
 *        valptr_overrun and the full decode round-trip).
 * N+1 = 2.
 * @since 0.1.0
 */
static void test_htab_decode_table_miss(void)
{
  TEST_BEGIN("jpeg_sw_cov htab_decode: table miss after 16 iters (line 286)");
  ra8_jpeg_htab_t h = {};
  ra8_jpeg_sw_htab_build(&h);
  /* Three zero bytes = 24 bits.  1 for line 268 + 16 in loop = 17
   * bits consumed; 7 spare bits prevent loop underflow. */
  static const uint8_t buf[] = {0x00U, 0x00U, 0x00U};
  ra8_jpeg_bitreader_t br    = {};
  br.buf                     = buf;
  br.len                     = (uint32_t)sizeof buf;
  int32_t r                  = ra8_jpeg_sw_htab_decode(&br, &h);
  TEST_ASSERT_EQ(-1, r);
  TEST_END("jpeg_sw_cov htab_decode: table miss after 16 iters (line 286)");
}

/* ------------------------------------------------------------------ */
/* ra8_jpeg_sw_get_dimensions guard returns */
/* ------------------------------------------------------------------ */

/**
 * @brief Verify get_dimensions rejects jpeg_len < 4.
 *
 * @details
 * With jpeg_len=3, the guard `if (jpeg_len < 4U)` at line 494 is
 * true and the function returns k_ra8_err_invalid_size (line 495).
 *
 * @par MC/DC:
 * Decision `if (jpeg_len < 4U)` (line 494, single condition):
 * - V_T: len=3 -> return invalid_size (this test).
 * - V_F: len>=4 -> continue (all other get_dimensions tests).
 * N+1 = 2.
 * @since 0.1.0
 */
static void test_get_dim_too_short(void)
{
  TEST_BEGIN("jpeg_sw_cov get_dimensions: jpeg_len<4 -> invalid_size (line 495)");
  static const uint8_t buf[] = {0xFFU, 0xD8U, 0xFFU};
  uint16_t             w     = 0U;
  uint16_t             h     = 0U;
  ra8_err_t            e     = ra8_jpeg_sw_get_dimensions(buf, 3U, &w, &h);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, e);
  TEST_END("jpeg_sw_cov get_dimensions: jpeg_len<4 -> invalid_size (line 495)");
}

/**
 * @brief Verify get_dimensions rejects a non-0xFF byte inside the loop.
 *
 * @details
 * After the SOI check, the parser enters the while loop and checks
 * `jpeg_buf[i] != 0xFF`.  A buffer with SOI followed by 0x00 at
 * position 2 triggers the return at line 503.
 *
 * @par MC/DC:
 * Decision `if (jpeg_buf[i] != 0xFF)` (line 502, single condition):
 * - V_T: byte=0x00 != 0xFF -> return protocol_error (this test).
 * - V_F: byte=0xFF -> continue pad-skip (all well-formed inputs).
 * N+1 = 2.
 * @since 0.1.0
 */
static void test_get_dim_non_marker_byte(void)
{
  TEST_BEGIN("jpeg_sw_cov get_dimensions: non-FF byte -> protocol_error (line 503)");
  /* SOI then a non-marker byte with enough length to enter the loop. */
  static const uint8_t buf[] = {0xFFU, 0xD8U, 0x00U, 0x00U, 0x00U, 0x00U};
  uint16_t             w     = 0U;
  uint16_t             h     = 0U;
  ra8_err_t            e     = ra8_jpeg_sw_get_dimensions(buf, (uint32_t)sizeof buf, &w, &h);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, e);
  TEST_END("jpeg_sw_cov get_dimensions: non-FF byte -> protocol_error (line 503)");
}

/**
 * @brief Verify get_dimensions returns protocol_error when a pad-byte
 *        run exhausts the buffer.
 *
 * @details
 * After SOI, a sequence of 0xFF bytes extends all the way to the end
 * of the buffer.  The pad-skip while loop increments i until
 * i == jpeg_len, then the guard `if (i >= jpeg_len)` at line 510 is
 * true and the function returns k_ra8_err_protocol_error (line 511).
 *
 * @par MC/DC:
 * Decision `if (i >= jpeg_len)` (line 510, single condition):
 * - V_T: pad run reaches EOF -> return protocol_error (this test).
 * - V_F: pad run stops before EOF -> continue parsing (all well-formed).
 * N+1 = 2.
 * @since 0.1.0
 */
static void test_get_dim_pad_run_to_eof(void)
{
  TEST_BEGIN("jpeg_sw_cov get_dimensions: pad run to EOF (line 511)");
  /* SOI (2 bytes) + 6 pad bytes of 0xFF.  Loop condition: 2+4=6<=8.
   * Pad-skip advances i from 2 to 8, equalling jpeg_len. */
  static const uint8_t buf[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xFFU,
    0xFFU,
    0xFFU,
    0xFFU,
    0xFFU,
  };
  uint16_t  w = 0U;
  uint16_t  h = 0U;
  ra8_err_t e = ra8_jpeg_sw_get_dimensions(buf, (uint32_t)sizeof buf, &w, &h);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, e);
  TEST_END("jpeg_sw_cov get_dimensions: pad run to EOF (line 511)");
}

/**
 * @brief Verify get_dimensions returns protocol_error when fewer than
 *        2 bytes remain after reading the marker byte.
 *
 * @details
 * The buffer contains SOI, two padding 0xFF bytes, and one marker
 * byte (0xC0 = SOF0).  After pad-skip lands at the 0xC0 byte, the
 * parser reads it (i becomes 6 = jpeg_len).  The check
 * `if (i + 2U > jpeg_len)` at line 519 evaluates 8 > 6, which is
 * true, so the function returns k_ra8_err_protocol_error (line 520).
 *
 * @par MC/DC:
 * Decision `if (i + 2U > jpeg_len)` (line 519, single condition):
 * - V_T: not enough bytes for seglen -> return protocol_error (this).
 * - V_F: at least 2 bytes remain -> read seglen (all other paths).
 * N+1 = 2.
 * @since 0.1.0
 */
static void test_get_dim_marker_then_truncated(void)
{
  TEST_BEGIN("jpeg_sw_cov get_dimensions: truncated after marker (line 520)");
  /* SOI + two 0xFF pad bytes + 0xC0 marker byte only (no seglen). */
  static const uint8_t buf[] = {0xFFU, 0xD8U, 0xFFU, 0xFFU, 0xFFU, 0xC0U};
  uint16_t             w     = 0U;
  uint16_t             h     = 0U;
  ra8_err_t            e     = ra8_jpeg_sw_get_dimensions(buf, (uint32_t)sizeof buf, &w, &h);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, e);
  TEST_END("jpeg_sw_cov get_dimensions: truncated after marker (line 520)");
}

/**
 * @brief Verify get_dimensions returns protocol_error when the SOF0
 *        segment length is less than 8.
 *
 * @details
 * An SOF0 marker with seglen=5 passes the `seglen < 2` check but
 * fails the `if (seglen < 8U)` check at line 527, returning
 * k_ra8_err_protocol_error (line 528).
 *
 * @par MC/DC:
 * Decision `if (seglen < 8U)` (line 527, single condition):
 * - V_T: seglen=5 < 8 -> return protocol_error (this test).
 * - V_F: seglen>=8 -> read precision (tests 13 and get_dimensions
 *        happy path).
 * N+1 = 2.
 * @since 0.1.0
 */
static void test_get_dim_sof0_seglen_too_small(void)
{
  TEST_BEGIN("jpeg_sw_cov get_dimensions: SOF0 seglen<8 -> protocol_error (line 528)");
  /* SOI + SOF0 marker + seglen=5 + 3 payload bytes. */
  static const uint8_t buf[] = {
    0xFFU,
    0xD8U, /* SOI */
    0xFFU,
    0xC0U, /* SOF0 marker */
    0x00U,
    0x05U, /* seglen = 5 (< 8) */
    0xAAU,
    0xBBU,
    0xCCU, /* 3 dummy payload bytes */
  };
  uint16_t  w = 0U;
  uint16_t  h = 0U;
  ra8_err_t e = ra8_jpeg_sw_get_dimensions(buf, (uint32_t)sizeof buf, &w, &h);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, e);
  TEST_END("jpeg_sw_cov get_dimensions: SOF0 seglen<8 -> protocol_error (line 528)");
}

/**
 * @brief Verify get_dimensions returns not_supported when SOF0 reports
 *        a precision other than 8.
 *
 * @details
 * An SOF0 marker with a valid seglen (11) but precision byte = 12
 * triggers `if (precision != 8U)` at line 531, returning
 * k_ra8_err_not_supported (line 532).
 *
 * @par MC/DC:
 * Decision `if (precision != 8U)` (line 531, single condition):
 * - V_T: precision=12 != 8 -> return not_supported (this test).
 * - V_F: precision=8 -> read dimensions (get_dimensions happy path).
 * N+1 = 2.
 * @since 0.1.0
 */
static void test_get_dim_sof0_bad_precision(void)
{
  TEST_BEGIN("jpeg_sw_cov get_dimensions: SOF0 precision!=8 -> not_supported (line 532)");
  /* SOI + SOF0 marker + seglen=11 + precision=12 (0x0C) + dims. */
  static const uint8_t buf[] = {
    0xFFU,
    0xD8U, /* SOI */
    0xFFU,
    0xC0U, /* SOF0 */
    0x00U,
    0x0BU, /* seglen=11 */
    0x0CU, /* prec=12   */
    0x00U,
    0x10U, /* height=16 */
    0x00U,
    0x10U, /* width=16 */
    0x01U,
    0x01U,
    0x11U,
    0x00U, /* 1 component */
  };
  uint16_t  w = 0U;
  uint16_t  h = 0U;
  ra8_err_t e = ra8_jpeg_sw_get_dimensions(buf, (uint32_t)sizeof buf, &w, &h);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, e);
  TEST_END("jpeg_sw_cov get_dimensions: SOF0 precision!=8 -> not_supported (line 532)");
}

/**
 * @brief Test entry point.
 *
 * @details
 * Runs all coverage-supplement tests for ra8_jpeg_sw.c.  The
 * ra8_sim_mmap_reset() call clears the RA8D2 MMIO backing store;
 * none of these tests touch MMIO, but the call matches the
 * project convention for test binaries.
 *
 * @return 0 on success; non-zero (via exit(1)) on any assertion failure.
 * @since 0.1.0
 */
int32_t main(void)
{
  ra8_sim_mmap_reset();
  test_br_fill_empty_buffer();
  test_br_fill_marker_at_end_of_stream();
  test_br_get_bits_refill_and_succeed();
  test_htab_decode_empty_stream();
  test_htab_decode_valptr_overrun();
  test_htab_decode_underflow_in_loop();
  test_htab_decode_table_miss();
  test_get_dim_too_short();
  test_get_dim_non_marker_byte();
  test_get_dim_pad_run_to_eof();
  test_get_dim_marker_then_truncated();
  test_get_dim_sof0_seglen_too_small();
  test_get_dim_sof0_bad_precision();
  (void)fprintf(stderr, "[OK ] test_ra8_jpeg_sw_cov.c\n");
  return 0;
}
