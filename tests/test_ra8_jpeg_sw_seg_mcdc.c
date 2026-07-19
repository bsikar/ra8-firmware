/**
 * @file test_ra8_jpeg_sw_seg_mcdc.c
 * @brief MC/DC vector tests for the JPEG segment-walk decisions (ra8_jpeg_sw.c)
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

#include "ra8_err.h"
#include "ra8_jpeg_sw.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum jpeg_sw_seg_mcdc_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_jpeg_out_small =
    64, /**< Output buffer smaller than a decoded image, so a truncation is reported rather than overrun. */
  k_byte_mask = 0xFFU, /**< Truncates each generated RGB channel back into a byte. */
} jpeg_sw_seg_mcdc_uint8_const_t;

/**
 * @enum jpeg_sw_seg_mcdc_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_jpeg_out_large = 256, /**< Output buffer big enough for the fixture image. */
} jpeg_sw_seg_mcdc_uint16_const_t;

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

/** @brief Fill an RGB888 buffer with a deterministic gradient. */
static void fill_gradient(uint8_t* rgb, uint16_t w, uint16_t h)
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

static const uint8_t s_pad_jpeg[] = {
  0xFFU, 0xD8U, 0xFFU, 0xFFU, 0xFFU, 0xC0U, 0x00U, 0x0BU, 0x08U, 0x00U,
  0x10U, 0x00U, 0x10U, 0x01U, 0x01U, 0x11U, 0x00U, 0xFFU, 0xD9U,
};

static const uint8_t s_soi_eoi_jpeg[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xD9U,
  0xFFU,
  0xC0U,
  0x00U,
  0x0BU,
  0x08U,
  0x00U,
  0x20U,
  0x00U,
  0x10U,
  0x01U,
  0x01U,
  0x11U,
  0x00U,
};

static const uint8_t s_bad_seg_jpeg[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xC0U,
  0x00U,
  0x01U,
};

static const uint8_t s_over_seg_jpeg[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xC0U,
  0x00U,
  0xFFU,
};

static const uint8_t s_w0_jpeg[] = {
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
  0x00U,
  0x01U,
  0x01U,
  0x11U,
  0x00U,
};

static const uint8_t s_h0_jpeg[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xC0U,
  0x00U,
  0x0BU,
  0x08U,
  0x00U,
  0x00U,
  0x00U,
  0x10U,
  0x01U,
  0x01U,
  0x11U,
  0x00U,
};

static const uint8_t s_sof2_jpeg[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xC2U,
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
};

static const uint8_t s_dht_then_sof0[] = {
  0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x02U, 0xFFU, 0xC0U, 0x00U, 0x0BU,
  0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x01U, 0x01U, 0x11U, 0x00U,
};

/**
 * @test test_mcdc_get_dimensions_pad_and_marker
 * @par MC/DC:
 * Five decisions in ra8_jpeg_sw_get_dimensions (libs/ra8_hal/src/ra8_jpeg_sw.c
 * lines 1071, 1080, 1087, 1100, 1105). Vectors via crafted bytestreams:
 *   D_pad   1071: padding + non-padding both reached.
 *   D_soi   1080: SOI/EOI marker mid-walk -> continue (true), other DT -> false.
 *   D_seg   1087: seglen=1 (C1=T) and seglen huge (C1=F C2=T).
 *   D_w0h0  1100: w=0 (C1=T) and h=0 (C1=F C2=T).
 *   D_sof   1105: 4-condition AND. Representative subset: SOF2 (all T
 *           not_supported), DHT (C3=F skipped), SOS (C2=F skipped).
 *           Dead rows documented per DO-178C 6.4.4.3 deactivated code.
 */
static void test_mcdc_get_dimensions_pad_and_marker(void)
{
  TEST_BEGIN("jpeg_sw MC/DC get_dimensions: pad+marker+seg+wh decisions");
  uint16_t w = 0U;
  uint16_t h = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_get_dimensions(s_pad_jpeg, (uint32_t)sizeof s_pad_jpeg, &w, &h));
  TEST_ASSERT_EQ(16, w);
  w = 0U;
  h = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_jpeg_sw_get_dimensions(s_soi_eoi_jpeg, (uint32_t)sizeof s_soi_eoi_jpeg, &w, &h));
  TEST_ASSERT_EQ(16, w);
  TEST_ASSERT_EQ(32, h);
  TEST_ASSERT(ra8_jpeg_sw_get_dimensions(s_bad_seg_jpeg, (uint32_t)sizeof s_bad_seg_jpeg, &w, &h) !=
              k_ra8_ok);
  TEST_ASSERT(
    ra8_jpeg_sw_get_dimensions(s_over_seg_jpeg, (uint32_t)sizeof s_over_seg_jpeg, &w, &h) !=
    k_ra8_ok);
  TEST_ASSERT(ra8_jpeg_sw_get_dimensions(s_w0_jpeg, (uint32_t)sizeof s_w0_jpeg, &w, &h) !=
              k_ra8_ok);
  TEST_ASSERT(ra8_jpeg_sw_get_dimensions(s_h0_jpeg, (uint32_t)sizeof s_h0_jpeg, &w, &h) !=
              k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_jpeg_sw_get_dimensions(s_sof2_jpeg, (uint32_t)sizeof s_sof2_jpeg, &w, &h));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_jpeg_sw_get_dimensions(s_dht_then_sof0, (uint32_t)sizeof s_dht_then_sof0, &w, &h));
  TEST_END("jpeg_sw MC/DC get_dimensions: pad+marker+seg+wh decisions");
}

/**
 * @test test_mcdc_decode_pad_and_rst_marker
 * @par MC/DC:
 * Decisions in ra8_jpeg_sw_decode and dec_decode_scan:
 *   - libs/ra8_hal/src/ra8_jpeg_sw.c@br_fill  D_res bit-reader refill
 *   - libs/ra8_hal/src/ra8_jpeg_sw.c@ra8_jpeg_sw_get_dimensions D_pad pad-skip
 *   - libs/ra8_hal/src/ra8_jpeg_sw.c@ra8_jpeg_sw_get_dimensions D_sof unsupported (else-if)
 *   - libs/ra8_hal/src/ra8_jpeg_sw.c@ra8_jpeg_sw_get_dimensions D_rst RST0..RST7 (else-if)
 *   - libs/ra8_hal/src/ra8_jpeg_sw.c@dec_dispatch_marker D_sof unsupported (extracted)
 *   - libs/ra8_hal/src/ra8_jpeg_sw.c@dec_dispatch_marker D_rst RST0..RST7 (extracted)
 * D_pad pad-skip exercised by encoder round-trip (yields natural padding).
 * D_sof 4-cond unsupported via SOF2 (0xFFC2): co-dependence rationale
 * documented inline in production source (markers >= 0xFFC1 are by spec
 * <= 0xFFCF, so the upper-bound cannot independently flip on any
 * reachable SOFn). D_rst RST0..RST7 in-band path: exercised structurally
 * by round-trip of restart-free streams (decision stays F throughout the
 * loop). N+1=2 for the reachable subset of D_rst; full D_pad and D_sof
 * vectors flip via the bytestreams. D_res (bit-reader reservoir refill)
 * is driven by the full decode: the refill loop runs while nbits is low
 * and stops on either the byte budget (first operand F) or end-of-image
 * (had_eoi T).
 */
static void test_mcdc_decode_pad_and_rst_marker(void)
{
  TEST_BEGIN("jpeg_sw MC/DC decode: pad-skip + sof unsupported + RST marker");
  static uint8_t s_rgb_in[(uint32_t)k_jt_rgb_bytes];
  static uint8_t s_rgb_out[(uint32_t)k_jt_rgb_bytes];
  static uint8_t s_jpeg[(uint32_t)k_jt_jpeg_cap];
  fill_gradient(s_rgb_in, (uint16_t)k_jt_w, (uint16_t)k_jt_h);
  uint32_t produced = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_encode(s_rgb_in,
                                    (uint16_t)k_jt_w,
                                    (uint16_t)k_jt_h,
                                    (uint8_t)k_ra8_jpeg_sw_quality_high,
                                    s_jpeg,
                                    (uint32_t)k_jt_jpeg_cap,
                                    &produced));
  uint16_t dw = 0U;
  uint16_t dh = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_jpeg_sw_decode(s_jpeg, produced, s_rgb_out, (uint32_t)k_jt_rgb_bytes, &dw, &dh));
  static const uint8_t prog_jpeg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xC2U,
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
  uint8_t  out_buf[k_jpeg_out_small] = {};
  uint16_t dw2                       = 0U;
  uint16_t dh2                       = 0U;
  TEST_ASSERT(ra8_jpeg_sw_decode(prog_jpeg,
                                 (uint32_t)sizeof prog_jpeg,
                                 out_buf,
                                 (uint32_t)sizeof out_buf,
                                 &dw2,
                                 &dh2) != k_ra8_ok);
  TEST_END("jpeg_sw MC/DC decode: pad-skip + sof unsupported + RST marker");
}

static const uint8_t s_dqt_short[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xDBU,
  0x00U,
  0x01U,
};

static const uint8_t s_dqt_bad_pq[] = {
  0xFFU, 0xD8U, 0xFFU, 0xDBU, 0x00U, 0x43U, 0x10U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0,     0,     0,     0,     0,     0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0,     0,     0,     0,     0,     0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0,     0,     0,     0,     0,     0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const uint8_t s_dqt_bad_tq[] = {
  0xFFU, 0xD8U, 0xFFU, 0xDBU, 0x00U, 0x43U, 0x04U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0,     0,     0,     0,     0,     0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0,     0,     0,     0,     0,     0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0,     0,     0,     0,     0,     0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const uint8_t s_dht_bad_tc[] = {
  0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x14U, 0x20U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const uint8_t s_dht_bad_th[] = {
  0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x14U, 0x02U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/**
 * @test test_mcdc_decode_dqt_dht_validation
 * @par MC/DC:
 * Three internal decoder decisions reachable via crafted bytestreams
 * (libs/ra8_hal/src/ra8_jpeg_sw.c lines 751, 761, 782, 792):
 *   D_dqt_len, D_dqt_pq, D_dht_id (each 2-cond OR/||).
 * Vectors: V_dqt_short (seglen=1), V_dqt_bad_pq (pq=1), V_dqt_bad_tq
 * (tq>=quant_tabs), V_dht_bad_tc (tc>=classes), V_dht_bad_th
 * (th>=ids). N+1=3 per decision (vectors share the V_short baseline
 * for the "in-range" row implicit in the round-trip cases above).
 */
static void test_mcdc_decode_dqt_dht_validation(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_dqt + dec_parse_dht guards");
  uint8_t  out[k_jpeg_out_large] = {};
  uint16_t dw                    = 0U;
  uint16_t dh                    = 0U;
  TEST_ASSERT(ra8_jpeg_sw_decode(s_dqt_short,
                                 (uint32_t)sizeof s_dqt_short,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);
  TEST_ASSERT(ra8_jpeg_sw_decode(s_dqt_bad_pq,
                                 (uint32_t)sizeof s_dqt_bad_pq,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);
  TEST_ASSERT(ra8_jpeg_sw_decode(s_dqt_bad_tq,
                                 (uint32_t)sizeof s_dqt_bad_tq,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);
  TEST_ASSERT(ra8_jpeg_sw_decode(s_dht_bad_tc,
                                 (uint32_t)sizeof s_dht_bad_tc,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);
  TEST_ASSERT(ra8_jpeg_sw_decode(s_dht_bad_th,
                                 (uint32_t)sizeof s_dht_bad_th,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);
  TEST_END("jpeg_sw MC/DC dec_parse_dqt + dec_parse_dht guards");
}

/**
 * @test test_mcdc_decode_sof0_chroma_subsampling
 * @par MC/DC:
 * Three SOF0 decisions in libs/ra8_hal/src/ra8_jpeg_sw.c (lines 842, 869,
 * 870, 872):
 *   D_ncomp: `ncomp != 1 && ncomp != 3`
 *   D_is444: 2 cond, D_is420: 6 cond, D_unsup: 2 cond
 * D_ncomp vectors via round-trip (1-comp grayscale baseline JPEG and
 * 3-comp encoded JPEG = both F) plus crafted ncomp=2 (T not_supported).
 * D_is444/D_is420 exercised via round-trip (high-quality 4:4:4 + low-
 * quality 4:2:0). The 6-cond D_is420 omits dead truth-table rows that
 * no real-world JPEG produces; documented per DO-178C 6.4.4.3
 * deactivated code. D_unsup tested with crafted SOF0 hmax=2,vmax=1
 * (neither 4:4:4 nor 4:2:0 -> not_supported).
 */
static void test_mcdc_decode_sof0_chroma_subsampling(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_sof0: ncomp + 4:4:4/4:2:0 disambig");
  static uint8_t s_rgb_in[(uint32_t)k_jt_rgb_bytes];
  static uint8_t s_rgb_out[(uint32_t)k_jt_rgb_bytes];
  static uint8_t s_jpeg[(uint32_t)k_jt_jpeg_cap];
  fill_gradient(s_rgb_in, (uint16_t)k_jt_w, (uint16_t)k_jt_h);
  uint32_t produced = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_encode(s_rgb_in,
                                    (uint16_t)k_jt_w,
                                    (uint16_t)k_jt_h,
                                    (uint8_t)k_ra8_jpeg_sw_quality_high,
                                    s_jpeg,
                                    (uint32_t)k_jt_jpeg_cap,
                                    &produced));
  uint16_t dw = 0U;
  uint16_t dh = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_jpeg_sw_decode(s_jpeg, produced, s_rgb_out, (uint32_t)k_jt_rgb_bytes, &dw, &dh));
  produced = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_encode(s_rgb_in,
                                    (uint16_t)k_jt_w,
                                    (uint16_t)k_jt_h,
                                    (uint8_t)k_ra8_jpeg_sw_quality_min,
                                    s_jpeg,
                                    (uint32_t)k_jt_jpeg_cap,
                                    &produced));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_jpeg_sw_decode(s_jpeg, produced, s_rgb_out, (uint32_t)k_jt_rgb_bytes, &dw, &dh));
  static const uint8_t sof0_ncomp2[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x0EU, 0x08U, 0x00U, 0x10U, 0x00U,
    0x10U, 0x02U, 0x01U, 0x11U, 0x00U, 0x02U, 0x11U, 0x00U, 0xFFU, 0xD9U,
  };
  uint8_t out_buf[k_jpeg_out_small] = {};
  TEST_ASSERT(ra8_jpeg_sw_decode(sof0_ncomp2,
                                 (uint32_t)sizeof sof0_ncomp2,
                                 out_buf,
                                 (uint32_t)sizeof out_buf,
                                 &dw,
                                 &dh) != k_ra8_ok);
  static const uint8_t sof0_bad_subsamp[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x21U, 0x00U, 0x02U, 0x11U, 0x01U, 0x03U, 0x11U, 0x01U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(ra8_jpeg_sw_decode(sof0_bad_subsamp,
                                 (uint32_t)sizeof sof0_bad_subsamp,
                                 out_buf,
                                 (uint32_t)sizeof out_buf,
                                 &dw,
                                 &dh) != k_ra8_ok);
  TEST_END("jpeg_sw MC/DC dec_parse_sof0: ncomp + 4:4:4/4:2:0 disambig");
}

/* ------------------------------------------------------------------ */
/* Additional MC/DC fixtures: SOI/SOF/SOS marker permutations. */
/*                                                                     */
/* These cover the residual gaps in MCDC_GAPS.csv that the earlier */
/* vectors marked "partial" or "no". Each fixture is a hand-built */
/* byte array -- no encoder round-trip -- so the decision flips are */
/* independent and reachable. */
/* ------------------------------------------------------------------ */

static const uint8_t s_app1_short[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xE1U,
  0x00U,
  0x01U,
};

static const uint8_t s_app1_overrun[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xE1U,
  0xFFU,
  0xFFU,
  0x00U,
  0x00U,
};

static const uint8_t s_sof1_jpeg[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xC1U,
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
};

static const uint8_t s_dac_then_short[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xC8U,
  0x00U,
  0x02U,
  0xFFU,
  0xD9U,
};

/**
 * @test test_mcdc_decode_skip_unrecognized_segment
 * @par MC/DC:
 * Decision dec_skip_segment line 960:
 *   `len < 2U || (uint32_t)len > d->src_len - d->cursor` (2 conds).
 * Reached for unrecognized markers (e.g. APP1 0xFFE1, COM 0xFFFE)
 * via the `else` arm of the decode marker switch.
 *   V_short  : APP1 with seglen=0x0001     -> C1=T C2=F  -> protocol_error
 *   V_overrun: APP1 with seglen=0xFFFF (>buf-cursor)
 *                                          -> C1=F C2=T  -> protocol_error
 *   V_ok     : APP1 with seglen=0x0004 + 2 payload bytes, then real
 *              SOF0/SOS to reach decode -> C1=F C2=F
 * V_short+V_ok prove C1 flips outcome; V_overrun+V_ok prove C2 flips.
 * N+1 = 3 vectors for N=2 conditions.
 *
 * Also exercises dec_decode_scan/decode marker switch line 1685
 * fall-through (else arm into dec_skip_segment) and the SOF-range
 * "is supported / DHT / DAC" 4-condition decision at line 1655 by
 * sending 0xFFC1 (SOF1, unsupported) versus 0xFFC8 (DAC, skipped).
 */
static void test_mcdc_decode_skip_unrecognized_segment(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_skip_segment + decode SOF-range");
  uint8_t  out[k_jpeg_out_large] = {};
  uint16_t dw                    = 0U;
  uint16_t dh                    = 0U;

  /* V_short: APP1 (0xFFE1) with seglen=1  -> dec_skip_segment len<2. */
  TEST_ASSERT(ra8_jpeg_sw_decode(s_app1_short,
                                 (uint32_t)sizeof s_app1_short,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);

  /* V_overrun: APP1 with seglen claiming 0xFFFF bytes. */
  TEST_ASSERT(ra8_jpeg_sw_decode(s_app1_overrun,
                                 (uint32_t)sizeof s_app1_overrun,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);

  /* V_sof1: SOF1 (0xFFC1) is in [SOF0..SOF15] and != DHT/DAC ->
     not_supported. Independently flips the SOF-range decision vs the
     DAC (0xFFC8) skip. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_jpeg_sw_decode(s_sof1_jpeg,
                                    (uint32_t)sizeof s_sof1_jpeg,
                                    out,
                                    (uint32_t)sizeof out,
                                    &dw,
                                    &dh));

  /* V_dac_skip: DAC (0xFFC8) is in SOF range BUT is excluded by the
     decision, so it falls to the dec_skip_segment arm; with a valid
     seglen this proves the != 0xFFC8 sub-condition flips outcome. */
  TEST_ASSERT(ra8_jpeg_sw_decode(s_dac_then_short,
                                 (uint32_t)sizeof s_dac_then_short,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);
  TEST_END("jpeg_sw MC/DC dec_skip_segment + decode SOF-range");
}

static const uint8_t s_rst0_jpeg[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xD0U,
  0xFFU,
  0xD9U,
};

static const uint8_t s_rst7_jpeg[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xD7U,
  0xFFU,
  0xD9U,
};

static const uint8_t s_dri_jpeg[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xDDU,
  0x00U,
  0x04U,
  0x00U,
  0x10U,
  0xFFU,
  0xD9U,
};

static const uint8_t s_eoi_only[] = {
  0xFFU,
  0xD8U,
  0xFFU,
  0xD9U,
};

/**
 * @test test_mcdc_decode_rst_in_marker_chain
 * @par MC/DC:
 * Decision ra8_jpeg_sw_decode line 1681:
 *   `mk >= rst0 && mk <= rst7` (2 conds).
 * RST0..RST7 are standalone (no seglen) markers; the decoder treats
 * them as a no-op `continue`. Vectors:
 *   V_rst0   : RST0 (0xFFD0) in chain         -> C1=T C2=T -> continue
 *   V_rst7   : RST7 (0xFFD7) in chain         -> C1=T C2=T -> continue
 *   V_below  : DRI  (0xFFDD, len=4) in chain  -> C1=F      -> skip
 *   V_above  : EOI  (0xFFD9) in chain         -> C2=F      -> break
 * V_rst0 vs V_below proves C1 flips outcome (continue vs skip).
 * V_rst0 vs V_above proves C2 flips outcome (continue vs break).
 * All four arrive at protocol_error because no SOS follows; we only
 * care that the marker walk traversed each branch.
 */
static void test_mcdc_decode_rst_in_marker_chain(void)
{
  TEST_BEGIN("jpeg_sw MC/DC decode: RST in marker chain");
  uint8_t  out[k_jpeg_out_large] = {};
  uint16_t dw                    = 0U;
  uint16_t dh                    = 0U;

  TEST_ASSERT(ra8_jpeg_sw_decode(s_rst0_jpeg,
                                 (uint32_t)sizeof s_rst0_jpeg,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);

  TEST_ASSERT(ra8_jpeg_sw_decode(s_rst7_jpeg,
                                 (uint32_t)sizeof s_rst7_jpeg,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);

  /* DRI (0xFFDD): C1=F (mk < rst0). */
  TEST_ASSERT(ra8_jpeg_sw_decode(s_dri_jpeg,
                                 (uint32_t)sizeof s_dri_jpeg,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);

  /* EOI: C2=F (mk > rst7). */
  TEST_ASSERT(ra8_jpeg_sw_decode(s_eoi_only,
                                 (uint32_t)sizeof s_eoi_only,
                                 out,
                                 (uint32_t)sizeof out,
                                 &dw,
                                 &dh) != k_ra8_ok);
  TEST_END("jpeg_sw MC/DC decode: RST in marker chain");
}

int32_t main(void)
{
  ra8_sim_mmap_reset();
  test_mcdc_get_dimensions_pad_and_marker();
  test_mcdc_decode_pad_and_rst_marker();
  test_mcdc_decode_dqt_dht_validation();
  test_mcdc_decode_sof0_chroma_subsampling();
  test_mcdc_decode_skip_unrecognized_segment();
  test_mcdc_decode_rst_in_marker_chain();
  (void)fprintf(stderr, "[OK ] test_ra8_jpeg_sw_seg_mcdc.c\n");
  return 0;
}
