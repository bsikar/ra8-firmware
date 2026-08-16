/**
 * @file test_ra8_vreg.c
 * @brief Unit tests for ra8_vreg.c (Internal Voltage Regulator driver)
 *
 * @details
 * Exercises every public entry point defined in `ra8_vreg.h`:
 *   - init / deinit / reset (including bad-arg paths for every
 *     validated field).
 *   - LDO <-> DCDC mode switching including the conservative and
 *     fast-startup sequences.
 *   - VCCSEL programming for every documented voltage window.
 *   - OCP threshold programming for every enum value.
 *   - LDO charge-pump boost (LCBOOST) toggle.
 *   - Low-voltage profile programming (every profile, plus mutual
 *     exclusion).
 *   - Status snapshot for both LDO and DCDC modes.
 *   - clear_status with valid and reserved-bit masks.
 *   - enter_standby for every standby variant + invalid arg.
 *   - exit_standby with and without prior init.
 *   - Legacy enter_stop / exit_stop wrappers.
 *   - attach_handler / dispatch including detach via nullptr.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_vreg.h"
#include "ra8_vreg_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_vreg_bad_t
 * @brief Out-of-range enum values the voltage-regulator validator rejects.
 *
 * @details
 * Each is far outside its field's defined range, so a guard that merely masks
 * the value instead of range-checking it would still admit the setting.
 */
typedef enum : uint8_t {
  k_t_vccsel_over = 0x07U, /**< Past the 0..2 range of the VCC selector. */
  k_t_ocp_over    = 0x55U, /**< Past the over-current-protection range; also
                                 reused for the low-voltage profile field.      */
  k_t_mode_over   = 0xAAU, /**< Past the regulator-mode range. */
} t_vreg_bad_t;

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------
 */

static ra8_vreg_cfg_t make_cfg_dcdc(void)
{
  const ra8_vreg_cfg_t cfg = {
    .mode         = k_ra8_vreg_mode_dcdc,
    .vccsel       = k_ra8_vreg_vccsel_3v0_to_3v6,
    .ocp          = k_ra8_vreg_ocp_normal,
    .fast_startup = false,
    .ldo_boost    = false,
    .lv_profile   = k_ra8_vreg_lv_off,
  };
  return cfg;
}

static ra8_vreg_cfg_t make_cfg_dcdc_fast(void)
{
  ra8_vreg_cfg_t cfg = make_cfg_dcdc();
  cfg.fast_startup   = true;
  return cfg;
}

static ra8_vreg_cfg_t make_cfg_ldo(void)
{
  const ra8_vreg_cfg_t cfg = {
    .mode         = k_ra8_vreg_mode_ldo,
    .vccsel       = k_ra8_vreg_vccsel_2v7_to_3v0,
    .ocp          = k_ra8_vreg_ocp_off,
    .fast_startup = false,
    .ldo_boost    = false,
    .lv_profile   = k_ra8_vreg_lv_p0,
  };
  return cfg;
}

static uint32_t s_vreg_cb_count;
static uint8_t  s_vreg_cb_last_word;

static void stub_vreg_cb(void* ctx, uint8_t word)
{
  (void)ctx;
  ++s_vreg_cb_count;
  s_vreg_cb_last_word = word;
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  s_vreg_cb_count     = 0U;
  s_vreg_cb_last_word = 0U;
  /* Also detach any handler left over from a previous test. */
  (void)ra8_vreg_attach_handler(nullptr, nullptr);
  /* Bring the cached state back to "uninitialized" between tests so
   * each case observes the deinit path independently. */
  (void)ra8_vreg_deinit();
}

/* ---------------------------------------------------------------------------
 * Lifecycle / init validation
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_init_happy_dcdc(void)
{
  TEST_BEGIN("vreg init dcdc happy");
  prep();

  const ra8_vreg_cfg_t cfg = make_cfg_dcdc();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));

  /* DCDCCTL should have DCDCON | OCPEN | STOPZA = 0x13. */
  const uint8_t expected =
    (uint8_t)((uint8_t)k_ra8_vreg_mask_dcdcon | (uint8_t)k_ra8_vreg_mask_ocpen |
              (uint8_t)k_ra8_vreg_mask_stopza);
  TEST_ASSERT_EQ(expected, *ra8_vreg_dcdcctl());
  TEST_ASSERT_EQ(k_ra8_vreg_vccsel_3v0_to_3v6, *ra8_vreg_vccsel());
  TEST_END("vreg init dcdc happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy_dcdc_fast(void)
{
  TEST_BEGIN("vreg init dcdc fast-startup");
  prep();

  const ra8_vreg_cfg_t cfg = make_cfg_dcdc_fast();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));

  /* Fast path -> DCDCCTL should be 0x53 (STOPZA + DCDCON + OCPEN + FST). */
  TEST_ASSERT_EQ(k_ra8_vreg_dcdc_step_fast_on, *ra8_vreg_dcdcctl());
  TEST_END("vreg init dcdc fast-startup");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy_ldo(void)
{
  TEST_BEGIN("vreg init ldo happy");
  prep();

  const ra8_vreg_cfg_t cfg = make_cfg_ldo();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));

  /* LDO mode -> DCDCCTL should have DCDCON cleared. */
  TEST_ASSERT_EQ(0, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_dcdcon));
  TEST_ASSERT_EQ(k_ra8_vreg_mask_lvo0e, (*ra8_vreg_lvocr() & (uint8_t)k_ra8_vreg_mask_lvo0e));
  TEST_END("vreg init ldo happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy_ldo_with_boost(void)
{
  TEST_BEGIN("vreg init ldo with LCBOOST");
  prep();

  ra8_vreg_cfg_t cfg = make_cfg_ldo();
  cfg.ldo_boost      = true;
  cfg.lv_profile     = k_ra8_vreg_lv_off;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_vreg_mask_lcboost, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_lcboost));
  TEST_END("vreg init ldo with LCBOOST");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("vreg init null cfg");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vreg_init(nullptr));
  TEST_END("vreg init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_vccsel(void)
{
  TEST_BEGIN("vreg init bad vccsel");
  prep();

  ra8_vreg_cfg_t cfg = make_cfg_dcdc();
  cfg.vccsel         = (ra8_vreg_vccsel_t)k_t_vccsel_over; /* Out of 0..2 range. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vreg_init(&cfg));
  TEST_END("vreg init bad vccsel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_mode(void)
{
  TEST_BEGIN("vreg init bad mode");
  prep();

  ra8_vreg_cfg_t cfg = make_cfg_dcdc();
  cfg.mode           = (ra8_vreg_mode_t)k_t_mode_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vreg_init(&cfg));
  TEST_END("vreg init bad mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_ocp(void)
{
  TEST_BEGIN("vreg init bad ocp");
  prep();

  ra8_vreg_cfg_t cfg = make_cfg_dcdc();
  cfg.ocp            = (ra8_vreg_ocp_t)k_t_ocp_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vreg_init(&cfg));
  TEST_END("vreg init bad ocp");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_lv_profile(void)
{
  TEST_BEGIN("vreg init bad lv profile");
  prep();

  ra8_vreg_cfg_t cfg = make_cfg_ldo();
  cfg.lv_profile     = (ra8_vreg_lv_profile_t)k_t_ocp_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vreg_init(&cfg));
  TEST_END("vreg init bad lv profile");
}

/* ---------------------------------------------------------------------------
 * Mode + VCCSEL + OCP + LDO boost + Fast-startup setters
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_set_mode_round_trip(void)
{
  TEST_BEGIN("vreg set_mode round trip");
  prep();

  /* Start from the LDO config so we can transition LDO -> DCDC -> LDO. */
  const ra8_vreg_cfg_t cfg = make_cfg_ldo();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));
  TEST_ASSERT_EQ(0, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_dcdcon));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_mode(k_ra8_vreg_mode_dcdc));
  /* After the FSP-style 5-step sequence DCDCCTL = STOPZA | DCDCON | OCPEN = 0x13. */
  const uint8_t expected_on =
    (uint8_t)((uint8_t)k_ra8_vreg_mask_stopza | (uint8_t)k_ra8_vreg_mask_dcdcon |
              (uint8_t)k_ra8_vreg_mask_ocpen);
  TEST_ASSERT_EQ(expected_on, *ra8_vreg_dcdcctl());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_mode(k_ra8_vreg_mode_ldo));
  TEST_ASSERT_EQ(0, *ra8_vreg_dcdcctl());
  TEST_END("vreg set_mode round trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_mode_keeps_lcboost_on_disable(void)
{
  TEST_BEGIN("vreg set_mode preserves LCBOOST during DCDC->LDO");
  prep();

  ra8_vreg_cfg_t cfg = make_cfg_ldo();
  cfg.ldo_boost      = true;
  cfg.lv_profile     = k_ra8_vreg_lv_off;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));

  /* LDO -> DCDC -> LDO with cached LCBOOST should leave LCBOOST set. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_mode(k_ra8_vreg_mode_dcdc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_mode(k_ra8_vreg_mode_ldo));
  TEST_ASSERT_EQ(k_ra8_vreg_mask_lcboost, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_lcboost));
  TEST_END("vreg set_mode preserves LCBOOST during DCDC->LDO");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_mode_bad_arg(void)
{
  TEST_BEGIN("vreg set_mode bad arg");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vreg_set_mode((ra8_vreg_mode_t)0xAAU));
  TEST_END("vreg set_mode bad arg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_vccsel(void)
{
  TEST_BEGIN("vreg set_vccsel");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_vccsel(k_ra8_vreg_vccsel_2v4_to_2v7));
  TEST_ASSERT_EQ(k_ra8_vreg_vccsel_2v4_to_2v7, *ra8_vreg_vccsel());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_vccsel(k_ra8_vreg_vccsel_2v7_to_3v0));
  TEST_ASSERT_EQ(k_ra8_vreg_vccsel_2v7_to_3v0, *ra8_vreg_vccsel());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_vccsel(k_ra8_vreg_vccsel_3v0_to_3v6));
  TEST_ASSERT_EQ(k_ra8_vreg_vccsel_3v0_to_3v6, *ra8_vreg_vccsel());

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vreg_set_vccsel((ra8_vreg_vccsel_t)0x10U));
  TEST_END("vreg set_vccsel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_ocp_levels(void)
{
  TEST_BEGIN("vreg set_ocp every level");
  prep();

  /* Start from a DCDC init so OCPEN starts asserted. */
  const ra8_vreg_cfg_t cfg = make_cfg_dcdc();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));

  /* off should clear OCPEN. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_ocp(k_ra8_vreg_ocp_off));
  TEST_ASSERT_EQ(0, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_ocpen));

  /* normal/low/high should all set OCPEN. */
  for (uint8_t lvl = (uint8_t)k_ra8_vreg_ocp_normal; lvl <= (uint8_t)k_ra8_vreg_ocp_high; ++lvl) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_ocp((ra8_vreg_ocp_t)lvl));
    TEST_ASSERT_EQ(k_ra8_vreg_mask_ocpen, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_ocpen));
  }

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vreg_set_ocp((ra8_vreg_ocp_t)0xAAU));
  TEST_END("vreg set_ocp every level");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_fast_startup(void)
{
  TEST_BEGIN("vreg set_fast_startup");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_fast_startup(true));
  TEST_ASSERT_EQ(k_ra8_vreg_mask_fst, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_fst));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_fast_startup(false));
  TEST_ASSERT_EQ(0, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_fst));
  TEST_END("vreg set_fast_startup");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_ldo_boost(void)
{
  TEST_BEGIN("vreg set_ldo_boost");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_ldo_boost(true));
  TEST_ASSERT_EQ(k_ra8_vreg_mask_lcboost, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_lcboost));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_ldo_boost(false));
  TEST_ASSERT_EQ(0, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_lcboost));
  TEST_END("vreg set_ldo_boost");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_lv_profile(void)
{
  TEST_BEGIN("vreg set_lv_profile mutual exclusion");
  prep();

  /* off -> 0 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_lv_profile(k_ra8_vreg_lv_off));
  TEST_ASSERT_EQ(0, *ra8_vreg_lvocr());

  /* P0 -> only LVO0E. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_lv_profile(k_ra8_vreg_lv_p0));
  TEST_ASSERT_EQ(k_ra8_vreg_mask_lvo0e, *ra8_vreg_lvocr());

  /* P1 -> only LVO1E. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_lv_profile(k_ra8_vreg_lv_p1));
  TEST_ASSERT_EQ(k_ra8_vreg_mask_lvo1e, *ra8_vreg_lvocr());

  /* Bad enum -> invalid arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vreg_set_lv_profile((ra8_vreg_lv_profile_t)0x55U));
  TEST_END("vreg set_lv_profile mutual exclusion");
}

/* ---------------------------------------------------------------------------
 * Status snapshot + clear_status
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_get_status_decoded(void)
{
  TEST_BEGIN("vreg get_status decoded");
  prep();

  const ra8_vreg_cfg_t cfg = make_cfg_dcdc();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));

  ra8_vreg_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_get_status(&st));
  TEST_ASSERT_EQ(k_ra8_vreg_mode_dcdc, st.mode);
  TEST_ASSERT_EQ(k_ra8_vreg_vccsel_3v0_to_3v6, st.vccsel_dec);
  TEST_ASSERT_EQ(k_ra8_vreg_lv_off, st.lv_profile);
  TEST_ASSERT_EQ(k_ra8_vreg_ocp_normal, st.ocp);
  TEST_ASSERT(st.dcdc_ready);
  TEST_ASSERT(st.io_buf_on);
  TEST_ASSERT(!st.fast_startup);
  TEST_ASSERT(!st.ldo_boost);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vreg_get_status(nullptr));
  TEST_END("vreg get_status decoded");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status_ldo(void)
{
  TEST_BEGIN("vreg get_status decoded (LDO + LV)");
  prep();

  const ra8_vreg_cfg_t cfg = make_cfg_ldo();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));

  ra8_vreg_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_get_status(&st));
  TEST_ASSERT_EQ(k_ra8_vreg_mode_ldo, st.mode);
  TEST_ASSERT_EQ(k_ra8_vreg_lv_p0, st.lv_profile);
  TEST_ASSERT_EQ(k_ra8_vreg_ocp_off, st.ocp);
  TEST_ASSERT(!st.dcdc_ready);
  TEST_END("vreg get_status decoded (LDO + LV)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status(void)
{
  TEST_BEGIN("vreg clear_status");
  prep();

  const ra8_vreg_cfg_t cfg = make_cfg_dcdc();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));

  /* Clear the OCPEN bit, leave DCDCON intact. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_clear_status((uint8_t)k_ra8_vreg_mask_ocpen));
  TEST_ASSERT_EQ(0, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_ocpen));
  TEST_ASSERT_EQ(k_ra8_vreg_mask_dcdcon, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_dcdcon));
  TEST_END("vreg clear_status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_reserved_bits(void)
{
  TEST_BEGIN("vreg clear_status rejects reserved bits");
  prep();

  /* Bits 2 and 3 are reserved -- attempting to "clear" them returns
   * invalid_arg without touching the register. */
  const uint8_t before = *ra8_vreg_dcdcctl();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vreg_clear_status((uint8_t)0x0CU));
  TEST_ASSERT_EQ(before, *ra8_vreg_dcdcctl());
  TEST_END("vreg clear_status rejects reserved bits");
}

/* ---------------------------------------------------------------------------
 * Standby / stop transitions
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_enter_exit_stop_legacy(void)
{
  TEST_BEGIN("vreg enter/exit stop (legacy aliases)");
  prep();

  const ra8_vreg_cfg_t cfg = make_cfg_dcdc();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));
  const uint8_t before = *ra8_vreg_dcdcctl();
  TEST_ASSERT(before != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_enter_stop());
  TEST_ASSERT_EQ(0, *ra8_vreg_dcdcctl());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_exit_stop());
  TEST_ASSERT_EQ(before, *ra8_vreg_dcdcctl());
  TEST_END("vreg enter/exit stop (legacy aliases)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enter_standby_every_variant(void)
{
  TEST_BEGIN("vreg enter_standby every variant");
  prep();

  for (uint8_t v = (uint8_t)k_ra8_vreg_standby_software;
       v <= (uint8_t)k_ra8_vreg_standby_battery_backup;
       ++v) {
    const ra8_vreg_cfg_t cfg = make_cfg_dcdc();
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_enter_standby((ra8_vreg_standby_t)v));
    TEST_ASSERT_EQ(0, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_dcdcon));
    TEST_ASSERT_EQ(0, (*ra8_vreg_dcdcctl() & (uint8_t)k_ra8_vreg_mask_ocpen));
  }
  TEST_END("vreg enter_standby every variant");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enter_standby_bad_variant(void)
{
  TEST_BEGIN("vreg enter_standby bad variant");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vreg_enter_standby((ra8_vreg_standby_t)0x55U));
  TEST_END("vreg enter_standby bad variant");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_exit_stop_uninitialized(void)
{
  TEST_BEGIN("vreg exit stop uninitialized");
  prep();

  /* deinit clears the cached state. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_deinit());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_exit_stop());
  /* No restore should have occurred. */
  TEST_ASSERT_EQ(0, *ra8_vreg_dcdcctl());
  TEST_END("vreg exit stop uninitialized");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset_clears_cached_state(void)
{
  TEST_BEGIN("vreg reset clears cached state");
  prep();

  const ra8_vreg_cfg_t cfg = make_cfg_dcdc();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_reset());

  /* Registers should be back at reset and exit_stop should be a no-op. */
  TEST_ASSERT_EQ(0, *ra8_vreg_dcdcctl());
  TEST_ASSERT_EQ(0, *ra8_vreg_vccsel());
  TEST_ASSERT_EQ(0, *ra8_vreg_lvocr());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_exit_stop());
  TEST_ASSERT_EQ(0, *ra8_vreg_dcdcctl());
  TEST_END("vreg reset clears cached state");
}

/* ---------------------------------------------------------------------------
 * IRQ hook
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_attach_dispatch(void)
{
  TEST_BEGIN("vreg attach + dispatch");
  prep();

  /* Set DCDCCTL to a known marker value via init. */
  const ra8_vreg_cfg_t cfg = make_cfg_dcdc();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));
  const uint8_t live = *ra8_vreg_dcdcctl();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_attach_handler(stub_vreg_cb, (void*)(uintptr_t)0xCAFEU));
  ra8_vreg_dispatch();
  TEST_ASSERT_EQ(1, s_vreg_cb_count);
  TEST_ASSERT_EQ(live, s_vreg_cb_last_word);

  /* Detach via nullptr -- subsequent dispatch is a no-op. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_attach_handler(nullptr, nullptr));
  ra8_vreg_dispatch();
  TEST_ASSERT_EQ(1, s_vreg_cb_count);
  TEST_END("vreg attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_clears_regs(void)
{
  TEST_BEGIN("vreg deinit clears regs");
  prep();

  const ra8_vreg_cfg_t cfg = make_cfg_dcdc();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_deinit());

  TEST_ASSERT_EQ(0, *ra8_vreg_dcdcctl());
  TEST_ASSERT_EQ(0, *ra8_vreg_vccsel());
  TEST_ASSERT_EQ(0, *ra8_vreg_lvocr());
  TEST_END("vreg deinit clears regs");
}

/**
 * @test test_mcdc_ra8_vreg
 *
 * @par MC/DC:
 * Decision A: ``ra8_vreg_set_mode`` line 365,
 * libs/ra8_hal/src/ra8_vreg.c:
 * ``if ((mode != k_ra8_vreg_mode_ldo) && (mode != k_ra8_vreg_mode_dcdc))``
 * (2 conditions, ``&&``). N+1 = 3:
 * - V1: mode=ldo  -> C1=F (short)        -> dec F (accept)
 * - V2: mode=dcdc -> C1=T,C2=F           -> dec F (accept)
 * - V3: mode=99   -> C1=T,C2=T           -> dec T (reject)
 *
 * Decision B: ``ra8_vreg_get_status`` line 492,
 * ``if (dcdcon_set && pd_clear)`` (2 conditions, ``&&``). N+1 = 3:
 * - V1: dcdcon=F          -> dec F (false)
 * - V2: dcdcon=T, pd=set  -> dec F (false)
 * - V3: dcdcon=T, pd=clr  -> dec T (true)
 * DO-178C 6.4.4.3 met.
 */
static void test_mcdc_ra8_vreg(void)
{
  TEST_BEGIN("vreg MC/DC: set_mode + get_status 2-cond decisions");
  ra8_fake_mmap_reset();
  const ra8_vreg_cfg_t cfg = make_cfg_dcdc();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_mode(k_ra8_vreg_mode_ldo));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_set_mode(k_ra8_vreg_mode_dcdc));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vreg_set_mode((ra8_vreg_mode_t)99U));
  ra8_vreg_status_t out = {};
  *ra8_vreg_dcdcctl()   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_get_status(&out));
  TEST_ASSERT(out.dcdc_ready == false);
  *ra8_vreg_dcdcctl() = (uint8_t)((uint8_t)k_ra8_vreg_mask_dcdcon | (uint8_t)k_ra8_vreg_mask_pd);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_get_status(&out));
  TEST_ASSERT(out.dcdc_ready == false);
  *ra8_vreg_dcdcctl() = (uint8_t)k_ra8_vreg_mask_dcdcon;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vreg_get_status(&out));
  TEST_ASSERT(out.dcdc_ready == true);
  TEST_END("vreg MC/DC: set_mode + get_status 2-cond decisions");
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
  test_init_happy_dcdc,
  test_init_happy_dcdc_fast,
  test_init_happy_ldo,
  test_init_happy_ldo_with_boost,
  test_init_null_cfg,
  test_init_bad_vccsel,
  test_init_bad_mode,
  test_init_bad_ocp,
  test_init_bad_lv_profile,
  test_set_mode_round_trip,
  test_set_mode_keeps_lcboost_on_disable,
  test_set_mode_bad_arg,
  test_set_vccsel,
  test_set_ocp_levels,
  test_set_fast_startup,
  test_set_ldo_boost,
  test_set_lv_profile,
  test_get_status_decoded,
  test_get_status_ldo,
  test_clear_status,
  test_clear_status_reserved_bits,
  test_enter_exit_stop_legacy,
  test_enter_standby_every_variant,
  test_enter_standby_bad_variant,
  test_exit_stop_uninitialized,
  test_reset_clears_cached_state,
  test_attach_dispatch,
  test_deinit_clears_regs,
  test_mcdc_ra8_vreg,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
