/**
 * @file test_ra8_bkup.c
 * @brief Unit tests for ra8_bkup.c (Battery Backup Function driver)
 *
 * @details
 * This sibling owns the init / deinit, cold-start / warm-start / no-switch,
 * and status contract tests. The VBTBKR access, tamper, voltage monitor,
 * security, IRQ, VDET sweep, and MC/DC vector tests live in
 * test_ra8_bkup_tamper.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_bkup.h"
#include "ra8_bkup_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra8_bkup_test_const_t
 * @brief Local numeric constants for the tests.
 */
typedef enum : uint32_t {
  k_ra8_bkup_test_word_first   = 0U,      /**< First VBTBKR word slot.       */
  k_ra8_bkup_test_word_mid     = 7U,      /**< Mid-array word slot.          */
  k_ra8_bkup_test_word_last    = 31U,     /**< Last legal VBTBKR word slot.  */
  k_ra8_bkup_test_word_bad     = 32U,     /**< First invalid word slot.      */
  k_ra8_bkup_test_word_huge    = 200U,    /**< Way out of range.             */
  k_ra8_bkup_test_bad_level    = 6U,      /**< 110b -- "setting prohibited". */
  k_ra8_bkup_test_byte_first   = 0U,      /**< First VBTBKR byte slot.       */
  k_ra8_bkup_test_byte_mid     = 64U,     /**< Mid-array byte slot.          */
  k_ra8_bkup_test_byte_last    = 127U,    /**< Last legal byte slot.         */
  k_ra8_bkup_test_byte_bad     = 128U,    /**< First invalid byte slot.      */
  k_ra8_bkup_test_byte_huge    = 999U,    /**< Way out of range.             */
  k_ra8_bkup_test_chan_bad     = 3U,      /**< First invalid channel index.  */
  k_ra8_bkup_test_nc_width_bad = 8U,      /**< First invalid VINCW encoding. */
  k_ra8_bkup_test_timeout      = 32U,     /**< Reasonable poll budget.       */
  k_ra8_bkup_test_saba_aligned = 0x40U,   /**< 32-byte aligned (low bits 0). */
  k_ra8_bkup_test_saba_unalign = 0x41U,   /**< Unaligned -- bit 0 set.       */
  k_ra8_bkup_test_saba_max_ok  = 0xFFE0U, /**< saba_max boundary.            */
} ra8_bkup_test_const_t;

/**
 * @enum ra8_bkup_test_words_t
 * @brief 32-bit constants used by the read/write tests.
 */
typedef enum : uint32_t {
  k_ra8_bkup_test_pattern_a    = 0xDEADBEEFU, /**< Marker pattern A.                      */
  k_ra8_bkup_test_pattern_b    = 0x12345678U, /**< Marker pattern B.                      */
  k_ra8_bkup_test_bbfsar_value = 0x53UL,      /**< NONSEC0 + NONSEC1 + NONSEC4 + NONSEC6. */
  k_ra8_bkup_test_bbfsar_bad   = 0x80UL,      /**< Bit 7 reserved.                        */
} ra8_bkup_test_words_t;

/**
 * @enum ra8_bkup_test_byte_t
 * @brief 8-bit byte test patterns.
 */
typedef enum : uint8_t {
  k_ra8_bkup_test_byte_pat_a = 0x5AU, /**< Byte marker A. */
  k_ra8_bkup_test_byte_pat_b = 0xA5U, /**< Byte marker B. */
} ra8_bkup_test_byte_t;

static ra8_bkup_config_t make_cfg(void)
{
  const ra8_bkup_config_t cfg = {
    .vdet_level    = k_ra8_bkup_vdet_2p10v,
    .enable_switch = true,
    .enable_backup = true,
  };
  return cfg;
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  /* Detach any leftover handler from a previous test. */
  (void)ra8_bkup_attach_handler(nullptr, nullptr);
}

/* ---------------- init / deinit ----------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("bkup init happy");
  prep();

  const ra8_bkup_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_init(&cfg));

  /* HUM Ch 12.2.11 "VBTBPCR1 : VBATT Battery Power Supply Control Register 1", p 507 */
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtbpcr1());
  /* HUM Ch 12.2.12 "VBTBPCR2 : VBATT Battery Power Supply Control Register 2", p 508 */
  const uint8_t bpcr2 = *ra8_bkup_vbtbpcr2();
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_vdet_2p10v), (bpcr2 & (uint8_t)k_ra8_bkup_vbtbpcr2_mask_lvl));
  TEST_ASSERT((bpcr2 & (uint8_t)k_ra8_bkup_vbtbpcr2_mask_vdete) != 0U);
  /* HUM Ch 12.2.6 "VBTBER : VBATT Backup Enable Register", p 504 */
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_vbtber_mask_vbae), *ra8_bkup_vbtber());
  /* Tamper flags wiped during init. */
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtadsr());
  TEST_END("bkup init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("bkup init null cfg");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_bkup_init(nullptr));
  TEST_END("bkup init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_vdet_level(void)
{
  TEST_BEGIN("bkup init bad vdet level");
  prep();

  ra8_bkup_config_t cfg = make_cfg();
  cfg.vdet_level        = (ra8_bkup_vdet_level_t)k_ra8_bkup_test_bad_level;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_init(&cfg));
  TEST_END("bkup init bad vdet level");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_switch_disabled(void)
{
  TEST_BEGIN("bkup init switch disabled");
  prep();

  ra8_bkup_config_t cfg = make_cfg();
  cfg.enable_switch     = false;
  cfg.enable_backup     = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_init(&cfg));

  /* HUM Ch 12.2.11 "VBTBPCR1 : VBATT Battery Power Supply Control Register 1", p 507 */
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_vbtbpcr1_mask_bpwswstp), *ra8_bkup_vbtbpcr1());
  /* HUM Ch 12.2.6 "VBTBER : VBATT Backup Enable Register", p 504 */
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtber());
  TEST_END("bkup init switch disabled");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("bkup deinit");
  prep();

  const ra8_bkup_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_deinit());

  TEST_ASSERT_EQ(0, *ra8_bkup_vbtber());
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_vbtbpcr1_mask_bpwswstp), *ra8_bkup_vbtbpcr1());
  TEST_END("bkup deinit");
}

/* ---------------- cold / warm / no-switch ----------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_cold_start_init_happy(void)
{
  TEST_BEGIN("bkup cold_start_init happy");
  prep();
  /* Pre-set VBPORM=1 so the wait loop falls through immediately. */
  *ra8_bkup_vbtbpsr() =
    (uint8_t)((uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporm | (uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporf);

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_bkup_cold_start_init(k_ra8_bkup_vdet_1p95v, (uint32_t)k_ra8_bkup_test_timeout));

  /* HUM Ch 12.2.11 "VBTBPCR1 : VBATT Battery Power Supply Control Register 1", p 507 */
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtbpcr1());
  /* HUM Ch 12.2.12 "VBTBPCR2 : VBATT Battery Power Supply Control Register 2", p 508 */
  const uint8_t bpcr2 = *ra8_bkup_vbtbpcr2();
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_vdet_1p95v), (bpcr2 & (uint8_t)k_ra8_bkup_vbtbpcr2_mask_lvl));
  TEST_ASSERT((bpcr2 & (uint8_t)k_ra8_bkup_vbtbpcr2_mask_vdete) != 0U);
  /* VBPORF was W0Ced -- VBPORM stays. */
  TEST_ASSERT((*ra8_bkup_vbtbpsr() & (uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporf) == 0U);
  TEST_ASSERT((*ra8_bkup_vbtbpsr() & (uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporm) != 0U);
  TEST_END("bkup cold_start_init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_cold_start_init_bad_args(void)
{
  TEST_BEGIN("bkup cold_start_init bad args");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_bkup_cold_start_init((ra8_bkup_vdet_level_t)k_ra8_bkup_test_bad_level,
                                          (uint32_t)k_ra8_bkup_test_timeout));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_cold_start_init(k_ra8_bkup_vdet_2p80v, 0U));
  TEST_END("bkup cold_start_init bad args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_cold_start_init_timeout(void)
{
  TEST_BEGIN("bkup cold_start_init timeout");
  prep();
  /* VBPORM left at 0 -> poll loop must time out. */
  *ra8_bkup_vbtbpsr() = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_bkup_cold_start_init(k_ra8_bkup_vdet_2p80v, 4U));
  TEST_END("bkup cold_start_init timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_warm_start_no_reinit(void)
{
  TEST_BEGIN("bkup warm_start_check no reinit");
  prep();
  /* VBPORM=1, VBPORF=0 -> survived. */
  *ra8_bkup_vbtbpsr() = (uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporm;

  bool needs = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_warm_start_check(&needs, (uint32_t)k_ra8_bkup_test_timeout));
  TEST_ASSERT(!needs);
  TEST_END("bkup warm_start_check no reinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_warm_start_needs_reinit(void)
{
  TEST_BEGIN("bkup warm_start_check needs reinit");
  prep();
  /* VBPORM=1, VBPORF=1 -> reinit needed. */
  *ra8_bkup_vbtbpsr() =
    (uint8_t)((uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporm | (uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporf);

  bool needs = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_warm_start_check(&needs, (uint32_t)k_ra8_bkup_test_timeout));
  TEST_ASSERT(needs);
  /* The check must NOT W0C VBPORF -- caller decides. */
  TEST_ASSERT((*ra8_bkup_vbtbpsr() & (uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporf) != 0U);
  TEST_END("bkup warm_start_check needs reinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_warm_start_bad_args(void)
{
  TEST_BEGIN("bkup warm_start_check bad args");
  prep();

  bool needs = false;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_bkup_warm_start_check(nullptr, (uint32_t)k_ra8_bkup_test_timeout));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_warm_start_check(&needs, 0U));
  *ra8_bkup_vbtbpsr() = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_bkup_warm_start_check(&needs, 4U));
  TEST_END("bkup warm_start_check bad args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_no_switch_init(void)
{
  TEST_BEGIN("bkup no_switch_init");
  prep();
  /* VBPORM=0 already so the wait loop succeeds. */
  *ra8_bkup_vbtbpsr() = (uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporf; /* set VBPORF */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_no_switch_init((uint32_t)k_ra8_bkup_test_timeout));

  /* HUM Ch 12.2.11 "VBTBPCR1 : VBATT Battery Power Supply Control Register 1", p 507 */
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_vbtbpcr1_mask_bpwswstp), *ra8_bkup_vbtbpcr1());
  /* HUM Ch 12.2.12 "VBTBPCR2 : VBATT Battery Power Supply Control Register 2", p 508 */
  TEST_ASSERT_EQ(0x06U, *ra8_bkup_vbtbpcr2());
  /* HUM Ch 12.2.13 "VBTBPSR : VBATT Battery Power Supply Status Register", p 509 */
  TEST_ASSERT((*ra8_bkup_vbtbpsr() & (uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporf) == 0U);
  /* tamper-side regs zeroed */
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtictlr());
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtictlr2());
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtadsr());
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtadcr1());
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtadcr2());
  TEST_END("bkup no_switch_init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_no_switch_init_bad_args(void)
{
  TEST_BEGIN("bkup no_switch_init bad args");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_no_switch_init(0U));
  /* VBPORM stuck at 1 -> timeout. */
  *ra8_bkup_vbtbpsr() = (uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporm;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_bkup_no_switch_init(4U));
  TEST_END("bkup no_switch_init bad args");
}

/* ---------------- get / clear status ----------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status_vcc(void)
{
  TEST_BEGIN("bkup get_status on VCC");
  prep();

  /* Fake hardware indicating VCC > VDETBATT and VBATT_R OK. */
  *ra8_bkup_vbtbpsr() =
    (uint8_t)((uint8_t)k_ra8_bkup_vbtbpsr_mask_swm | (uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporm);
  *ra8_bkup_vbtadsr() = 0U;

  ra8_bkup_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_get_status(&st));
  TEST_ASSERT_EQ(k_ra8_bkup_source_vcc, st.source);
  TEST_ASSERT(st.vbatt_r_ok);
  TEST_ASSERT(!st.por_detected);
  TEST_ASSERT_EQ(0, st.tamper_flags);
  TEST_END("bkup get_status on VCC");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status_vbatt_with_por(void)
{
  TEST_BEGIN("bkup get_status on VBATT with POR");
  prep();

  /* SWM=0 means we're on VBATT, VBPORF latched. */
  *ra8_bkup_vbtbpsr() = (uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporf;
  *ra8_bkup_vbtadsr() = (uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf1;

  ra8_bkup_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_get_status(&st));
  TEST_ASSERT_EQ(k_ra8_bkup_source_vbatt, st.source);
  TEST_ASSERT(!st.vbatt_r_ok);
  TEST_ASSERT(st.por_detected);
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf1), st.tamper_flags);
  TEST_END("bkup get_status on VBATT with POR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status_null(void)
{
  TEST_BEGIN("bkup get_status null");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_bkup_get_status(nullptr));
  TEST_END("bkup get_status null");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status(void)
{
  TEST_BEGIN("bkup clear_status");
  prep();

  *ra8_bkup_vbtbpsr() =
    (uint8_t)((uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporf | (uint8_t)k_ra8_bkup_vbtbpsr_mask_swm);
  *ra8_bkup_vbtadsr() = (uint8_t)k_ra8_bkup_vbtadsr_mask_all;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_bkup_clear_status((uint8_t)((uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporf |
                                                 (uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf0)));

  /* VBPORF cleared, SWM preserved. */
  const uint8_t bpsr_after = *ra8_bkup_vbtbpsr();
  TEST_ASSERT((bpsr_after & (uint8_t)k_ra8_bkup_vbtbpsr_mask_vbporf) == 0U);
  TEST_ASSERT((bpsr_after & (uint8_t)k_ra8_bkup_vbtbpsr_mask_swm) != 0U);
  /* Only VBTADF0 cleared. */
  const uint8_t adsr_after = *ra8_bkup_vbtadsr();
  TEST_ASSERT((adsr_after & (uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf0) == 0U);
  TEST_ASSERT((adsr_after & (uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf1) != 0U);
  TEST_ASSERT((adsr_after & (uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf2) != 0U);
  TEST_END("bkup clear_status");
}

int main(void)
{
  test_init_happy();
  test_init_null_cfg();
  test_init_bad_vdet_level();
  test_init_switch_disabled();
  test_deinit();
  test_cold_start_init_happy();
  test_cold_start_init_bad_args();
  test_cold_start_init_timeout();
  test_warm_start_no_reinit();
  test_warm_start_needs_reinit();
  test_warm_start_bad_args();
  test_no_switch_init();
  test_no_switch_init_bad_args();
  test_get_status_vcc();
  test_get_status_vbatt_with_por();
  test_get_status_null();
  test_clear_status();
  return 0;
}
