/**
 * @file test_ra8_vin_capture.c
 * @brief Unit tests for the VIN lifecycle and capture datapath
 *
 * @details
 * Split sibling of the original test_ra8_vin.c suite covering the
 * lifecycle and capture surface of ra8_vin.c against the host-side
 * simulated MMIO (``ra8_sim_mmap``):
 *
 * - init happy path + null-cfg / zero-stride / IM / CLP rejection,
 *   reset
 * - capture arm in single / continuous / field-skip modes incl.
 *   invalid-mode, already-running, stop and idle-disarm legs
 * - the sweep-17 high-level surface: ra8_vin_capture_start(buf,w,h,
 *   format) geometry + alignment guards, stop wrapper, capture
 *   window, frame handler fan-out, deinit
 *
 * Sibling suites: test_ra8_vin_config.c (scaling / CSC / routing /
 * status setters) and test_ra8_vin_mcdc.c (MC/DC vectors). Shared
 * fixtures live in support/vin_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_vin.h"
#include "ra8_vin_regs.h"
#include "support/vin_test_util.h"
#include "unity_minimal.h"

/**
 * @brief Reset the host harness before each test.
 */
static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
}

/* ----------------------------------------------------------------------------
 * Lifecycle tests
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------*/

static void test_init_happy(void)
{
  TEST_BEGIN("vin init happy");
  prep();

  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  /* MC has BPS = 1, INF = RGB888 (6 << 16 = 0x00060000), IM = 1 << 3 = 0x08. */
  const uint32_t mc = *ra8_vin_reg32(k_ra8_vin_off_mc);
  TEST_ASSERT_EQ(0U, (mc & (uint32_t)k_ra8_vin_mc_me));
  TEST_ASSERT((mc & (uint32_t)k_ra8_vin_mc_bps) != 0U);
  TEST_ASSERT_EQ(((uint32_t)k_ra8_vin_input_rgb888 << 16), (mc & (uint32_t)k_ra8_vin_mc_inf));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_vin_im_odd_even << 3), (mc & (uint32_t)k_ra8_vin_mc_im));

  /* IS / MB1..3 / IE / SI all reflect the config. */
  TEST_ASSERT_EQ(k_ra8_vin_test_stride, *ra8_vin_reg32(k_ra8_vin_off_is));
  TEST_ASSERT_EQ(k_ra8_vin_test_fb1, *ra8_vin_reg32(k_ra8_vin_off_mb1));
  TEST_ASSERT_EQ(k_ra8_vin_test_fb2, *ra8_vin_reg32(k_ra8_vin_off_mb2));
  TEST_ASSERT_EQ(k_ra8_vin_test_fb3, *ra8_vin_reg32(k_ra8_vin_off_mb3));
  TEST_ASSERT_EQ(k_ra8_vin_test_ie_mask, *ra8_vin_reg32(k_ra8_vin_off_ie));
  TEST_ASSERT_EQ(k_ra8_vin_test_si_val, *ra8_vin_reg32(k_ra8_vin_off_si));
  TEST_END("vin init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("vin init null cfg");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vin_init(nullptr));
  TEST_END("vin init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_zero_stride(void)
{
  TEST_BEGIN("vin init zero stride");
  prep();

  ra8_vin_config_t cfg = make_cfg();
  cfg.image_stride_px  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_init(&cfg));
  TEST_END("vin init zero stride");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_im(void)
{
  TEST_BEGIN("vin init bad interlace mode");
  prep();
  ra8_vin_config_t cfg = make_cfg();
  cfg.interlace_mode   = 3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_init(&cfg));
  TEST_END("vin init bad interlace mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_clp(void)
{
  TEST_BEGIN("vin init bad clp mode");
  prep();
  ra8_vin_config_t cfg = make_cfg();
  cfg.pixel_clip_mode  = 4U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_init(&cfg));
  TEST_END("vin init bad clp mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset(void)
{
  TEST_BEGIN("vin reset");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_reset());
  const uint32_t mc = *ra8_vin_reg32(k_ra8_vin_off_mc);
  TEST_ASSERT_EQ(0U, (mc & (uint32_t)k_ra8_vin_mc_me));
  TEST_END("vin reset");
}

/* ----------------------------------------------------------------------------
 * Capture tests
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------*/

static void test_capture_arm_single(void)
{
  TEST_BEGIN("vin capture_arm single");
  prep();

  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_arm(k_ra8_vin_capture_single));
  const uint32_t mc = *ra8_vin_reg32(k_ra8_vin_off_mc);
  const uint32_t fc = *ra8_vin_reg32(k_ra8_vin_off_fc);
  TEST_ASSERT((mc & (uint32_t)k_ra8_vin_mc_me) != 0U);
  TEST_ASSERT_EQ(0U, (fc & (uint32_t)k_ra8_vin_fc_cc));
  TEST_END("vin capture_arm single");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_arm_continuous(void)
{
  TEST_BEGIN("vin capture_arm continuous");
  prep();

  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_arm(k_ra8_vin_capture_continuous));
  const uint32_t mc = *ra8_vin_reg32(k_ra8_vin_off_mc);
  const uint32_t fc = *ra8_vin_reg32(k_ra8_vin_off_fc);
  TEST_ASSERT((mc & (uint32_t)k_ra8_vin_mc_me) != 0U);
  TEST_ASSERT((fc & (uint32_t)k_ra8_vin_fc_cc) != 0U);
  TEST_END("vin capture_arm continuous");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_arm_field_skip(void)
{
  TEST_BEGIN("vin capture_arm field-skip");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_arm(k_ra8_vin_capture_continuous_field_skip));
  const uint32_t fc = *ra8_vin_reg32(k_ra8_vin_off_fc);
  TEST_ASSERT((fc & (uint32_t)k_ra8_vin_fc_cc) != 0U);
  TEST_END("vin capture_arm field-skip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_arm_invalid_mode(void)
{
  TEST_BEGIN("vin capture_arm invalid mode");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_capture_arm((ra8_vin_capture_mode_t)0xFFU));
  TEST_END("vin capture_arm invalid mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_arm_already_running(void)
{
  TEST_BEGIN("vin capture_arm already running");
  prep();

  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_arm(k_ra8_vin_capture_single));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_vin_capture_arm(k_ra8_vin_capture_single));
  TEST_END("vin capture_arm already running");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_stop(void)
{
  TEST_BEGIN("vin capture_disarm");
  prep();

  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_arm(k_ra8_vin_capture_continuous));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_stop());
  const uint32_t mc      = *ra8_vin_reg32(k_ra8_vin_off_mc);
  const uint32_t fc      = *ra8_vin_reg32(k_ra8_vin_off_fc);
  const uint32_t mtcstop = *ra8_vin_reg32(k_ra8_vin_off_mtcstop);
  TEST_ASSERT_EQ(0U, (mc & (uint32_t)k_ra8_vin_mc_me));
  TEST_ASSERT_EQ(0U, (fc & (uint32_t)k_ra8_vin_fc_cc));
  TEST_ASSERT((mtcstop & (uint32_t)k_ra8_vin_mtcstop_req) != 0U);
  TEST_END("vin capture_disarm");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_disarm_idle(void)
{
  TEST_BEGIN("vin capture_disarm idle");
  prep();

  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_vin_capture_stop());
  TEST_END("vin capture_disarm idle");
}

/* ----------------------------------------------------------------------------
 * Sweep 17: ra8_vin_capture_start(buf, w, h, format) + window + frame handler
 * --------------------------------------------------------------------------*/

/**
 * @enum ra8_vin_test_geom_t
 * @brief Magic-free geometry for the new high-level capture tests.
 */
typedef enum : uint16_t {
  k_ra8_vin_test_w        = 640U, /**< RA8 vin test w.        */
  k_ra8_vin_test_h        = 480U, /**< RA8 vin test h.        */
  k_ra8_vin_test_window_x = 16U,  /**< RA8 vin test window x. */
  k_ra8_vin_test_window_y = 8U,   /**< RA8 vin test window y. */
  k_ra8_vin_test_window_w = 320U, /**< RA8 vin test window w. */
  k_ra8_vin_test_window_h = 240U, /**< RA8 vin test window h. */
} ra8_vin_test_geom_t;

static uint32_t s_vin_frame_count;
static void*    s_vin_frame_last_buf;
static uint32_t s_vin_frame_last_len;
static void*    s_vin_frame_last_ctx;

/**
 * @brief Frame-end stub used by `test_attach_frame_handler`.
 */
static void stub_vin_frame(void* ctx, void* buf, uint32_t len)
{
  ++s_vin_frame_count;
  s_vin_frame_last_buf = buf;
  s_vin_frame_last_len = len;
  s_vin_frame_last_ctx = ctx;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_buf_happy(void)
{
  TEST_BEGIN("vin capture_start(buf,w,h,format) happy");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  void* const buf = (void*)(uintptr_t)k_ra8_vin_test_fb_alt;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vin_capture_start(buf,
                                       (uint16_t)k_ra8_vin_test_w,
                                       (uint16_t)k_ra8_vin_test_h,
                                       k_ra8_vin_input_rgb888));
  TEST_ASSERT_EQ(k_ra8_vin_test_fb_alt, *ra8_vin_reg32(k_ra8_vin_off_mb1));
  TEST_ASSERT_EQ(k_ra8_vin_test_w, *ra8_vin_reg32(k_ra8_vin_off_is));
  const uint32_t mc = *ra8_vin_reg32(k_ra8_vin_off_mc);
  TEST_ASSERT((mc & (uint32_t)k_ra8_vin_mc_me) != 0U);
  TEST_END("vin capture_start(buf,w,h,format) happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_buf_null(void)
{
  TEST_BEGIN("vin capture_start NULL buf");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_vin_capture_start(nullptr,
                                       (uint16_t)k_ra8_vin_test_w,
                                       (uint16_t)k_ra8_vin_test_h,
                                       k_ra8_vin_input_rgb888));
  TEST_END("vin capture_start NULL buf");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_buf_bad_geom(void)
{
  TEST_BEGIN("vin capture_start bad geometry");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  void* const buf = (void*)(uintptr_t)k_ra8_vin_test_fb_alt;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_vin_capture_start(buf, 0U, (uint16_t)k_ra8_vin_test_h, k_ra8_vin_input_rgb888));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_vin_capture_start(buf, (uint16_t)k_ra8_vin_test_w, 0U, k_ra8_vin_input_rgb888));
  TEST_END("vin capture_start bad geometry");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_buf_misaligned(void)
{
  TEST_BEGIN("vin capture_start misaligned buf");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  void* const bad = (void*)(uintptr_t)(k_ra8_vin_test_fb_alt + 1U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_vin_capture_start(bad,
                                       (uint16_t)k_ra8_vin_test_w,
                                       (uint16_t)k_ra8_vin_test_h,
                                       k_ra8_vin_input_rgb888));
  TEST_END("vin capture_start misaligned buf");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_stop_wrapper(void)
{
  TEST_BEGIN("vin capture_stop wrapper");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  void* const buf = (void*)(uintptr_t)k_ra8_vin_test_fb_alt;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vin_capture_start(buf,
                                       (uint16_t)k_ra8_vin_test_w,
                                       (uint16_t)k_ra8_vin_test_h,
                                       k_ra8_vin_input_rgb888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_stop());
  const uint32_t mc = *ra8_vin_reg32(k_ra8_vin_off_mc);
  TEST_ASSERT_EQ(0U, (mc & (uint32_t)k_ra8_vin_mc_me));
  TEST_END("vin capture_stop wrapper");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_window_happy(void)
{
  TEST_BEGIN("vin set_window happy");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vin_set_window((uint16_t)k_ra8_vin_test_window_x,
                                    (uint16_t)k_ra8_vin_test_window_y,
                                    (uint16_t)k_ra8_vin_test_window_w,
                                    (uint16_t)k_ra8_vin_test_window_h));
  TEST_ASSERT_EQ(k_ra8_vin_test_window_y, *ra8_vin_reg32(k_ra8_vin_off_slprc));
  TEST_ASSERT_EQ(k_ra8_vin_test_window_x, *ra8_vin_reg32(k_ra8_vin_off_spprc));
  TEST_END("vin set_window happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_window_invalid(void)
{
  TEST_BEGIN("vin set_window invalid");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_window(0U, 0U, 0U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_window(4090U, 0U, 100U, 1U));
  TEST_END("vin set_window invalid");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_frame_handler(void)
{
  TEST_BEGIN("vin attach_frame_handler");
  prep();
  s_vin_frame_count    = 0U;
  s_vin_frame_last_buf = nullptr;
  s_vin_frame_last_len = 0U;
  s_vin_frame_last_ctx = nullptr;

  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  void* const buf = (void*)(uintptr_t)k_ra8_vin_test_fb_alt;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_vin_attach_frame_handler(stub_vin_frame, (void*)(uintptr_t)k_ra8_vin_test_ctx));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vin_capture_start(buf,
                                       (uint16_t)k_ra8_vin_test_w,
                                       (uint16_t)k_ra8_vin_test_h,
                                       k_ra8_vin_input_raw8));

  /* Inject the FME bit into INTS, then dispatch. */
  *ra8_vin_reg32(k_ra8_vin_off_ints) = (uint32_t)k_ra8_vin_int_fme;
  ra8_vin_dispatch();
  TEST_ASSERT_EQ(1U, s_vin_frame_count);
  TEST_ASSERT_EQ((uintptr_t)buf, (uintptr_t)s_vin_frame_last_buf);
  /* RAW8 = 1 byte/pixel -> len = w*h. */
  TEST_ASSERT_EQ(((uint32_t)k_ra8_vin_test_w * (uint32_t)k_ra8_vin_test_h), s_vin_frame_last_len);
  TEST_ASSERT_EQ(k_ra8_vin_test_ctx, (uintptr_t)s_vin_frame_last_ctx);

  /* Detach -> next dispatch must not increment. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_attach_frame_handler(nullptr, nullptr));
  *ra8_vin_reg32(k_ra8_vin_off_ints) = (uint32_t)k_ra8_vin_int_fme;
  ra8_vin_dispatch();
  TEST_ASSERT_EQ(1U, s_vin_frame_count);
  TEST_END("vin attach_frame_handler");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("vin deinit");
  prep();

  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_deinit());

  TEST_ASSERT_EQ(0U, *ra8_vin_reg32(k_ra8_vin_off_mc));
  TEST_ASSERT_EQ(0U, *ra8_vin_reg32(k_ra8_vin_off_fc));
  TEST_ASSERT_EQ(0U, *ra8_vin_reg32(k_ra8_vin_off_ie));
  TEST_END("vin deinit");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_init_happy,
  test_init_null_cfg,
  test_init_zero_stride,
  test_init_bad_im,
  test_init_bad_clp,
  test_reset,
  test_capture_arm_single,
  test_capture_arm_continuous,
  test_capture_arm_field_skip,
  test_capture_arm_invalid_mode,
  test_capture_arm_already_running,
  test_capture_stop,
  test_capture_disarm_idle,
  test_capture_start_buf_happy,
  test_capture_start_buf_null,
  test_capture_start_buf_bad_geom,
  test_capture_start_buf_misaligned,
  test_capture_stop_wrapper,
  test_set_window_happy,
  test_set_window_invalid,
  test_attach_frame_handler,
  test_deinit,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_vin_capture.c\n");
  return 0;
}
