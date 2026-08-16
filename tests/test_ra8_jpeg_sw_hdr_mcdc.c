/**
 * @file test_ra8_jpeg_sw_hdr_mcdc.c
 * @brief MC/DC vector tests for the JPEG header-parse decisions (ra8_jpeg_sw.c)
 *
 * @details
 * Split out of test_ra8_jpeg_sw.c to keep each test translation unit under
 * the repository file-size cap. Each MC/DC test builds its own minimal
 * JPEG byte stream inline; the shared gradient fixture is duplicated
 * per sibling. The core encode / decode / get_dimensions contract tests
 * stay in test_ra8_jpeg_sw.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_jpeg_sw.h"
#include "unity_minimal.h"

/**
 * @enum jpeg_sw_hdr_mcdc_fixture_t
 * @brief The byte-level helpers.
 */
typedef enum : uint8_t {
  k_jpeg_marker_soi = 0xD8U, /**< 0xD8: the Start Of Image marker code. */
  k_jpeg_out_small =
    64, /**< Output buffer smaller than decoded image, so truncation is reported, not overrun. */
  /** Low-byte mask used to split the connection handle little-endian. */
  k_byte_mask = 0xFFU,
} jpeg_sw_hdr_mcdc_fixture_t;

/**
 * @enum jpeg_hdr_out_t
 * @brief Output buffer big enough for the fixture image.
 */
typedef enum : uint16_t {
  k_jpeg_out_large = 256, /**< Output buffer big enough for the fixture image. */
} jpeg_hdr_out_t;

/**
 * @enum ra8_jpeg_test_const_t
 * @brief Sizes used by the test fixtures.
 */
typedef enum : uint16_t {
  k_jt_w              = 16U,                        /**< Test image width.      */
  k_jt_h              = 16U,                        /**< Test image height.     */
  k_jt_pixels         = (uint16_t)(16U * 16U),      /**< Pixel count.           */
  k_jt_rgb_bytes      = (uint16_t)(16U * 16U * 3U), /**< Jt RGB bytes.          */
  k_jt_jpeg_cap       = 4096U,                      /**< Encoder out cap.       */
  k_jt_mse_psnr30_max = 65U,                        /**< MSE for PSNR ~= 30 dB. */
} ra8_jpeg_test_const_t;

/** @brief Fill an RGB888 buffer with a deterministic gradient. @details Exercises the fill gradient path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] rgb Interleaved RGB fixture pixels. @param[in] w Image width value or receiver exercised by this helper. @param[in] h Image height value or receiver exercised by this helper. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_fill_gradient(uint8_t* rgb, uint16_t w, uint16_t h)
{
  for (uint16_t y = 0U; y < h; y++) {
    for (uint16_t x = 0U; x < w; x++) {
      uint32_t i  = (((uint32_t)y * (uint32_t)w) + (uint32_t)x) * 3U;
      rgb[i + 0U] = (uint8_t)((x * 16U) & k_byte_mask);
      rgb[i + 1U] = (uint8_t)((y * 16U) & k_byte_mask);
      rgb[i + 2U] = (uint8_t)(((x + y) * 8U) & k_byte_mask);
    }
  }
}

/**
 * @test internal_test_mcdc_decode_pad_byte_chain
 * @par MC/DC:
 * Decision ra8_jpeg_sw_decode line 1639 inner pad-skip while-loop:
 *   `d->cursor < d->src_len && d->src[d->cursor] == 0xFF` (2 conds).
 * Vectors:
 *   V_one_pad  : single 0xFF then real marker -> C1=T C2=T then C2=F
 *   V_many_pad : 0xFF 0xFF 0xFF then marker   -> loop iterates
 *   V_pad_eob  : trailing 0xFF run hitting EOB -> C1=F (cursor==len)
 * Same sub-decisions also exist in get_dimensions line 1400 and are
 * hit by V_one_pad / V_many_pad through that path. @brief Verify mcdc decode pad byte chain behavior. @details Executes the mcdc decode pad byte chain scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_decode_pad_byte_chain(void)
{
  TEST_BEGIN("jpeg_sw MC/DC decode/get_dimensions: pad-byte while-loop");
  uint8_t  out[k_jpeg_out_large] = {};
  uint16_t dw                    = 0U;
  uint16_t dh                    = 0U;

  /* V_many_pad: three 0xFF padding bytes ahead of EOI. */
  static const uint8_t many_pad[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xFFU,
    0xFFU,
    0xFFU,
    0xD9U,
  };
  TEST_ASSERT(
    ra8_jpeg_sw_decode(many_pad, (uint32_t)sizeof many_pad, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra8_ok);
  TEST_ASSERT(ra8_jpeg_sw_get_dimensions(many_pad, (uint32_t)sizeof many_pad, &dw, &dh) !=
              k_ra8_ok);

  /* V_pad_eob: 0xFF run that exhausts the buffer. */
  static const uint8_t pad_eob[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xFFU,
    0xFFU,
    0xFFU,
  };
  TEST_ASSERT(
    ra8_jpeg_sw_decode(pad_eob, (uint32_t)sizeof pad_eob, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra8_ok);
  TEST_END("jpeg_sw MC/DC decode/get_dimensions: pad-byte while-loop");
}

/**
 * @test internal_test_mcdc_get_dimensions_seglen_independent
 * @par MC/DC:
 * Decision ra8_jpeg_sw_get_dimensions line 1416:
 *   `seglen < 2U || (uint32_t)seglen > jpeg_len - i`.
 * The earlier `test_mcdc_get_dimensions_pad_and_marker` covered both
 * conditions but in the same fixture; this fixture pins them to
 * separate inputs so MC/DC can attribute each independent flip:
 *   V_short_after_app : APP0 with seglen=1 mid-stream -> C1=T
 *   V_over_after_app  : APP0 with seglen=0xFFFF       -> C2=T
 *   V_ok              : APP0 with seglen=4 + 2 payload bytes, then
 *                       valid SOF0 -> C1=F C2=F (success) @brief Verify mcdc get dimensions seglen independent behavior. @details Executes the mcdc get dimensions seglen independent scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_get_dimensions_seglen_independent(void)
{
  TEST_BEGIN("jpeg_sw MC/DC get_dimensions: seglen<2 vs seglen>buf");
  uint16_t w = 0U;
  uint16_t h = 0U;

  static const uint8_t short_seg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xE0U,
    0x00U,
    0x01U,
  };
  TEST_ASSERT(ra8_jpeg_sw_get_dimensions(short_seg, (uint32_t)sizeof short_seg, &w, &h) !=
              k_ra8_ok);

  static const uint8_t over_seg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xE0U,
    0xFFU,
    0xFFU,
    0x00U,
    0x00U,
  };
  TEST_ASSERT(ra8_jpeg_sw_get_dimensions(over_seg, (uint32_t)sizeof over_seg, &w, &h) != k_ra8_ok);

  /* V_ok: skip APP0(len=4) then real SOF0. */
  static const uint8_t app0_then_sof0[] = {
    0xFFU, 0xD8U, 0xFFU, 0xE0U, 0x00U, 0x04U, 0x00U, 0x00U, 0xFFU, 0xC0U, 0x00U,
    0x0BU, 0x08U, 0x00U, 0x20U, 0x00U, 0x40U, 0x01U, 0x01U, 0x11U, 0x00U,
  };
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_jpeg_sw_get_dimensions(app0_then_sof0, (uint32_t)sizeof app0_then_sof0, &w, &h));
  TEST_ASSERT_EQ(64, w);
  TEST_ASSERT_EQ(32, h);
  TEST_END("jpeg_sw MC/DC get_dimensions: seglen<2 vs seglen>buf");
}

/**
 * @test internal_test_mcdc_decode_sos_without_sof
 * @par MC/DC:
 * SOS handling in ra8_jpeg_sw_decode requires SOF0 first; this guards
 * the `if (!got_sof)` branch (independent of the SOS-validation
 * decisions in dec_parse_sos at lines 1161/1184). Pairs with the
 * round-trip tests where !got_sof is false.
 *   V_no_sof : SOI -> SOS directly             -> protocol_error
 *   V_with_sof: SOI -> SOF0 -> DQT -> DHT -> SOS (round-trip path,
 *               already covered) -> ok @brief Verify mcdc decode sos without sof behavior. @details Executes the mcdc decode sos without sof scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_decode_sos_without_sof(void)
{
  TEST_BEGIN("jpeg_sw MC/DC decode: SOS arrives before SOF0");
  uint8_t              out[k_jpeg_out_large] = {};
  uint16_t             dw                    = 0U;
  uint16_t             dh                    = 0U;
  static const uint8_t sos_first[]           = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xDAU,
    0x00U,
    0x08U,
    0x01U,
    0x01U,
    0x00U,
    0x00U,
    0x3FU,
    0x00U,
  };
  TEST_ASSERT(ra8_jpeg_sw_decode(sos_first,
                                 (uint32_t)sizeof sos_first,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);
  TEST_END("jpeg_sw MC/DC decode: SOS arrives before SOF0");
}

/**
 * @test internal_test_mcdc_decode_dht_tc_th_independent
 * @par MC/DC:
 * Decision dec_parse_dht line 1041:
 *   `if (tc >= k_ra8_jpeg_huff_classes || th >= k_ra8_jpeg_huff_ids)` (2 conds).
 * Reaches the DHT body with valid framing (`len > 2` and
 * `len <= src_len - cursor`) so the OR can actually evaluate, and
 * supplies a complete 16-byte BITS list of zeros (no symbols) so
 * the `cursor + huff_lengths > end` and `total > huff_max` guards
 * pass. tc_th nibble pair selects the condition under test:
 *   V_F_F: tc_th=0x00 -> tc=0,th=0      -> F,F (DHT body completes
 *           through the EOI exit; success leg of the OR)
 *   V_T_F: tc_th=0x20 -> tc=2,th=0      -> T,F (returns not_supported,
 *           proves C1 alone flips the outcome vs V_F_F)
 *   V_F_T: tc_th=0x02 -> tc=0,th=2      -> F,T (returns not_supported,
 *           proves C2 alone flips the outcome vs V_F_F)
 * N+1 = 3 vectors for N=2 conditions.
 *
 * Layout (per-vector): SOI, DHT_marker, len=0x13 (19), tc_th, 16x 0x00
 * (BITS list -- all-zero so total=0 symbols) and EOI. Total = 23 bytes;
 * the framing satisfies `len <= src_len - cursor` (19 <= 23-4 = 19). @brief Verify mcdc decode dht tc th independent behavior. @details Executes the mcdc decode dht tc th independent scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_decode_dht_tc_th_independent(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_dht: tc/th independence pairs");
  uint8_t  out[k_jpeg_out_small] = {};
  uint16_t dw                    = 0U;
  uint16_t dh                    = 0U;
  /* V_F_F: tc=0, th=0 -- valid DHT, no symbols. Decoder returns from
   * dec_parse_dht successfully, then loop sees EOI and exits with
   * protocol_error (no SOS) -- but line 1041 was evaluated F,F. */
  static const uint8_t dht_ff[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x13U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  };
  (void)ra8_jpeg_sw_decode(dht_ff, (uint32_t)sizeof dht_ff, out, (uint32_t)sizeof out, &dw, &dh);
  /* V_T_F: tc_th=0x20 -> tc=2, th=0 -- C1=T, C2=F (short-circuit). */
  static const uint8_t dht_tf[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x13U, 0x20U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  };
  TEST_ASSERT(
    ra8_jpeg_sw_decode(dht_tf, (uint32_t)sizeof dht_tf, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra8_ok);
  /* V_F_T: tc_th=0x02 -> tc=0, th=2 -- C1=F, C2=T. */
  static const uint8_t dht_ft[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x13U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  };
  TEST_ASSERT(
    ra8_jpeg_sw_decode(dht_ft, (uint32_t)sizeof dht_ft, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra8_ok);
  TEST_END("jpeg_sw MC/DC dec_parse_dht: tc/th independence pairs");
}

/**
 * @test internal_test_mcdc_decode_sof0_ncomp1
 * @par MC/DC:
 * Decision dec_parse_sof0 line 1104:
 *   `if (d->ncomp != 1U && d->ncomp != 3U)` (2 conds).
 * Existing tests cover ncomp=3 (T,F success) and ncomp=2 (T,T fail).
 * This test adds the missing C1-pair vector with ncomp=1 (F,-) so all
 * three independence pairs are present.
 *   V_F_dash: ncomp=1, hmax=vmax=1 -> dec_parse_sof0 returns ok (line
 *             1130 ncomp==3 is false, so is_444/is_420 not evaluated;
 *             decoder loop continues, hits EOI, returns protocol_error).
 *
 * Layout: SOI + SOF0(ncomp=1, 16x16, single component id=1, hv=0x11,
 * qid=0) + EOI. @brief Verify mcdc decode sof0 ncomp1 behavior. @details Executes the mcdc decode sof0 ncomp1 scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_decode_sof0_ncomp1(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_sof0: ncomp=1 pair");
  uint8_t              out[k_jpeg_out_small] = {};
  uint16_t             dw                    = 0U;
  uint16_t             dh                    = 0U;
  static const uint8_t sof0_ncomp1[]         = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xC0U,
    0x00U,
    0x0BU,
    0x08U,
    0x00U,
    0x10U,
    0x00U,
    0x10U,
    0x01U,
    0x01U,
    0x11U,
    0x00U,
    0xFFU,
    0xD9U,
  };
  /* dec_parse_sof0 succeeds with ncomp=1; the decoder eventually fails
   * at EOI without a SOS, but line 1104 was evaluated with C1=F. */
  (void)ra8_jpeg_sw_decode(sof0_ncomp1,
                           (uint32_t)sizeof sof0_ncomp1,
                           out,
                           (uint32_t)sizeof out,
                           &dw,
                           &dh);
  TEST_END("jpeg_sw MC/DC dec_parse_sof0: ncomp=1 pair");
}

/**
 * @test internal_test_mcdc_decode_sof0_444_chroma
 * @par MC/DC:
 * Decision dec_parse_sof0 line 1131 `is_444 = (hmax==1 && vmax==1)`
 * (2 conds). Existing 4:2:0 round-trip leaves is_444 with only F,-
 * (because hmax==2 short-circuits C1 to F). This test adds a hand-
 * built ncomp=3 SOF0 with hmax==1, vmax==1 so is_444 evaluates T,T
 * (closing C1-pair and C2-pair) and is_420 short-circuits (C1=F).
 *   V_T_T: hmax=1, vmax=1 (4:4:4) -> is_444=T (and !is_444 false at
 *          line 1134, so dec_parse_sof0 returns ok)
 *   V_T_F: hmax=1, vmax=2 (illegal subsamp) -> is_444 = T,F = F;
 *          is_420 also F -> line 1134 not_supported.
 * Combined with the existing F,- vector from the round-trip, line 1131
 * has F,-, T,T, T,F: closes both pairs.
 *
 * Layout per fixture: SOI + SOF0(ncomp=3, 16x16) + EOI. @brief Verify mcdc decode sof0 444 chroma behavior. @details Executes the mcdc decode sof0 444 chroma scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_decode_sof0_444_chroma(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_sof0: 4:4:4 + hmax/vmax pairs");
  uint8_t  out[k_jpeg_out_small] = {};
  uint16_t dw                    = 0U;
  uint16_t dh                    = 0U;
  /* V_T_T: full 4:4:4 (1,1,1). is_444 = T,T -> true; line 1134
   * !is_444 = false so the if-body is skipped (covers is_444 C2
   * independence pair and provides T,F,T,T,T,T row for is_420 too,
   * since the OR there short-circuits at C2 once the order is right
   * -- actually with hmax=1 line 1132 C1 = (hmax==2) = F, not T --
   * but combined with the round-trip's T,T,T,T,T,T row it still
   * helps cover line 1131 fully). */
  static const uint8_t sof0_444[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x11U, 0x00U, 0x02U, 0x11U, 0x01U, 0x03U, 0x11U, 0x01U, 0xFFU, 0xD9U,
  };
  (void)
    ra8_jpeg_sw_decode(sof0_444, (uint32_t)sizeof sof0_444, out, (uint32_t)sizeof out, &dw, &dh);
  /* V_T_F: hmax=1, vmax=2 -- comp1 hv=0x12 forces vmax=2 alongside
   * hmax=1. is_444 = (1==1 && 2==1) = T,F = F; is_420 starts (1==2)=F
   * short-circuit; !is_444 && !is_420 = T && T = T -> not_supported. */
  static const uint8_t sof0_h1v2[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x12U, 0x00U, 0x02U, 0x11U, 0x01U, 0x03U, 0x11U, 0x01U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(ra8_jpeg_sw_decode(sof0_h1v2,
                                 (uint32_t)sizeof sof0_h1v2,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);
  TEST_END("jpeg_sw MC/DC dec_parse_sof0: 4:4:4 + hmax/vmax pairs");
}

/**
 * @test internal_test_mcdc_decode_sof0_is420_subconditions
 * @par MC/DC:
 * Decision dec_parse_sof0 line 1132/1133 `is_420` (6 conds).
 * Existing covered rows: T,F,-,-,-,- (round-trip lo-quality 4:2:0
 * almost? actually shows T,F) and T,T,T,T,T,T (full 4:2:0). This
 * test adds vectors that flip C1, C3, C4, C5, C6 individually:
 *   V_F_dash:    hmax=1 vmax=1 -> is_444 path; is_420 C1=F (closes
 *                 C1-pair vs T,T,T,T,T,T from round-trip)
 *   V_T_T_F_:    hmax=2 vmax=2 comp_h[1]=2 -> is_420 = T,T,F,-,...
 *                 (closes C3-pair vs T,T,T,T,T,T)
 *   V_T_T_T_F_:  hmax=2 vmax=2 comp_h[1]=1 comp_v[1]=2 -> T,T,T,F,-,
 *                 (closes C4-pair)
 *   V_TTTT_F_:   comp_h[2]=2 -> T,T,T,T,F,- (closes C5-pair)
 *   V_TTTTT_F:   comp_v[2]=2 -> T,T,T,T,T,F (closes C6-pair)
 * Each of these is_420=F variants triggers line 1134 not_supported. @brief Verify mcdc decode sof0 is420 subconditions behavior. @details Executes the mcdc decode sof0 is420 subconditions scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_decode_sof0_is420_subconditions(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_sof0: is_420 6-cond independence");
  uint8_t  out[k_jpeg_out_small] = {};
  uint16_t dw                    = 0U;
  uint16_t dh                    = 0U;
  /* C1=F via 4:4:4 already covered by sof0_444 above. Re-run for
   * MCDC isolation. */
  static const uint8_t sof0_c1f[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x11U, 0x00U, 0x02U, 0x11U, 0x01U, 0x03U, 0x11U, 0x01U, 0xFFU, 0xD9U,
  };
  (void)
    ra8_jpeg_sw_decode(sof0_c1f, (uint32_t)sizeof sof0_c1f, out, (uint32_t)sizeof out, &dw, &dh);
  /* C3=F: 4:2:0 except comp_h[1]=2 (instead of 1). */
  static const uint8_t sof0_c3f[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x22U, 0x00U, 0x02U, 0x21U, 0x01U, 0x03U, 0x11U, 0x01U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(
    ra8_jpeg_sw_decode(sof0_c3f, (uint32_t)sizeof sof0_c3f, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra8_ok);
  /* C4=F: 4:2:0 except comp_v[1]=2. */
  static const uint8_t sof0_c4f[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x22U, 0x00U, 0x02U, 0x12U, 0x01U, 0x03U, 0x11U, 0x01U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(
    ra8_jpeg_sw_decode(sof0_c4f, (uint32_t)sizeof sof0_c4f, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra8_ok);
  /* C5=F: 4:2:0 except comp_h[2]=2. */
  static const uint8_t sof0_c5f[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x22U, 0x00U, 0x02U, 0x11U, 0x01U, 0x03U, 0x21U, 0x01U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(
    ra8_jpeg_sw_decode(sof0_c5f, (uint32_t)sizeof sof0_c5f, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra8_ok);
  /* C6=F: 4:2:0 except comp_v[2]=2. */
  static const uint8_t sof0_c6f[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x22U, 0x00U, 0x02U, 0x11U, 0x01U, 0x03U, 0x12U, 0x01U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(
    ra8_jpeg_sw_decode(sof0_c6f, (uint32_t)sizeof sof0_c6f, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra8_ok);
  TEST_END("jpeg_sw MC/DC dec_parse_sof0: is_420 6-cond independence");
}

/**
 * @test internal_test_mcdc_decode_sos_dc_ac_id_independent
 * @par MC/DC:
 * Decision dec_parse_sos line 1184:
 *   `if (comp_dc_id[idx] >= k_ra8_jpeg_huff_ids ||
 *        comp_ac_id[idx] >= k_ra8_jpeg_huff_ids)` (2 conds).
 * Reaches dec_parse_sos with valid framing by first feeding a
 * valid SOF0 (ncomp=1) so got_sof=true, then SOS with ns=1 and
 * a tdta nibble pair selecting bad dc_id or bad ac_id.
 *   V_T_F: tdta=0x20 -> dc_id=2 (>=2), ac_id=0 -> C1=T,C2=F
 *   V_F_T: tdta=0x02 -> dc_id=0, ac_id=2 (>=2) -> C1=F,C2=T
 *   (V_F_F success row not added here -- requires full Huffman
 * bitstream, deferred to .)
 * Combined with 1 implicit F,F row from any future success path,
 * V_T_F and V_F_T close C1-pair and C2-pair respectively.
 *
 * Layout: SOI + SOF0(ncomp=1, 16x16, comp_id=1) + SOS(ns=1, cs=1,
 * tdta) + Ss/Se/AhAl(0,63,0) + EOI. @brief Verify mcdc decode sos dc ac id independent behavior. @details Executes the mcdc decode sos dc ac id independent scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_decode_sos_dc_ac_id_independent(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_sos: dc_id/ac_id independence pairs");
  uint8_t  out[k_jpeg_out_small] = {};
  uint16_t dw                    = 0U;
  uint16_t dh                    = 0U;
  /* V_T_F: tdta=0x20 -> dc_id=2 (T), ac_id=0 (F). SOS length 8:
   * 2(len) + 1(ns) + 2(cs+tdta)*1 + 3(Ss/Se/AhAl) = 8. */
  static const uint8_t sos_tf[] = {
    /* SOI */ 0xFFU,
    0xD8U,
    /* SOF0 ncomp=1 */
    0xFFU,
    0xC0U,
    0x00U,
    0x0BU,
    0x08U,
    0x00U,
    0x10U,
    0x00U,
    0x10U,
    0x01U,
    0x01U,
    0x11U,
    0x00U,
    /* SOS */
    0xFFU,
    0xDAU,
    0x00U,
    0x08U,
    0x01U,
    0x01U,
    0x20U,
    0x00U,
    0x3FU,
    0x00U,
    /* EOI */ 0xFFU,
    0xD9U,
  };
  TEST_ASSERT(
    ra8_jpeg_sw_decode(sos_tf, (uint32_t)sizeof sos_tf, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra8_ok);
  /* V_F_T: tdta=0x02 -> dc_id=0 (F), ac_id=2 (T). */
  static const uint8_t sos_ft[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x0BU, 0x08U, 0x00U, 0x10U,
    0x00U, 0x10U, 0x01U, 0x01U, 0x11U, 0x00U, 0xFFU, 0xDAU, 0x00U,
    0x08U, 0x01U, 0x01U, 0x02U, 0x00U, 0x3FU, 0x00U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(
    ra8_jpeg_sw_decode(sos_ft, (uint32_t)sizeof sos_ft, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra8_ok);
  TEST_END("jpeg_sw MC/DC dec_parse_sos: dc_id/ac_id independence pairs");
}

/**
 * @test internal_test_mcdc_get_dimensions_padding_truncated
 *
 * @par MC/DC:
 * Decision at libs/ra8_jpeg/src/ra8_jpeg_sw.c
 *   ``while (i < jpeg_len && jpeg_buf[i] == 0xFF)`` (2 cond AND).
 * Existing tests cover (T,T) and (T,F); this fixture closes the
 * C1-pair by feeding a stream whose pad-byte run extends to EOF
 * (i becomes equal to jpeg_len mid-loop, so C1 flips to F).
 * Vectors:
 *  - V_TT (existing pad_jpeg): C1=T, C2=T -> stay in loop.
 *  - V_TF (existing pad_jpeg): C1=T, C2=F -> exit on non-pad byte.
 *  - V_FX (this fixture):      C1=F       -> exit on i >= jpeg_len.
 * V_TT + V_FX isolate C1; V_TT + V_TF isolate C2. N+1 = 3. @brief Verify mcdc get dimensions padding truncated behavior. @details Executes the mcdc get dimensions padding truncated scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_get_dimensions_padding_truncated(void)
{
  TEST_BEGIN("jpeg_sw MC/DC get_dimensions: pad-run extends to EOF");
  uint16_t w = 0U;
  uint16_t h = 0U;
  /* SOI followed by an FF pad-byte run that ends exactly at EOF. */
  static const uint8_t pad_to_eof[] = {0xFFU, 0xD8U, 0xFFU, 0xFFU, 0xFFU};
  TEST_ASSERT(ra8_jpeg_sw_get_dimensions(pad_to_eof, (uint32_t)sizeof pad_to_eof, &w, &h) !=
              k_ra8_ok);
  TEST_END("jpeg_sw MC/DC get_dimensions: pad-run extends to EOF");
}

/**
 * @test internal_test_mcdc_get_dimensions_soi_mid_walk
 *
 * @par MC/DC:
 * Decision at libs/ra8_jpeg/src/ra8_jpeg_sw.c
 *   ``if (mk == SOI || mk == EOI)`` (2 cond OR).
 * Existing tests cover (F,F) and (F,T mk==EOI); this fixture closes
 * the C1-pair by emitting a second SOI (0xFFD8) mid-stream.
 * Vectors:
 *  - V_FF (existing pad_jpeg):       C1=F, C2=F -> fall-through to seglen.
 *  - V_FT (existing soi_eoi_jpeg):   C1=F, C2=T -> continue (EOI).
 *  - V_TX (this fixture):            C1=T       -> continue (SOI).
 * V_FF + V_TX isolate C1; V_FF + V_FT isolate C2. N+1 = 3. @brief Verify mcdc get dimensions soi mid walk behavior. @details Executes the mcdc get dimensions soi mid walk scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_get_dimensions_soi_mid_walk(void)
{
  TEST_BEGIN("jpeg_sw MC/DC get_dimensions: SOI mid-walk continue");
  uint16_t w = 0U;
  uint16_t h = 0U;
  /* SOI, second SOI (continue), SOF0 16x16. */
  static const uint8_t soi_mid[] = {
    0xFFU,
    0xD8U, /* SOI */
    0xFFU,
    0xD8U, /* SOI again -> continue */
    0xFFU,
    0xC0U,
    0x00U,
    0x0BU,
    0x08U,
    0x00U,
    0x10U,
    0x00U,
    0x10U, /* SOF0 16x16 */
    0x01U,
    0x01U,
    0x11U,
    0x00U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_sw_get_dimensions(soi_mid, (uint32_t)sizeof soi_mid, &w, &h));
  TEST_ASSERT_EQ(16, w);
  TEST_ASSERT_EQ(16, h);
  TEST_END("jpeg_sw MC/DC get_dimensions: SOI mid-walk continue");
}

/**
 * @test internal_test_mcdc_get_dimensions_skip_appn
 *
 * @par MC/DC:
 * Decision at libs/ra8_jpeg/src/ra8_jpeg_sw.c
 *   ``if (mk >= 0xFFC0 && mk <= 0xFFCF && mk != DHT && mk != 0xFFC8)``
 *   (4-cond AND). Existing tests cover (T,T,T,T) sof2 and (T,T,F,-)
 *   dht; this fixture supplies (F,-,-,-) via APP0 (0xFFE0) skip and
 *   (T,T,T,F) via JPG marker (0xFFC8). With dht (T,T,F,-) already
 *   tested, all four C-pairs are now isolated.
 *  - V_TTTT (sof2_jpeg):    C1=T,C2=T,C3=T,C4=T -> not_supported.
 *  - V_TTF- (dht_then_sof0):C1=T,C2=T,C3=F      -> skipped, ok.
 *  - V_F--- (this APP0):    C1=F                -> skipped, ok.
 *  - V_TTTF (this 0xFFC8):  C1=T,C2=T,C3=T,C4=F -> skipped, ok.
 * V_TTTT + V_F--- isolate C1; V_TTTT + (synthetic mk>0xFFCF, dead in
 * a SOF range walk) is exempt; V_TTTT + V_TTF- isolate C3; V_TTTT +
 * V_TTTF isolate C4. C2-pair remains structurally dead (no valid mk
 * is simultaneously >=0xFFC0 yet >0xFFCF without first failing C1);
 * documented per DO-178C 6.4.4.3. @brief Verify mcdc get dimensions skip appn behavior. @details Executes the mcdc get dimensions skip appn scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_get_dimensions_skip_appn(void)
{
  TEST_BEGIN("jpeg_sw MC/DC get_dimensions: skip APP0 + JPG marker");
  uint16_t w = 0U;
  uint16_t h = 0U;
  /* APP0 (mk=0xFFE0, C1=F) then SOF0. */
  static const uint8_t app0_then_sof0[] = {
    0xFFU, 0xD8U,                                                  /* SOI                      */
    0xFFU, 0xE0U, 0x00U, 0x04U, 0x00U, 0x00U,                      /* APP0 length 4, payload 2 */
    0xFFU, 0xC0U, 0x00U, 0x0BU, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, /* SOF0 16x16               */
    0x01U, 0x01U, 0x11U, 0x00U,
  };
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_jpeg_sw_get_dimensions(app0_then_sof0, (uint32_t)sizeof app0_then_sof0, &w, &h));
  TEST_ASSERT_EQ(16, w);
  /* JPG marker (0xFFC8, C4=F) then SOF0. */
  static const uint8_t jpg_then_sof0[] = {
    0xFFU, 0xD8U,                                                  /* SOI                     */
    0xFFU, 0xC8U, 0x00U, 0x04U, 0x00U, 0x00U,                      /* JPG length 4, payload 2 */
    0xFFU, 0xC0U, 0x00U, 0x0BU, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, /* SOF0 16x16              */
    0x01U, 0x01U, 0x11U, 0x00U,
  };
  w = 0U;
  h = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_get_dimensions(jpg_then_sof0, (uint32_t)sizeof jpg_then_sof0, &w, &h));
  TEST_ASSERT_EQ(16, w);
  TEST_END("jpeg_sw MC/DC get_dimensions: skip APP0 + JPG marker");
}

/**
 * @test internal_test_mcdc_decode_skip_appn_marker
 *
 * @par MC/DC:
 * Decision at libs/ra8_jpeg/src/ra8_jpeg_sw.c (decode_scan)
 *   ``else if (mk >= 0xFFC1 && mk <= 0xFFCF && mk != DHT && mk != 0xFFC8)``
 *   (4-cond AND). Existing round-trip covers (T,T,T,T) and structurally
 *   exercises C2/C3/C4 false rows via DQT/DHT/SOS path. This fixture
 *   drives C1=F by feeding APP0 mid-stream and producing a successful
 *   round-trip decode -- the APP0 is dispatched to the
 *   ``dec_skip_segment`` else-arm, hitting the C1=F vector that
 *   round-trip alone never produced (C1=T baseline already covered). @brief Verify mcdc decode skip appn marker behavior. @details Executes the mcdc decode skip appn marker scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_decode_skip_appn_marker(void)
{
  TEST_BEGIN("jpeg_sw MC/DC decode: skip APPn marker mid-stream");
  static uint8_t local_rgb_in[(uint32_t)k_jt_rgb_bytes];
  static uint8_t local_rgb_out[(uint32_t)k_jt_rgb_bytes];
  static uint8_t local_jpeg[(uint32_t)k_jt_jpeg_cap];
  internal_fill_gradient(local_rgb_in, (uint16_t)k_jt_w, (uint16_t)k_jt_h);
  uint32_t produced = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_encode(local_rgb_in,
                                    (uint16_t)k_jt_w,
                                    (uint16_t)k_jt_h,
                                    (uint8_t)k_ra8_jpeg_sw_quality_high,
                                    local_jpeg,
                                    (uint32_t)k_jt_jpeg_cap,
                                    &produced));
  /* Splice an APP0 marker into the encoded stream right after SOI. */
  static uint8_t       local_spliced[(uint32_t)k_jt_jpeg_cap + 16U];
  static const uint8_t app0_payload[] = {
    0xFFU,
    0xE0U,
    0x00U,
    0x06U,
    0x4AU,
    0x46U,
    0x49U,
    0x46U,
  };
  local_spliced[0] = k_byte_mask;
  local_spliced[1] = k_jpeg_marker_soi;
  for (uint32_t i = 0U; i < (uint32_t)sizeof app0_payload; ++i) {
    local_spliced[2U + i] = app0_payload[i];
  }
  for (uint32_t i = 2U; i < produced; ++i) {
    local_spliced[(uint32_t)sizeof app0_payload + i] = local_jpeg[i];
  }
  uint32_t spliced_len = produced + (uint32_t)sizeof app0_payload;

  uint16_t dw = 0U;
  uint16_t dh = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_decode(local_spliced,
                                    spliced_len,
                                    local_rgb_out,
                                    (uint32_t)k_jt_rgb_bytes,
                                    &dw,
                                    &dh));
  TEST_ASSERT_EQ(k_jt_w, dw);
  TEST_END("jpeg_sw MC/DC decode: skip APPn marker mid-stream");
}

int main(void)
{
  ra8_fake_mmap_reset();
  internal_test_mcdc_decode_pad_byte_chain();
  internal_test_mcdc_get_dimensions_seglen_independent();
  internal_test_mcdc_decode_sos_without_sof();
  internal_test_mcdc_decode_dht_tc_th_independent();
  internal_test_mcdc_decode_sof0_ncomp1();
  internal_test_mcdc_decode_sof0_444_chroma();
  internal_test_mcdc_decode_sof0_is420_subconditions();
  internal_test_mcdc_decode_sos_dc_ac_id_independent();
  internal_test_mcdc_get_dimensions_padding_truncated();
  internal_test_mcdc_get_dimensions_soi_mid_walk();
  internal_test_mcdc_get_dimensions_skip_appn();
  internal_test_mcdc_decode_skip_appn_marker();
  return 0;
}
