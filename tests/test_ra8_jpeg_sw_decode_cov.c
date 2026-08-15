/**
 * @file test_ra8_jpeg_sw_decode_cov.c
 * @brief Black-box line-coverage supplement for ra8_jpeg_sw_decode.c.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The round-trip tests in test_ra8_jpeg_sw.c drive the decoder with
 * well-formed 3-component streams produced by the encoder, which
 * leaves the marker-parser guard returns, the entropy-scan error
 * legs, and the grayscale / image-edge branches of
 * ra8_jpeg_sw_decode.c uncovered.  This file drives those paths
 * directly through the public ra8_jpeg_sw_decode() entry point using
 * hand-built JPEG byte streams.  Because it calls the real
 * (un-renamed) production symbol linked from ra8_core_hal, gcovr
 * merges the covered lines into the aggregate .gcda for
 * ra8_jpeg_sw_decode.c (a renamed white-box #include copy would not
 * union into that object).
 *
 * The entropy fixtures rely on the canonical-Huffman builder in
 * ra8_jpeg_sw.c: a BITS list with a single length-1 code assigns
 * code "0" to the first symbol, and a two-entry length-1 list
 * assigns "0" and "1" to the first two symbols.  A DC symbol value
 * of 0 means category 0 (zero magnitude bits), and an AC symbol of
 * 0x00 is EOB, 0xF0 is ZRL, and 0xF1 is run-15 / size-1.  The
 * decoder context is memset to zero on entry, so any Huffman table
 * that was never populated by a DHT segment has maxcode[i] == 0 and
 * total == 0, which makes priv_jpeg_sw_htab_decode() return -1 the
 * first time it is consulted (undefined-table fault injection).
 *
 * Spec citations are tagged `T.81 sec X.Y "..."` and refer to
 * ITU-T Recommendation T.81 (1992) | ISO/IEC 10918-1.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_jpeg_sw.h"
#include "support/ra8_jpeg_sw_decode_cov_fixture.h"
#include "unity_minimal.h"

/* ------------------------------------------------------------------ */
/* Top-level input guards */
/* ------------------------------------------------------------------ */

/**
 * @brief Drive the three top-level guard returns of ra8_jpeg_sw_decode().
 *
 * @details
 * Covers the `jpeg_len < 4` size guard, the leading-marker check in
 * dec_dispatch_marker() (`src[cursor] != 0xFF`), and the truncated
 * variable-length skip in dec_skip_segment() (`cursor + 2 > src_len`).
 *
 * @par MC/DC:
 * - Decision `if (jpeg_len < 4U)` (single condition):
 *   V_T: jpeg_len=3 -> invalid_size (this test).  V_F: len>=4 -> all
 *   other fixtures continue past the guard.
 * - Decision `if (d->src[d->cursor] != 0xFF)` (single condition):
 *   V_T: byte 0x00 after SOI -> protocol_error (this test).  V_F:
 *   every FF-led marker in the other fixtures.
 * - Decision `if (d->cursor + 2U > d->src_len)` in dec_skip_segment
 *   (single condition): V_T: APP1 marker with no seglen bytes ->
 *   protocol_error (this test).  V_F: the padded-APP0 skip in
 *   test_ra8_jpeg_sw.c.
 * Each guard here is single-condition, so N+1 = 2 per decision with
 * the complementary vector supplied by the sibling round-trip tests.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_decode_top_level_guards(void)
{
  TEST_BEGIN("jpeg_dec_cov: len<4 / non-FF marker / skip-segment truncation");

  /* jpeg_len < 4 -> invalid_size. */
  static const uint8_t too_short[] = {0xFFU, 0xD8U, 0xFFU};
  uint16_t             w           = 0U;
  uint16_t             h           = 0U;
  ra8_err_t            e           = ra8_jpeg_sw_decode(too_short,
                                                        (uint32_t)sizeof too_short,
                                                        s_out,
                                                        (uint32_t)k_cov_out_big,
                                                        &w,
                                                        &h);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, e);

  /* SOI then a non-marker byte -> dec_dispatch_marker leading check. */
  static const uint8_t non_marker[] = {0xFFU, 0xD8U, 0x00U, 0x00U, 0x00U, 0x00U};
  e                                 = ra8_jpeg_sw_decode(non_marker,
                                                         (uint32_t)sizeof non_marker,
                                                         s_out,
                                                         (uint32_t)k_cov_out_big,
                                                         &w,
                                                         &h);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, e);

  /* APP1 (0xFFE1) marker with no length bytes -> dec_skip_segment. */
  static const uint8_t skip_trunc[] = {0xFFU, 0xD8U, 0xFFU, 0xE1U};
  e                                 = ra8_jpeg_sw_decode(skip_trunc,
                                                         (uint32_t)sizeof skip_trunc,
                                                         s_out,
                                                         (uint32_t)k_cov_out_big,
                                                         &w,
                                                         &h);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, e);

  TEST_END("jpeg_dec_cov: len<4 / non-FF marker / skip-segment truncation");
}

/* ------------------------------------------------------------------ */
/* DQT parser guards */
/* ------------------------------------------------------------------ */

/**
 * @brief Drive the two guard returns of dec_parse_dqt().
 *
 * @details
 * The first fixture ends immediately after the DQT marker so the
 * `cursor + 2 > src_len` header guard fires.  The second declares a
 * seglen of 3 (just enough for one Pq/Tq byte) so the
 * `cursor + block_size > end` table-body guard fires before the 64
 * quantization entries.
 *
 * @par MC/DC:
 * - Header decision `if (d->cursor + 2U > d->src_len)` (single cond):
 *   V_T this test; V_F every well-framed DQT.
 * - Body decision `if (d->cursor + block_size > end)` (single cond):
 *   V_T this test (seglen=3); V_F a full 65-byte DQT.
 * Single-condition guards: N+1 = 2 with the sibling round-trip DQT.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_decode_dqt_guards(void)
{
  TEST_BEGIN("jpeg_dec_cov: dec_parse_dqt header + table-body guards");
  uint16_t w = 0U;
  uint16_t h = 0U;

  static const uint8_t dqt_hdr[] = {0xFFU, 0xD8U, 0xFFU, 0xDBU};
  TEST_ASSERT_EQ(
    k_ra8_err_protocol_error,
    ra8_jpeg_sw_decode(dqt_hdr, (uint32_t)sizeof dqt_hdr, s_out, (uint32_t)k_cov_out_big, &w, &h));

  static const uint8_t dqt_body[] = {0xFFU, 0xD8U, 0xFFU, 0xDBU, 0x00U, 0x03U, 0x00U};
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_jpeg_sw_decode(dqt_body,
                                    (uint32_t)sizeof dqt_body,
                                    s_out,
                                    (uint32_t)k_cov_out_big,
                                    &w,
                                    &h));

  TEST_END("jpeg_dec_cov: dec_parse_dqt header + table-body guards");
}

/* ------------------------------------------------------------------ */
/* DHT parser guards */
/* ------------------------------------------------------------------ */

/**
 * @brief Drive the four guard returns of dec_parse_dht().
 *
 * @details
 * Fixtures target, in order: the `cursor + 2 > src_len` header guard;
 * the `cursor + huff_lengths > end` BITS-list guard (seglen=3); the
 * `total > huff_max` symbol-count guard (a BITS list of sixteen 0xFF
 * counts, sum 4080); and the `cursor + total > end` VALS-overrun
 * guard (BITS declares 10 symbols but the segment carries none).
 *
 * @par MC/DC:
 * Every DHT guard here is single-condition; each fixture supplies the
 * true vector and the sibling round-trip DHT supplies the false
 * vector (N+1 = 2 per decision).  The compound tc/th classifier and
 * seglen decisions already have full MC/DC vectors in
 * test_ra8_jpeg_sw.c.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_decode_dht_guards(void)
{
  TEST_BEGIN("jpeg_dec_cov: dec_parse_dht header/BITS/total/VALS guards");
  uint16_t w = 0U;
  uint16_t h = 0U;

  static const uint8_t dht_hdr[] = {0xFFU, 0xD8U, 0xFFU, 0xC4U};
  TEST_ASSERT_EQ(
    k_ra8_err_protocol_error,
    ra8_jpeg_sw_decode(dht_hdr, (uint32_t)sizeof dht_hdr, s_out, (uint32_t)k_cov_out_big, &w, &h));

  static const uint8_t dht_bits[] = {0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x03U, 0x00U};
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_jpeg_sw_decode(dht_bits,
                                    (uint32_t)sizeof dht_bits,
                                    s_out,
                                    (uint32_t)k_cov_out_big,
                                    &w,
                                    &h));

  static const uint8_t dht_total[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x13U, 0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
  };
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_jpeg_sw_decode(dht_total,
                                    (uint32_t)sizeof dht_total,
                                    s_out,
                                    (uint32_t)k_cov_out_big,
                                    &w,
                                    &h));

  static const uint8_t dht_vals[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x13U, 0x00U, 0x0AU, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  };
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_jpeg_sw_decode(dht_vals,
                                    (uint32_t)sizeof dht_vals,
                                    s_out,
                                    (uint32_t)k_cov_out_big,
                                    &w,
                                    &h));

  TEST_END("jpeg_dec_cov: dec_parse_dht header/BITS/total/VALS guards");
}

/* ------------------------------------------------------------------ */
/* SOF0 parser guards */
/* ------------------------------------------------------------------ */

/**
 * @brief Decode a malformed SOF0 fixture and assert the expected rejection.
 * @param[in] d    Encoded JPEG bytes.
 * @param[in] n    Length of @p d in bytes.
 * @param[in] want Expected `ra8_jpeg_sw_decode` result code.
 * @pre @p d is non-null.
 * @post `ra8_jpeg_sw_decode` returned @p want.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0 @details Exercises the jd expect path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. */
RA8_INTERNAL static void internal_jd_expect(const uint8_t* d, uint32_t n, ra8_err_t want)
{
  uint16_t w = 0U;
  uint16_t h = 0U;
  TEST_ASSERT_EQ(want, ra8_jpeg_sw_decode(d, n, s_out, (uint32_t)k_cov_out_big, &w, &h));
}

/**
 * @brief Drive four guard returns of dec_parse_sof0().
 *
 * @details
 * Fixtures target: the `cursor + 2 > src_len` header guard; the
 * `len < 8` minimum-length guard (seglen=5); the `precision != 8`
 * not-supported guard (precision byte 0x07); and the
 * `ncomp*3 + (s - cursor) > len` component-fit guard (seglen=8 leaves
 * no room for the single component descriptor).
 *
 * @par MC/DC:
 * Each targeted guard is single-condition on the leg this test flips;
 * the complementary vectors (well-framed SOF0, precision 8, fitting
 * component list) are supplied by the round-trip and dimension tests.
 * The 4:4:4 / 4:2:0 disambiguation compound decisions retain their
 * dedicated MC/DC coverage in test_ra8_jpeg_sw.c.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_decode_sof0_guards(void)
{
  TEST_BEGIN("jpeg_dec_cov: dec_parse_sof0 header/len/precision/comp guards");

  static const uint8_t sof0_hdr[] = {0xFFU, 0xD8U, 0xFFU, 0xC0U};
  internal_jd_expect(sof0_hdr, (uint32_t)sizeof sof0_hdr, k_ra8_err_protocol_error);

  static const uint8_t sof0_len[] = {0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x05U};
  internal_jd_expect(sof0_len, (uint32_t)sizeof sof0_len, k_ra8_err_protocol_error);

  static const uint8_t sof0_prec[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xC0U,
    0x00U,
    0x08U,
    0x07U,
    0x00U,
    0x10U,
    0x00U,
    0x10U,
    0x01U,
  };
  internal_jd_expect(sof0_prec, (uint32_t)sizeof sof0_prec, k_ra8_err_not_supported);

  static const uint8_t sof0_comp[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xC0U,
    0x00U,
    0x08U,
    0x08U,
    0x00U,
    0x10U,
    0x00U,
    0x10U,
    0x01U,
  };
  internal_jd_expect(sof0_comp, (uint32_t)sizeof sof0_comp, k_ra8_err_protocol_error);

  TEST_END("jpeg_dec_cov: dec_parse_sof0 header/len/precision/comp guards");
}

/* ------------------------------------------------------------------ */
/* SOS parser guards */
/* ------------------------------------------------------------------ */

/**
 * @brief Drive three guard returns of dec_parse_sos().
 *
 * @details
 * Each fixture supplies a valid grayscale SOF0 (so got_sof is true and
 * the SOS reaches its own validation) followed by: an SOS marker with
 * no seglen bytes (`cursor + 2 > src_len`); an SOS with seglen=5
 * (`len < 6`); and an SOS declaring ns=2 against a 1-component frame
 * (`ns != d->ncomp`).
 *
 * @par MC/DC:
 * The header and seglen guards are single-condition here; the ns/ncomp
 * mismatch is single-condition.  The dc_id/ac_id compound selector
 * decision keeps its independence pairs in test_ra8_jpeg_sw.c; this
 * test only supplies the leading not-supported / protocol-error legs.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_decode_sos_guards(void)
{
  TEST_BEGIN("jpeg_dec_cov: dec_parse_sos header/len/ns guards");
  static uint8_t local_buf[k_jpeg_buf_small];
  uint16_t       w = 0U;
  uint16_t       h = 0U;

  /* SOS marker with no seglen bytes. */
  uint32_t n = 0U;
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_soi, (uint32_t)sizeof s_soi);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sof0_8x8_gray,
                      (uint32_t)sizeof s_sof0_8x8_gray);
  static const uint8_t sos_marker[] = {0xFFU, 0xDAU};
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      sos_marker,
                      (uint32_t)sizeof sos_marker);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_jpeg_sw_decode(local_buf, n, s_out, (uint32_t)k_cov_out_big, &w, &h));

  /* SOS with seglen=5 (< 6). */
  n = 0U;
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_soi, (uint32_t)sizeof s_soi);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sof0_8x8_gray,
                      (uint32_t)sizeof s_sof0_8x8_gray);
  static const uint8_t sos_short[] = {0xFFU, 0xDAU, 0x00U, 0x05U};
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      sos_short,
                      (uint32_t)sizeof sos_short);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_jpeg_sw_decode(local_buf, n, s_out, (uint32_t)k_cov_out_big, &w, &h));

  /* SOS with ns=2 against a 1-component frame. */
  n = 0U;
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_soi, (uint32_t)sizeof s_soi);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sof0_8x8_gray,
                      (uint32_t)sizeof s_sof0_8x8_gray);
  static const uint8_t sos_ns2[] = {0xFFU, 0xDAU, 0x00U, 0x06U, 0x02U, 0x01U, 0x00U, 0x00U};
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, sos_ns2, (uint32_t)sizeof sos_ns2);
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_jpeg_sw_decode(local_buf, n, s_out, (uint32_t)k_cov_out_big, &w, &h));

  TEST_END("jpeg_dec_cov: dec_parse_sos header/len/ns guards");
}

/* ------------------------------------------------------------------ */
/* Entropy-scan error legs */
/* ------------------------------------------------------------------ */

/**
 * @brief Cover the output-size guard of dec_decode_scan().
 *
 * @details
 * A valid grayscale SOF0 + SOS reaches dec_decode_scan(), which then
 * rejects an undersized output buffer (`out_buf_len < width*height*3`)
 * before any entropy decode.  8x8 grayscale needs 192 bytes; a 10-byte
 * buffer trips the guard.
 *
 * @par MC/DC:
 * Decision `if (out_buf_len < need)` (single condition):
 * - V_T: out_buf_len=10 < 192 -> invalid_size (this test).
 * - V_F: the successful grayscale decode below and every round-trip.
 * N+1 = 2.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_decode_scan_buffer_too_small(void)
{
  TEST_BEGIN("jpeg_dec_cov: dec_decode_scan output-size guard");
  static uint8_t local_buf[k_jpeg_buf_small];
  static uint8_t local_tiny[(uint32_t)k_cov_out_small];
  uint16_t       w = 0U;
  uint16_t       h = 0U;

  uint32_t n = 0U;
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_soi, (uint32_t)sizeof s_soi);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sof0_8x8_gray,
                      (uint32_t)sizeof s_sof0_8x8_gray);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sos_gray,
                      (uint32_t)sizeof s_sos_gray);
  static const uint8_t entropy[] = {0x00U, 0x00U};
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, entropy, (uint32_t)sizeof entropy);
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_eoi, (uint32_t)sizeof s_eoi);

  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_jpeg_sw_decode(local_buf, n, local_tiny, (uint32_t)k_cov_out_small, &w, &h));
  TEST_END("jpeg_dec_cov: dec_decode_scan output-size guard");
}

/**
 * @brief Cover the DC-symbol failure leg of dec_block() and its
 *        propagation through the luma-block and scan loops.
 *
 * @details
 * A grayscale frame with a valid SOF0 + SOS but no DHT leaves the DC
 * Huffman table zero-initialized (total=0), so the first
 * priv_jpeg_sw_htab_decode() returns -1, tripping `if (t < 0)` in
 * dec_block().  The error propagates through dec_decode_mcu_y_blocks()
 * and dec_decode_scan().
 *
 * @par MC/DC:
 * Decision `if (t < 0)` in dec_block() (single condition):
 * - V_T: undefined DC table -> t=-1 -> protocol_error (this test).
 * - V_F: the successful grayscale decode below (t >= 0).
 * The `if (e != k_ra8_ok)` propagation checks in the y-block and scan
 * loops are single-condition; the false vector is the success case.
 * N+1 = 2 per decision.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_decode_dc_huffman_failure(void)
{
  TEST_BEGIN("jpeg_dec_cov: dec_block DC failure -> y-block + scan propagation");
  static uint8_t local_buf[k_jpeg_buf_small];
  uint16_t       w = 0U;
  uint16_t       h = 0U;

  uint32_t n = 0U;
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_soi, (uint32_t)sizeof s_soi);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sof0_8x8_gray,
                      (uint32_t)sizeof s_sof0_8x8_gray);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sos_gray,
                      (uint32_t)sizeof s_sos_gray);
  static const uint8_t entropy[] = {0x00U, 0x00U};
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, entropy, (uint32_t)sizeof entropy);
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_eoi, (uint32_t)sizeof s_eoi);

  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_jpeg_sw_decode(local_buf, n, s_out, (uint32_t)k_cov_out_big, &w, &h));
  TEST_END("jpeg_dec_cov: dec_block DC failure -> y-block + scan propagation");
}

/**
 * @brief Cover the DC-magnitude underflow leg of dec_block().
 *
 * @details
 * A DC table whose only code is a 4-bit "0000" mapping to symbol 0x08
 * decodes category t=8, consuming 4 of the 8 available entropy bits.
 * The following priv_jpeg_sw_br_get_bits(br, 8) then underflows (only 4
 * bits remain and had_eoi is set by the trailing EOI marker),
 * returning -1 with t != 0 so the `if (r < 0 && t != 0)` guard fires.
 *
 * @par MC/DC:
 * Decision `if (r < 0 && t != 0)` (2 conditions).  The production
 * source marks this decision MC/DC-deactivated (a defensive
 * fault-injection guard unreachable on well-formed JFIF streams); this
 * fixture supplies the reachable T,T vector for line coverage:
 * - V_TT: r=-1 (magnitude underflow) and t=8 -> protocol_error.
 * The F rows are the ordinary DC path exercised by every round-trip
 * (r >= 0), consistent with the deactivation rationale in the source.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_decode_dc_magnitude_underflow(void)
{
  TEST_BEGIN("jpeg_dec_cov: dec_block DC magnitude underflow (r<0 && t!=0)");
  static uint8_t local_buf[k_jpeg_buf_small];
  uint16_t       w = 0U;
  uint16_t       h = 0U;

  uint32_t n = 0U;
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_soi, (uint32_t)sizeof s_soi);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sof0_8x8_gray,
                      (uint32_t)sizeof s_sof0_8x8_gray);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_dht_dc_t8,
                      (uint32_t)sizeof s_dht_dc_t8);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sos_gray,
                      (uint32_t)sizeof s_sos_gray);
  static const uint8_t entropy[] = {0x00U};
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, entropy, (uint32_t)sizeof entropy);
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_eoi, (uint32_t)sizeof s_eoi);

  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_jpeg_sw_decode(local_buf, n, s_out, (uint32_t)k_cov_out_big, &w, &h));
  TEST_END("jpeg_dec_cov: dec_block DC magnitude underflow (r<0 && t!=0)");
}

/**
 * @brief Cover the AC-symbol failure leg of dec_block().
 *
 * @details
 * A grayscale frame with a valid DC DHT but no AC DHT decodes the DC
 * coefficient (symbol 0, t=0) and then consults the zero-initialized
 * AC table, whose priv_jpeg_sw_htab_decode() returns -1, tripping
 * `if (rs < 0)` in the AC loop.
 *
 * @par MC/DC:
 * Decision `if (rs < 0)` in the AC loop (single condition):
 * - V_T: undefined AC table -> rs=-1 -> protocol_error (this test).
 * - V_F: the successful grayscale decode (rs >= 0, EOB symbol).
 * N+1 = 2.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_decode_ac_huffman_failure(void)
{
  TEST_BEGIN("jpeg_dec_cov: dec_block AC Huffman failure (rs<0)");
  static uint8_t local_buf[k_jpeg_buf_small];
  uint16_t       w = 0U;
  uint16_t       h = 0U;

  uint32_t n = 0U;
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_soi, (uint32_t)sizeof s_soi);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sof0_8x8_gray,
                      (uint32_t)sizeof s_sof0_8x8_gray);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_dht_dc,
                      (uint32_t)sizeof s_dht_dc);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sos_gray,
                      (uint32_t)sizeof s_sos_gray);
  static const uint8_t entropy[] = {0x00U, 0x00U};
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, entropy, (uint32_t)sizeof entropy);
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_eoi, (uint32_t)sizeof s_eoi);

  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_jpeg_sw_decode(local_buf, n, s_out, (uint32_t)k_cov_out_big, &w, &h));
  TEST_END("jpeg_dec_cov: dec_block AC Huffman failure (rs<0)");
}

/**
 * @brief Cover the AC-magnitude underflow leg of dec_block().
 *
 * @details
 * The DC coefficient decodes (symbol 0), then an AC table whose only
 * code maps to symbol 0x0F (run 0, size 15) is decoded.  With only 6
 * bits left in the accumulator and had_eoi set, the subsequent
 * priv_jpeg_sw_br_get_bits(br, 15) underflows, tripping `if (v < 0)`.
 *
 * @par MC/DC:
 * Decision `if (v < 0)` in the AC loop (single condition):
 * - V_T: size-15 magnitude read underflows -> v=-1 -> protocol_error
 *   (this test).
 * - V_F: every well-formed AC coefficient read in the round-trip.
 * N+1 = 2.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_decode_ac_magnitude_underflow(void)
{
  TEST_BEGIN("jpeg_dec_cov: dec_block AC magnitude underflow (v<0)");
  static uint8_t local_buf[k_jpeg_buf_large];
  uint16_t       w = 0U;
  uint16_t       h = 0U;

  uint32_t n = 0U;
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_soi, (uint32_t)sizeof s_soi);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sof0_8x8_gray,
                      (uint32_t)sizeof s_sof0_8x8_gray);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_dht_dc,
                      (uint32_t)sizeof s_dht_dc);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_dht_ac_f0,
                      (uint32_t)sizeof s_dht_ac_f0);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sos_gray,
                      (uint32_t)sizeof s_sos_gray);
  static const uint8_t entropy[] = {0x00U};
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, entropy, (uint32_t)sizeof entropy);
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_eoi, (uint32_t)sizeof s_eoi);

  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_jpeg_sw_decode(local_buf, n, s_out, (uint32_t)k_cov_out_big, &w, &h));
  TEST_END("jpeg_dec_cov: dec_block AC magnitude underflow (v<0)");
}

/**
 * @brief Cover the ZRL run and the AC index-overrun leg of dec_block().
 *
 * @details
 * An AC table with two 1-bit codes -- symbol 0xF0 (ZRL) and symbol
 * 0xF1 (run 15, size 1) -- decodes three consecutive ZRLs, advancing
 * the coefficient index k from 1 to 49 (exercising the ZRL `k += 16`
 * and `continue`), then a 0xF1 symbol adds run 15 so k reaches 64,
 * tripping `if (k >= block_size)`.  The entropy byte 0x08 supplies the
 * bit sequence 0 (DC) 0 0 0 (three ZRLs) 1 (0xF1).
 *
 * @par MC/DC:
 * - ZRL classifier `if (rrrr == k_jpeg_nibble_mask)` (single cond):
 *   V_T: symbol 0xF0 -> ZRL skip (this test).  V_F: the EOB symbol
 *   0x00 in the successful decode.
 * - Index-overrun `if (k >= block_size)` (single cond): V_T: k=64 via
 *   ZRL runs plus a run-15 coefficient (this test).  V_F: the in-range
 *   AC coefficients of the round-trip.  N+1 = 2 per decision.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_decode_ac_zrl_index_overrun(void)
{
  TEST_BEGIN("jpeg_dec_cov: dec_block ZRL run + AC index overrun (k>=64)");
  static uint8_t local_buf[k_jpeg_buf_mid];
  uint16_t       w = 0U;
  uint16_t       h = 0U;

  uint32_t n = 0U;
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_soi, (uint32_t)sizeof s_soi);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sof0_8x8_gray,
                      (uint32_t)sizeof s_sof0_8x8_gray);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_dht_dc,
                      (uint32_t)sizeof s_dht_dc);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_dht_ac_zrl,
                      (uint32_t)sizeof s_dht_ac_zrl);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sos_gray,
                      (uint32_t)sizeof s_sos_gray);
  static const uint8_t entropy[] = {0x08U};
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, entropy, (uint32_t)sizeof entropy);
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_eoi, (uint32_t)sizeof s_eoi);

  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_jpeg_sw_decode(local_buf, n, s_out, (uint32_t)k_cov_out_big, &w, &h));
  TEST_END("jpeg_dec_cov: dec_block ZRL run + AC index overrun (k>=64)");
}

/**
 * @brief Assemble and reject one 4:4:4 stream with a failing chroma table.
 * @details Combines valid SOF0, luma DC/AC tables, and the supplied SOS
 * selector so the decoder reaches either the Cb or Cr propagation guard.
 * @param[in] sos Chroma-table-selecting SOS fragment.
 * @param[in] sos_len Readable size of @p sos in bytes.
 * @pre @p sos is non-null and names three scan components.
 * @pre @p sos_len is the complete SOS fragment length.
 * @post The assembled stream has returned ::k_ra8_err_protocol_error.
 * @post Mutations remain confined to the file-local source and output buffers.
 * @note Single-threaded injected-stream coverage helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_chroma_failure(const uint8_t* sos, uint32_t sos_len)
{
  static uint8_t       local_buf[k_jpeg_buf_large];
  static const uint8_t entropy[] = {0x00U, 0x00U};
  uint16_t             w         = 0U;
  uint16_t             h         = 0U;
  uint32_t             n         = 0U;

  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_soi, (uint32_t)sizeof s_soi);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sof0_444,
                      (uint32_t)sizeof s_sof0_444);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_dht_dc,
                      (uint32_t)sizeof s_dht_dc);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_dht_ac,
                      (uint32_t)sizeof s_dht_ac);
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, sos, sos_len);
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, entropy, (uint32_t)sizeof entropy);
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_eoi, (uint32_t)sizeof s_eoi);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_jpeg_sw_decode(local_buf, n, s_out, (uint32_t)k_cov_out_big, &w, &h));
}

/**
 * @brief Cover both chroma-block failure legs of
 *        dec_decode_mcu_chroma_blocks().
 *
 * @details
 * Two 4:4:4 fixtures share a valid luma DC+AC table.  The first points
 * both chroma components at the undefined Huffman table 1 so the Cb
 * block fails immediately (`if (e != k_ra8_ok)` after dec_block(ci=1)),
 * which also exercises the chroma-propagation return in
 * dec_decode_scan().  The second gives Cb the valid table 0 (so it
 * decodes) but leaves Cr pointing at the undefined table 1, so the
 * second `if (e != k_ra8_ok)` after dec_block(ci=2) fires.
 *
 * @par MC/DC:
 * Both propagation decisions are single-condition:
 * - Cb `if (e != k_ra8_ok)`: V_T this test's first fixture; V_F the
 *   round-trip's successful Cb block.
 * - Cr `if (e != k_ra8_ok)`: V_T this test's second fixture; V_F the
 *   round-trip's successful Cr block.
 * - dec_decode_scan chroma `if (e != k_ra8_ok)`: V_T (either fixture);
 *   V_F the round-trip.  N+1 = 2 per decision.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_decode_chroma_block_failures(void)
{
  TEST_BEGIN("jpeg_dec_cov: chroma Cb + Cr block failures + scan propagation");
  internal_assert_chroma_failure(s_sos_444_cb_fail, (uint32_t)sizeof s_sos_444_cb_fail);
  internal_assert_chroma_failure(s_sos_444_cr_fail, (uint32_t)sizeof s_sos_444_cr_fail);
  TEST_END("jpeg_dec_cov: chroma Cb + Cr block failures + scan propagation");
}
/**
 * @brief Cover the grayscale-emit and image-edge crop branches of
 *        dec_emit_mcu_rgb() via a successful 5x5 decode.
 *
 * @details
 * A fully valid 5x5 grayscale JPEG (DC symbol 0, AC EOB) decodes one
 * 8x8 MCU whose visible region is only 5x5.  The pixel-emit loop takes
 * the `py >= d->height` and `px >= d->width` edge-crop breaks and the
 * grayscale `else` branch that sets Cb = Cr = level offset (128).  This
 * is the sole successful-decode fixture in this file, providing the
 * false vectors for the scan/dec_block error decisions above.
 *
 * @par MC/DC:
 * - `if (py >= d->height)` (single cond): V_T rows 5..7 of the MCU;
 *   V_F rows 0..4.
 * - `if (px >= d->width)` (single cond): V_T cols 5..7; V_F cols 0..4.
 * - `if (d->ncomp == 3U)` (single cond): V_F grayscale (this test);
 *   V_T the 3-component round-trip.  N+1 = 2 per decision.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_decode_grayscale_edge_success(void)
{
  TEST_BEGIN("jpeg_dec_cov: 5x5 grayscale decode -- edge crop + gray emit");
  static uint8_t local_buf[k_jpeg_buf_mid];
  uint16_t       w = 0U;
  uint16_t       h = 0U;

  uint32_t n = 0U;
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_soi, (uint32_t)sizeof s_soi);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sof0_5x5_gray,
                      (uint32_t)sizeof s_sof0_5x5_gray);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_dht_dc,
                      (uint32_t)sizeof s_dht_dc);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_dht_ac,
                      (uint32_t)sizeof s_dht_ac);
  internal_cov_append(local_buf,
                      (uint32_t)sizeof local_buf,
                      &n,
                      s_sos_gray,
                      (uint32_t)sizeof s_sos_gray);
  static const uint8_t entropy[] = {0x00U, 0x00U};
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, entropy, (uint32_t)sizeof entropy);
  internal_cov_append(local_buf, (uint32_t)sizeof local_buf, &n, s_eoi, (uint32_t)sizeof s_eoi);

  ra8_err_t e = ra8_jpeg_sw_decode(local_buf, n, s_out, (uint32_t)k_cov_out_big, &w, &h);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  TEST_ASSERT_EQ(5, w);
  TEST_ASSERT_EQ(5, h);
  TEST_END("jpeg_dec_cov: 5x5 grayscale decode -- edge crop + gray emit");
}

/**
 * @brief Test entry point.
 *
 * @details
 * Runs every ra8_jpeg_sw_decode.c coverage fixture.  ra8_fake_mmap_reset()
 * clears the fake MMIO backing store per project convention; none
 * of these tests touch MMIO (the software codec is pure computation).
 *
 * @return 0 on success; the harness exits non-zero on any assertion
 *         failure.
 * @since 0.1.0
 */
int32_t main(void)
{
  ra8_fake_mmap_reset();
  internal_test_decode_top_level_guards();
  internal_test_decode_dqt_guards();
  internal_test_decode_dht_guards();
  internal_test_decode_sof0_guards();
  internal_test_decode_sos_guards();
  internal_test_decode_scan_buffer_too_small();
  internal_test_decode_dc_huffman_failure();
  internal_test_decode_dc_magnitude_underflow();
  internal_test_decode_ac_huffman_failure();
  internal_test_decode_ac_magnitude_underflow();
  internal_test_decode_ac_zrl_index_overrun();
  internal_test_decode_chroma_block_failures();
  internal_test_decode_grayscale_edge_success();
  return 0;
}
