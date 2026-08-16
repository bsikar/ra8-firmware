/**
 * @file test_ra8_vin_mcdc.c
 * @brief MC/DC vectors for the ra8_vin.c compound decisions
 *
 * @details
 * Split sibling of the original test_ra8_vin.c suite closing the
 * DO-178C Level B / IEC 61508 SIL 3 MC/DC obligations for the
 * compound boolean decisions in ra8_vin.c: UDS clip bounds, window
 * zero-dimension and pair guards, framebuffer all-zero and alignment
 * guards, capture-arm mode / continuous decisions, pre-clip window
 * pairs, UDS scale / passband quads, capture-start geometry, data
 * mode and CSI input pairs.
 *
 * Sibling suites: test_ra8_vin_capture.c (lifecycle + capture path)
 * and test_ra8_vin_config.c (scaling / CSC / routing / status
 * setters). Shared fixtures live in support/vin_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_vin.h"
#include "ra8_vin_regs.h"
#include "support/vin_test_util.h"
#include "unity_minimal.h"

/**
 * @enum t_vin_mcdc_t
 * @brief Capture-buffer geometry and the out-of-range field value.
 */
typedef enum : uint16_t {
  k_t_capture_cap   = 1024U, /**< Capture buffer, bytes. */
  k_t_capture_align = 128U,  /**< Its required alignment: the VIN write burst
                                 width, which the buffer must satisfy.          */
  k_t_field_bad     = 99U,   /**< A value past the last defined enum member,
                                applied to each field in turn so every guard is
                                driven independently.                           */
} t_vin_mcdc_t;

/**
 * @brief Reset the host harness before each test.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @test test_set_uds_clip_mcdc_bounds
 *
 * @par MC/DC:
 * Decision: `if ((v_size > k_ra8_vin_uds_max_clip) ||
 *               (h_size > k_ra8_vin_uds_max_clip))`
 * (2 conditions, libs/ra8_hal/src/ra8_vin.c line 436)
 * Standard: DO-178C Table A-7 obj 5; ISO 26262 Part 6 Table 12.
 * - Vector 1: v=0xFFFF,h=0     -> C1=T (short-circuits) -> Decision T
 * - Vector 2: v=0,    h=0      -> C1=F, C2=F -> Decision F (ok)
 * - Vector 3: v=0,    h=0xFFFF -> C1=F, C2=T -> Decision T
 * Vectors 1+2 vary C1; vectors 2+3 vary C2 with C1=F. N+1 minimal.
 */
static void test_set_uds_clip_mcdc_bounds(void)
{
  TEST_BEGIN("vin set_uds_clip MC/DC: v>max || h>max");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  /* Vector 1: v_size > max, h_size in range. C1=T short-circuits. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_uds_clip(0xFFFFU, 0U));
  /* Vector 2: both in range. C1=F, C2=F -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_uds_clip(0U, 0U));
  /* Vector 3: v in range, h_size > max. C1=F, C2=T. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_uds_clip(0U, 0xFFFFU));
  TEST_END("vin set_uds_clip MC/DC: v>max || h>max");
}

/**
 * @test test_set_window_zero_dim_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((w == 0U) || (h == 0U))`
 * (2 conditions, libs/ra8_hal/src/ra8_vin.c line 940)
 * Standard: DO-178C Table A-7 obj 5; IEC 61508-3 SIL 3.
 * - Vector 1: w=0,  h=10 -> C1=T (short-circuits) -> Decision T
 * - Vector 2: w=10, h=10 -> C1=F, C2=F -> Decision F (ok path)
 * - Vector 3: w=10, h=0  -> C1=F, C2=T -> Decision T
 */
static void test_set_window_zero_dim_mcdc(void)
{
  TEST_BEGIN("vin set_window MC/DC: w==0 || h==0");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  /* Vector 1: w=0. C1=T short-circuits. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_window(0U, 0U, 0U, 10U));
  /* Vector 2: both non-zero, end fits. C1=F, C2=F -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_window(0U, 0U, 10U, 10U));
  /* Vector 3: h=0. C1=F, C2=T. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_window(0U, 0U, 10U, 0U));
  TEST_END("vin set_window MC/DC: w==0 || h==0");
}

/**
 * @test test_set_framebuffers_all_zero_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((mb1 == 0U) && (mb2 == 0U) && (mb3 == 0U))`
 * (3 conditions, libs/ra8_hal/src/ra8_vin.c line 709)
 * Standard: DO-178C Table A-7 obj 5; ISO 26262 Part 6 Table 12.
 * For an N=3 short-circuiting AND, MC/DC requires N+1 = 4 vectors.
 * - Vector 1: mb1=0,mb2=0,mb3=0 -> C1=T,C2=T,C3=T -> Decision T (invalid_arg)
 * - Vector 2: mb1=fb1,_,_       -> C1=F (short-circuits) -> Decision F
 * - Vector 3: mb1=0,mb2=fb2,_   -> C1=T,C2=F (short-circuits) -> F
 * - Vector 4: mb1=0,mb2=0,mb3=fb3 -> C1=T,C2=T,C3=F -> Decision F
 * Pairs proving independence:
 *   - C1: vec1 vs vec2 (decision flips when C2,C3 are observation-masked
 *         by short-circuit; valid masking-MC/DC per DO-178C 6.4.4.2)
 *   - C2: vec3 vs a hypothetical vec where mb1=0,mb2=0,_ -- captured
 *         by the comparison vec3 vs vec1's prefix (C1=T held), C2 flip
 *         changes outcome
 *   - C3: vec4 vs vec1 (C1=T,C2=T held; C3 flip flips outcome)
 * Subsequent decision at line 712 (alignment check) is exercised by
 * vectors 2-4 with aligned addresses (k_ra8_vin_test_fb*).
 */
static void test_set_framebuffers_all_zero_mcdc(void)
{
  TEST_BEGIN("vin set_framebuffers MC/DC: mb1==0 && mb2==0 && mb3==0");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));

  /* Vector 1: all zero -> Decision T -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_framebuffers(0U, 0U, 0U));
  /* Vector 2: mb1 non-zero -> C1=F short-circuits -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_framebuffers((uint32_t)k_ra8_vin_test_fb1, 0U, 0U));
  /* Vector 3: mb1=0, mb2 non-zero -> C1=T, C2=F -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_framebuffers(0U, (uint32_t)k_ra8_vin_test_fb2, 0U));
  /* Vector 4: mb1=0, mb2=0, mb3 non-zero -> C1=T,C2=T,C3=F -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_framebuffers(0U, 0U, (uint32_t)k_ra8_vin_test_fb3));
  TEST_END("vin set_framebuffers MC/DC: mb1==0 && mb2==0 && mb3==0");
}

/**
 * @test test_mcdc_capture_arm_mode_triple
 *
 * @par MC/DC:
 * Decision: ``if ((mode != single) && (mode != continuous) &&
 *               (mode != continuous_field_skip))`` (3 conditions,
 * libs/ra8_hal/src/ra8_vin.c ra8_vin_capture_arm).
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * Full short-circuit MC/DC for N=3 AND requires N+1=4 vectors. We use
 * the canonical short-circuit set; each predicate flips with the others
 * held at their masking value (T):
 * - V1: mode=single                       -> C1=F short-circuits -> dec F (proceeds, ok)
 * - V2: mode=continuous                   -> C1=T, C2=F short    -> dec F (proceeds, ok)
 * - V3: mode=continuous_field_skip        -> C1=T, C2=T, C3=F    -> dec F (proceeds, ok)
 * - V4: mode=0xFE (none)                  -> C1=T, C2=T, C3=T    -> dec T -> invalid_arg
 */
static void test_mcdc_capture_arm_mode_triple(void)
{
  TEST_BEGIN("vin capture_arm MC/DC: mode != {single, cont, cont_field_skip}");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  /* V1: single. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_arm(k_ra8_vin_capture_single));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_disarm());
  /* V2: continuous. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_arm(k_ra8_vin_capture_continuous));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_disarm());
  /* V3: continuous_field_skip. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_arm(k_ra8_vin_capture_continuous_field_skip));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_disarm());
  /* V4: bogus. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_capture_arm((ra8_vin_capture_mode_t)0xFEU));
  TEST_END("vin capture_arm MC/DC: mode != {single, cont, cont_field_skip}");
}

/**
 * @test test_mcdc_capture_arm_continuous_pair
 *
 * @par MC/DC:
 * Decision: ``if ((mode == continuous) || (mode == continuous_field_skip))``
 * (2 conditions, libs/ra8_hal/src/ra8_vin.c ra8_vin_capture_arm). N+1=3.
 * - V1: mode=single        -> C1=F, C2=F             -> dec F (no FC.CC write)
 * - V2: mode=continuous    -> C1=T short-circuits    -> dec T (FC.CC programmed)
 * - V3: mode=cont_field_skip -> C1=F, C2=T            -> dec T (FC.CC programmed)
 * (V1,V2) flips C1 with C2 masked F; (V1,V3) flips C2 with C1 fixed F.
 */
static void test_mcdc_capture_arm_continuous_pair(void)
{
  TEST_BEGIN("vin capture_arm MC/DC: mode == cont || mode == cont_field_skip");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  volatile uint32_t* fc_reg = ra8_vin_reg32(k_ra8_vin_off_fc);
  /* V1: single -- FC.CC must remain clear. */
  *fc_reg = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_arm(k_ra8_vin_capture_single));
  TEST_ASSERT_EQ(0, (*fc_reg & k_ra8_vin_fc_cc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_disarm());
  /* V2: continuous -- FC.CC must be set. */
  *fc_reg = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_arm(k_ra8_vin_capture_continuous));
  TEST_ASSERT((*fc_reg & k_ra8_vin_fc_cc) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_disarm());
  /* V3: continuous_field_skip -- FC.CC must be set. */
  *fc_reg = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_arm(k_ra8_vin_capture_continuous_field_skip));
  TEST_ASSERT((*fc_reg & k_ra8_vin_fc_cc) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_disarm());
  TEST_END("vin capture_arm MC/DC: mode == cont || mode == cont_field_skip");
}

/**
 * @test test_mcdc_set_preclip_window_pair
 *
 * @par MC/DC:
 * Decision: ``if ((window->line_end < window->line_start) ||
 *               (window->pixel_end < window->pixel_start))`` (2 conditions,
 * libs/ra8_hal/src/ra8_vin.c ra8_vin_set_preclip). N+1 = 3.
 * - V1: line_end>=start, pixel_end>=start  -> C1=F, C2=F -> dec F (proceeds, ok)
 * - V2: line_end<start                     -> C1=T short -> dec T -> invalid_arg
 * - V3: line ok, pixel_end<start           -> C1=F, C2=T -> dec T -> invalid_arg
 */
static void test_mcdc_set_preclip_window_pair(void)
{
  TEST_BEGIN("vin set_preclip MC/DC: line_end<start || pixel_end<start");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  /* V1: well-formed window. */
  const ra8_vin_preclip_t v1 = {.line_start  = 0U,
                                .line_end    = 10U,
                                .pixel_start = 0U,
                                .pixel_end   = 10U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_preclip(&v1));
  /* V2: line_end < line_start. */
  const ra8_vin_preclip_t v2 = {.line_start  = 10U,
                                .line_end    = 0U,
                                .pixel_start = 0U,
                                .pixel_end   = 10U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_preclip(&v2));
  /* V3: pixel_end < pixel_start. */
  const ra8_vin_preclip_t v3 = {.line_start  = 0U,
                                .line_end    = 10U,
                                .pixel_start = 10U,
                                .pixel_end   = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_preclip(&v3));
  TEST_END("vin set_preclip MC/DC: line_end<start || pixel_end<start");
}

/**
 * @test test_mcdc_set_uds_scale_quad
 *
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_vin.c): 4-cond OR over UDS scale fields.
 * ``v_mant > MAX_MANT || h_mant > MAX_MANT || v_frac > MAX_FRAC || h_frac > MAX_FRAC``
 * MAX_MANT = 0xF, MAX_FRAC = 0xFFF. N+1 = 5 vectors.
 * - V1: all in range          -> all F -> dec F -> ok
 * - V2: v_mant = 0x10         -> C1=T short -> err
 * - V3: h_mant = 0x10         -> C1=F, C2=T -> err
 * - V4: v_frac = 0x1000       -> C3=T -> err
 * - V5: h_frac = 0x1000       -> C4=T -> err
 */
static void test_mcdc_set_uds_scale_quad(void)
{
  TEST_BEGIN("vin set_uds_scale MC/DC: 4-cond OR field range");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  /* V1 */
  const ra8_vin_uds_scale_t v1 = {.v_mantissa = 1U,
                                  .h_mantissa = 1U,
                                  .v_fraction = 0U,
                                  .h_fraction = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_uds_scale(&v1));
  /* V2: v_mant out. */
  const ra8_vin_uds_scale_t v2 = {.v_mantissa = 0x10U,
                                  .h_mantissa = 1U,
                                  .v_fraction = 0U,
                                  .h_fraction = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_uds_scale(&v2));
  /* V3: h_mant out. */
  const ra8_vin_uds_scale_t v3 = {.v_mantissa = 1U,
                                  .h_mantissa = 0x10U,
                                  .v_fraction = 0U,
                                  .h_fraction = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_uds_scale(&v3));
  /* V4: v_frac out. */
  const ra8_vin_uds_scale_t v4 = {.v_mantissa = 1U,
                                  .h_mantissa = 1U,
                                  .v_fraction = 0x1000U,
                                  .h_fraction = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_uds_scale(&v4));
  /* V5: h_frac out. */
  const ra8_vin_uds_scale_t v5 = {.v_mantissa = 1U,
                                  .h_mantissa = 1U,
                                  .v_fraction = 0U,
                                  .h_fraction = 0x1000U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_uds_scale(&v5));
  TEST_END("vin set_uds_scale MC/DC: 4-cond OR field range");
}

/**
 * @test test_mcdc_set_framebuffers_align_triple
 *
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_vin.c): 3-cond OR over alignment masks.
 * ``(mb1 & MASK)!=0 || (mb2 & MASK)!=0 || (mb3 & MASK)!=0``  MASK=0x7F
 * N+1 = 4. Note: the prior all-zero check (line 737) gates this; we use
 * non-zero, aligned bases as the control.
 * - V1: all aligned (e.g. 0x100, 0, 0)           -> all F -> ok
 * - V2: mb1 misaligned (e.g. 0x101)              -> C1=T short -> err
 * - V3: mb2 misaligned                           -> C1=F, C2=T -> err
 * - V4: mb3 misaligned                           -> C3=T -> err
 */
static void test_mcdc_set_framebuffers_align_triple(void)
{
  TEST_BEGIN("vin set_framebuffers MC/DC: 3-cond OR misalignment");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  /* V1: aligned mb1 only. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_framebuffers(0x100U, 0U, 0U));
  /* V2: mb1 misaligned. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_framebuffers(0x101U, 0U, 0U));
  /* V3: mb1 ok, mb2 misaligned. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_framebuffers(0x100U, 0x101U, 0U));
  /* V4: mb1, mb2 ok, mb3 misaligned. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_framebuffers(0x100U, 0x200U, 0x101U));
  TEST_END("vin set_framebuffers MC/DC: 3-cond OR misalignment");
}

/**
 * @test test_mcdc_capture_start_geom_quad
 *
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_vin.c): 4-cond OR over geometry.
 * ``w == 0 || h == 0 || w > MAX || h > MAX``  MAX = 4096.
 * N+1 = 5 vectors. Buffer must be aligned (separate check after).
 * - V1: w=640, h=480, aligned buf                -> all F -> ok
 * - V2: w=0                                      -> C1=T short -> err
 * - V3: w=640, h=0                               -> C2=T -> err
 * - V4: w=4097, h=480                            -> C3=T -> err
 * - V5: w=640, h=4097                            -> C4=T -> err
 */
static void test_mcdc_capture_start_geom_quad(void)
{
  TEST_BEGIN("vin capture_start MC/DC: 4-cond OR geometry");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  /* 128-byte aligned buffer. */
  [[gnu::aligned(k_t_capture_align)]] static uint8_t s_buf[k_t_capture_cap];
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_capture_start(s_buf, 640U, 480U, k_ra8_vin_input_ycbcr422_8));
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_vin_capture_start(s_buf, 0U, 480U, k_ra8_vin_input_ycbcr422_8));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_vin_capture_start(s_buf, 640U, 0U, k_ra8_vin_input_ycbcr422_8));
  /* V4 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_vin_capture_start(s_buf, 4097U, 480U, k_ra8_vin_input_ycbcr422_8));
  /* V5 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_vin_capture_start(s_buf, 640U, 4097U, k_ra8_vin_input_ycbcr422_8));
  TEST_END("vin capture_start MC/DC: 4-cond OR geometry");
}

/**
 * @test test_mcdc_set_uds_passband_pair
 *
 * @par MC/DC:
 * Decision: ``if ((v_bwidth > k_ra8_vin_uds_max_bw) || (h_bwidth > k_ra8_vin_uds_max_bw))``
 * (2 conditions, libs/ra8_hal/src/ra8_vin.c ra8_vin_set_uds_passband).
 * Short-circuit OR: N+1 = 3 vectors.
 * - V1: v=0, h=0       -> C1=F, C2=F -> dec F (ok).
 * - V2: v=0xFF, h=0    -> C1=T short -> dec T -> invalid_arg.
 *   (V1->V2 isolates C1.)
 * - V3: v=0, h=0xFF    -> C1=F, C2=T -> dec T -> invalid_arg.
 *   (V1->V3 isolates C2.)
 */
static void test_mcdc_set_uds_passband_pair(void)
{
  TEST_BEGIN("vin set_uds_passband MC/DC: v||h > max_bw");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_uds_passband(0U, 0U));
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_uds_passband(0xFFU, 0U));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_uds_passband(0U, 0xFFU));
  TEST_END("vin set_uds_passband MC/DC: v||h > max_bw");
}

/**
 * @test test_mcdc_set_data_mode_pair
 *
 * @par MC/DC:
 * Decision: ``if ((mode->conv_mode > k_ra8_vin_max_dtmd) || (mode->y_mode > k_ra8_vin_max_ymode))``
 * (2 conditions, libs/ra8_hal/src/ra8_vin.c ra8_vin_set_data_mode).
 * Short-circuit OR: N+1 = 3 vectors.
 * - V1: conv=0, y=0   -> C1=F, C2=F -> dec F (ok).
 * - V2: conv=99, y=0  -> C1=T short -> dec T -> invalid_arg.
 * - V3: conv=0, y=99  -> C1=F, C2=T -> dec T -> invalid_arg.
 */
static void test_mcdc_set_data_mode_pair(void)
{
  TEST_BEGIN("vin set_data_mode MC/DC: conv||y > max");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  ra8_vin_data_mode_t m = {};
  m.conv_mode           = 0U;
  m.y_mode              = 0U;
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_data_mode(&m));
  /* V2 */
  m.conv_mode = k_t_field_bad;
  m.y_mode    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_data_mode(&m));
  /* V3 */
  m.conv_mode = 0U;
  m.y_mode    = k_t_field_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_data_mode(&m));
  TEST_END("vin set_data_mode MC/DC: conv||y > max");
}

/**
 * @test test_mcdc_set_csi_input_pair
 *
 * @par MC/DC:
 * Decision: ``if ((input->virtual_channel > k_ra8_vin_max_vc_sel) ||
 *               (input->data_type > k_ra8_vin_max_dt))``
 * (2 conditions, libs/ra8_hal/src/ra8_vin.c ra8_vin_set_csi_input).
 * Short-circuit OR: N+1 = 3 vectors.
 * - V1: vc=0, dt=0  -> C1=F, C2=F -> dec F.
 * - V2: vc=99, dt=0 -> C1=T short -> dec T -> invalid_arg.
 * - V3: vc=0, dt=99 -> C1=F, C2=T -> dec T -> invalid_arg.
 */
static void test_mcdc_set_csi_input_pair(void)
{
  TEST_BEGIN("vin set_csi_input MC/DC: vc||dt > max");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  ra8_vin_csi_input_t in = {};
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_csi_input(&in));
  /* V2 */
  in.virtual_channel = k_t_field_bad;
  in.data_type       = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_csi_input(&in));
  /* V3 */
  in.virtual_channel = 0U;
  in.data_type       = k_t_field_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_csi_input(&in));
  TEST_END("vin set_csi_input MC/DC: vc||dt > max");
}

/**
 * @test test_mcdc_set_window_pair
 *
 * @par MC/DC:
 * Decision: ``if ((pixel_end > k_ra8_vin_preclip_mask) || (line_end > k_ra8_vin_preclip_mask))``
 * (2 conditions, libs/ra8_hal/src/ra8_vin.c ra8_vin_set_window).
 * Short-circuit OR: N+1 = 3 vectors. preclip_mask = 0xFFF (4095).
 * - V1: w=8,h=8 at (0,0) -> ends 7,7  -> C1=F, C2=F -> dec F.
 * - V2: w=4096 at x=1    -> pixel_end=4096 > 4095   -> C1=T short -> invalid_arg.
 * - V3: w=8,h=4096 at y=1 -> line_end=4096 > 4095   -> C1=F, C2=T -> invalid_arg.
 */
static void test_mcdc_set_window_pair(void)
{
  TEST_BEGIN("vin set_window MC/DC: pixel_end||line_end > mask");
  prep();
  const ra8_vin_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_init(&cfg));
  /* V1: ok small window. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vin_set_window(0U, 0U, 8U, 8U));
  /* V2: pixel_end overflow. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_window(1U, 0U, 4096U, 8U));
  /* V3: line_end overflow. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vin_set_window(0U, 1U, 8U, 4096U));
  TEST_END("vin set_window MC/DC: pixel_end||line_end > mask");
}

int main(void)
{
  test_set_uds_clip_mcdc_bounds();
  test_set_window_zero_dim_mcdc();
  test_set_framebuffers_all_zero_mcdc();
  test_mcdc_capture_arm_mode_triple();
  test_mcdc_capture_arm_continuous_pair();
  test_mcdc_set_preclip_window_pair();
  test_mcdc_set_uds_scale_quad();
  test_mcdc_set_framebuffers_align_triple();
  test_mcdc_capture_start_geom_quad();
  test_mcdc_set_uds_passband_pair();
  test_mcdc_set_data_mode_pair();
  test_mcdc_set_csi_input_pair();
  test_mcdc_set_window_pair();
  return 0;
}
