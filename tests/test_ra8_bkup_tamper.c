/**
 * @file test_ra8_bkup_tamper.c
 * @brief Backup-register and tamper tests for ra8_bkup.c
 *
 * @details
 * Split out of test_ra8_bkup.c to keep each test translation unit under the
 * repository file-size cap. This sibling owns the VBTBKR word / byte access,
 * tamper init / disable / input, voltage monitor, security partitioning,
 * IRQ path, VDET-level sweep, and MC/DC vector tests; the init / cold-start /
 * warm-start / status contract tests stay in test_ra8_bkup.c.
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
 * @enum bkup_tamper_fixture_t
 * @brief Out-of-range and malformed inputs the code under test must reject.
 */
typedef enum : uint8_t {
  k_bkup_field_invalid =
    0xFFU, /**< A value outside every enumeration it is assigned to; each guard must reject it. */
} bkup_tamper_fixture_t;

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

static uint32_t s_bkup_cb_count;
static uint8_t  s_bkup_cb_last_flags;
static void*    s_bkup_cb_last_ctx;

static void stub_bkup_cb(void* ctx, uint8_t tamper_flags)
{
  ++s_bkup_cb_count;
  s_bkup_cb_last_flags = tamper_flags;
  s_bkup_cb_last_ctx   = ctx;
}

static ra8_bkup_config_t make_cfg(void)
{
  const ra8_bkup_config_t cfg = {
    .vdet_level    = k_ra8_bkup_vdet_2p10v,
    .enable_switch = true,
    .enable_backup = true,
  };
  return cfg;
}

static ra8_bkup_tamper_chan_cfg_t
make_chan_cfg(bool ien, bool ie, bool ce, bool ze, ra8_bkup_edge_t edge)
{
  const ra8_bkup_tamper_chan_cfg_t cfg = {
    .input_enable       = ien,
    .noise_canceller_en = true,
    .edge               = edge,
    .irq_enable         = ie,
    .clear_backup       = ce,
    .zeroize_huk        = ze,
    .capture_src        = k_ra8_bkup_capture_src_vbtadf,
  };
  return cfg;
}

static ra8_bkup_tamper_config_t make_tamper_cfg(void)
{
  const ra8_bkup_tamper_config_t cfg = {
    .nc_width = k_ra8_bkup_nc_width_64hz,
    .channels =
      {
        make_chan_cfg(true, true, true, false, k_ra8_bkup_edge_falling),
        make_chan_cfg(true, true, false, true, k_ra8_bkup_edge_rising),
        make_chan_cfg(false, false, true, true, k_ra8_bkup_edge_falling),
      },
  };
  return cfg;
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  s_bkup_cb_count      = 0U;
  s_bkup_cb_last_flags = 0U;
  s_bkup_cb_last_ctx   = nullptr;
  /* Detach any leftover handler from a previous test. */
  (void)ra8_bkup_attach_handler(nullptr, nullptr);
}

/* ---------------- VBTBKR access ----------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_word_read_write_roundtrip(void)
{
  TEST_BEGIN("bkup word read/write roundtrip");
  prep();

  const ra8_bkup_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_init(&cfg));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_bkup_write_word((uint8_t)k_ra8_bkup_test_word_first, (uint32_t)k_ra8_bkup_test_pattern_a));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_bkup_write_word((uint8_t)k_ra8_bkup_test_word_mid, (uint32_t)k_ra8_bkup_test_pattern_b));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_bkup_write_word((uint8_t)k_ra8_bkup_test_word_last, (uint32_t)k_ra8_bkup_test_pattern_a));

  uint32_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_read_word((uint8_t)k_ra8_bkup_test_word_first, &v));
  TEST_ASSERT_EQ(k_ra8_bkup_test_pattern_a, v);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_read_word((uint8_t)k_ra8_bkup_test_word_mid, &v));
  TEST_ASSERT_EQ(k_ra8_bkup_test_pattern_b, v);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_read_word((uint8_t)k_ra8_bkup_test_word_last, &v));
  TEST_ASSERT_EQ(k_ra8_bkup_test_pattern_a, v);
  TEST_END("bkup word read/write roundtrip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_word_bad_args(void)
{
  TEST_BEGIN("bkup word bad args");
  prep();

  uint32_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_read_word((uint8_t)k_ra8_bkup_test_word_bad, &v));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_read_word((uint8_t)k_ra8_bkup_test_word_huge, &v));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_bkup_read_word((uint8_t)k_ra8_bkup_test_word_first, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_bkup_write_word((uint8_t)k_ra8_bkup_test_word_bad, (uint32_t)k_ra8_bkup_test_pattern_a));
  TEST_END("bkup word bad args");
}

/**
 * @brief Regression for #131: ra8_bkup_init must arm VBTBER.VBAE before access.
 *
 * @details
 * The silicon RW window failed because the bkup_survival_demo touched
 * VBTBKRn without ever calling ra8_bkup_init, so VBTBER.VBAE was never
 * written to 1. HUM Ch 12.2.6 p 504 requires "You must write 1 to VBAE
 * before accessing VBTBKR". This pins the driver contract: init with
 * ``enable_backup == true`` sets VBAE (arming the window, plus the
 * >= 500 ns settle inside the driver), init with ``enable_backup == false``
 * leaves it 0 (window closed). The host MMIO map does not model the gate,
 * so silicon + ra8_emulator prove the drop; this pins the register write.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_word_access_requires_vbae_arm(void)
{
  TEST_BEGIN("bkup #131: init arms VBTBER.VBAE for VBTBKRn access");
  prep();

  /* enable_backup == true -> VBAE armed, window open. */
  ra8_bkup_config_t cfg = make_cfg();
  cfg.enable_backup     = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_init(&cfg));
  /* HUM Ch 12.2.6 "VBTBER : VBATT Backup Enable Register", p 504 */
  TEST_ASSERT((*ra8_bkup_vbtber() & (uint8_t)k_ra8_bkup_vbtber_mask_vbae) != 0U);

  uint32_t v = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_bkup_write_word((uint8_t)k_ra8_bkup_test_word_mid, (uint32_t)k_ra8_bkup_test_pattern_a));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_read_word((uint8_t)k_ra8_bkup_test_word_mid, &v));
  TEST_ASSERT_EQ(k_ra8_bkup_test_pattern_a, v);

  /* enable_backup == false -> VBAE cleared, window closed. */
  cfg.enable_backup = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_init(&cfg));
  /* HUM Ch 12.2.6 "VBTBER : VBATT Backup Enable Register", p 504 */
  TEST_ASSERT((*ra8_bkup_vbtber() & (uint8_t)k_ra8_bkup_vbtber_mask_vbae) == 0U);
  TEST_END("bkup #131: init arms VBTBER.VBAE for VBTBKRn access");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_byte_read_write_roundtrip(void)
{
  TEST_BEGIN("bkup byte read/write roundtrip");
  prep();
  const ra8_bkup_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_init(&cfg));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_bkup_write_byte((uint16_t)k_ra8_bkup_test_byte_first, (uint8_t)k_ra8_bkup_test_byte_pat_a));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_bkup_write_byte((uint16_t)k_ra8_bkup_test_byte_mid, (uint8_t)k_ra8_bkup_test_byte_pat_b));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_bkup_write_byte((uint16_t)k_ra8_bkup_test_byte_last, (uint8_t)k_ra8_bkup_test_byte_pat_a));

  uint8_t b = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_read_byte((uint16_t)k_ra8_bkup_test_byte_first, &b));
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_test_byte_pat_a), b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_read_byte((uint16_t)k_ra8_bkup_test_byte_mid, &b));
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_test_byte_pat_b), b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_read_byte((uint16_t)k_ra8_bkup_test_byte_last, &b));
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_test_byte_pat_a), b);
  TEST_END("bkup byte read/write roundtrip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_byte_bad_args(void)
{
  TEST_BEGIN("bkup byte bad args");
  prep();
  uint8_t b = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_read_byte((uint16_t)k_ra8_bkup_test_byte_bad, &b));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_bkup_read_byte((uint16_t)k_ra8_bkup_test_byte_huge, &b));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_bkup_read_byte((uint16_t)k_ra8_bkup_test_byte_first, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_bkup_write_byte((uint16_t)k_ra8_bkup_test_byte_bad, (uint8_t)k_ra8_bkup_test_byte_pat_a));
  TEST_END("bkup byte bad args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_zero_all(void)
{
  TEST_BEGIN("bkup zero_all");
  prep();
  /* Pre-fill with non-zero data. */
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_bkup_reg_count; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_write_byte(i, (uint8_t)k_ra8_bkup_test_byte_pat_a));
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_zero_all());
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_bkup_reg_count; ++i) {
    uint8_t b = k_bkup_field_invalid;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_read_byte(i, &b));
    TEST_ASSERT_EQ(0, b);
  }
  TEST_END("bkup zero_all");
}

/* ---------------- tamper init / disable / read ----------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_tamper_init_happy(void)
{
  TEST_BEGIN("bkup tamper_init happy");
  prep();
  /* Set a stale VBTADF flag to confirm init clears it. */
  *ra8_bkup_vbtadsr() = (uint8_t)k_ra8_bkup_vbtadsr_mask_all;

  const ra8_bkup_tamper_config_t cfg = make_tamper_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_tamper_init(&cfg));

  /* HUM Ch 12.2.8 "VBTICTLR : VBATT Input Control Register", p 505
   * channels 0+1 are input-enabled, channel 2 is not. */
  const uint8_t expected_ictlr = (uint8_t)((uint8_t)k_ra8_bkup_vbtictlr_mask_vch0inen |
                                           (uint8_t)k_ra8_bkup_vbtictlr_mask_vch1inen);
  TEST_ASSERT_EQ(expected_ictlr, *ra8_bkup_vbtictlr());

  /* HUM Ch 12.2.9 "VBTICTLR2 : VBATT Input Control Register 2", p 506
   * NCE on all 3 channels; rising edge on channel 1 only. */
  const uint8_t expected_ictlr2 = (uint8_t)((uint8_t)k_ra8_bkup_vbtictlr2_mask_nce_all |
                                            (uint8_t)k_ra8_bkup_vbtictlr2_mask_vch1eg);
  TEST_ASSERT_EQ(expected_ictlr2, *ra8_bkup_vbtictlr2());

  /* HUM Ch 12.2.18 "VBTNCWCR : VBATT Noise Canceler Width Control Register", p 512 */
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_nc_width_64hz), *ra8_bkup_vbtncwcr());

  /* HUM Ch 12.2.15 "VBTADCR1 : VBATT Tamper Detection Control Register 1", p 510
   * IE channels 0+1; CE channels 0+2. */
  const uint8_t expected_adcr1 = (uint8_t)((uint8_t)k_ra8_bkup_vbtadcr1_mask_vbtadie0 |
                                           (uint8_t)k_ra8_bkup_vbtadcr1_mask_vbtadie1 |
                                           (uint8_t)k_ra8_bkup_vbtadcr1_mask_vbtadce0 |
                                           (uint8_t)k_ra8_bkup_vbtadcr1_mask_vbtadce2);
  TEST_ASSERT_EQ(expected_adcr1, *ra8_bkup_vbtadcr1());

  /* HUM Ch 12.2.16 "VBTADCR2 : VBATT Tamper Detection Control Register 2", p 511
   * All 3 channels routed to VBTADFn for capture. */
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_vbtadcr2_mask_all), *ra8_bkup_vbtadcr2());

  /* HUM Ch 12.2.17 "VBTADCR3 : VBATT Tamper Detection Control Register 3", p 511
   * HUK zeroize on channels 1+2. */
  const uint8_t expected_adcr3 = (uint8_t)((uint8_t)k_ra8_bkup_vbtadcr3_mask_vbtadze1 |
                                           (uint8_t)k_ra8_bkup_vbtadcr3_mask_vbtadze2);
  TEST_ASSERT_EQ(expected_adcr3, *ra8_bkup_vbtadcr3());

  /* HUM Ch 12.2.14 "VBTADSR : VBATT Tamper Detection Status Register", p 509
   * Stale flags must be wiped. */
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtadsr());
  TEST_END("bkup tamper_init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_tamper_init_bad_args(void)
{
  TEST_BEGIN("bkup tamper_init bad args");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_bkup_tamper_init(nullptr));

  ra8_bkup_tamper_config_t cfg = make_tamper_cfg();
  cfg.nc_width                 = (ra8_bkup_nc_width_t)k_ra8_bkup_test_nc_width_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_tamper_init(&cfg));

  cfg                  = make_tamper_cfg();
  cfg.channels[1].edge = (ra8_bkup_edge_t)k_bkup_field_invalid;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_tamper_init(&cfg));

  cfg                         = make_tamper_cfg();
  cfg.channels[2].capture_src = (ra8_bkup_capture_src_t)k_bkup_field_invalid;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_tamper_init(&cfg));
  TEST_END("bkup tamper_init bad args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_tamper_disable(void)
{
  TEST_BEGIN("bkup tamper_disable");
  prep();
  /* Pre-load every tamper-side register. */
  *ra8_bkup_vbtictlr()  = (uint8_t)k_ra8_bkup_vbtictlr_mask_all;
  *ra8_bkup_vbtictlr2() = k_bkup_field_invalid;
  *ra8_bkup_vbtadsr()   = (uint8_t)k_ra8_bkup_vbtadsr_mask_all;
  *ra8_bkup_vbtadcr1()  = k_bkup_field_invalid;
  *ra8_bkup_vbtadcr2()  = (uint8_t)k_ra8_bkup_vbtadcr2_mask_all;
  *ra8_bkup_vbtadcr3()  = (uint8_t)k_ra8_bkup_vbtadcr3_mask_all;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_tamper_disable());

  TEST_ASSERT_EQ(0, *ra8_bkup_vbtictlr());
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtictlr2());
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtadsr());
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtadcr1());
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtadcr2());
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtadcr3());
  TEST_END("bkup tamper_disable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_input(void)
{
  TEST_BEGIN("bkup read_input");
  prep();
  *ra8_bkup_vbtimonr() = (uint8_t)((uint8_t)k_ra8_bkup_vbtimonr_mask_vch0mon |
                                   (uint8_t)k_ra8_bkup_vbtimonr_mask_vch2mon);

  bool high = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_read_input(k_ra8_bkup_chan_rtcic0, &high));
  TEST_ASSERT(high);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_read_input(k_ra8_bkup_chan_rtcic1, &high));
  TEST_ASSERT(!high);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_read_input(k_ra8_bkup_chan_rtcic2, &high));
  TEST_ASSERT(high);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_bkup_read_input(k_ra8_bkup_chan_rtcic0, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_bkup_read_input((ra8_bkup_channel_t)k_ra8_bkup_test_chan_bad, &high));
  TEST_END("bkup read_input");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_input_enable(void)
{
  TEST_BEGIN("bkup set_input_enable");
  prep();
  *ra8_bkup_vbtictlr() = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_set_input_enable(k_ra8_bkup_chan_rtcic1, true));
  TEST_ASSERT((*ra8_bkup_vbtictlr() & (uint8_t)k_ra8_bkup_vbtictlr_mask_vch1inen) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_set_input_enable(k_ra8_bkup_chan_rtcic1, false));
  TEST_ASSERT_EQ(0, *ra8_bkup_vbtictlr());

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_bkup_set_input_enable((ra8_bkup_channel_t)k_ra8_bkup_test_chan_bad, true));
  TEST_END("bkup set_input_enable");
}

/* ---------------- voltage monitor ----------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_voltage_monitor(void)
{
  TEST_BEGIN("bkup voltage_monitor");
  prep();
  bool en = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_get_voltage_monitor_enabled(&en));
  TEST_ASSERT(!en);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_set_voltage_monitor(true));
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_vbattmnselr_mask_vbtmnsel), *ra8_bkup_vbattmnselr());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_get_voltage_monitor_enabled(&en));
  TEST_ASSERT(en);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_set_voltage_monitor(false));
  TEST_ASSERT_EQ(0, *ra8_bkup_vbattmnselr());

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_bkup_get_voltage_monitor_enabled(nullptr));
  TEST_END("bkup voltage_monitor");
}

/* ---------------- security partitioning ----------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_security_apply_get(void)
{
  TEST_BEGIN("bkup security apply/get");
  prep();

  const ra8_bkup_security_config_t in = {
    .bbfsar = (uint32_t)k_ra8_bkup_test_bbfsar_value,
    .saba   = (uint16_t)k_ra8_bkup_test_saba_aligned,
    .pabas  = 0U,
    .pabans = (uint16_t)k_ra8_bkup_test_saba_max_ok,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_security_apply(&in));

  /* Live-register checks. */
  TEST_ASSERT_EQ(((uint32_t)k_ra8_bkup_test_bbfsar_value), *ra8_bkup_bbfsar());
  TEST_ASSERT_EQ(((uint16_t)k_ra8_bkup_test_saba_aligned), *ra8_bkup_vbrsabar());
  TEST_ASSERT_EQ(0, *ra8_bkup_vbrpabars());
  TEST_ASSERT_EQ(((uint16_t)k_ra8_bkup_test_saba_max_ok), *ra8_bkup_vbrpabarns());

  ra8_bkup_security_config_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_security_get(&out));
  TEST_ASSERT_EQ(in.bbfsar, out.bbfsar);
  TEST_ASSERT_EQ(in.saba, out.saba);
  TEST_ASSERT_EQ(in.pabas, out.pabas);
  TEST_ASSERT_EQ(in.pabans, out.pabans);
  TEST_END("bkup security apply/get");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_security_bad_args(void)
{
  TEST_BEGIN("bkup security bad args");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_bkup_security_apply(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_bkup_security_get(nullptr));

  /* Bad bbfsar (reserved bit set). */
  ra8_bkup_security_config_t cfg = {
    .bbfsar = (uint32_t)k_ra8_bkup_test_bbfsar_bad,
    .saba   = (uint16_t)k_ra8_bkup_test_saba_aligned,
    .pabas  = 0U,
    .pabans = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_security_apply(&cfg));

  /* Unaligned saba. */
  cfg.bbfsar = 0U;
  cfg.saba   = (uint16_t)k_ra8_bkup_test_saba_unalign;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_security_apply(&cfg));

  /* Bad pabas. */
  cfg.saba  = 0U;
  cfg.pabas = (uint16_t)k_ra8_bkup_test_saba_unalign;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_security_apply(&cfg));

  /* Bad pabans. */
  cfg.pabas  = 0U;
  cfg.pabans = (uint16_t)k_ra8_bkup_test_saba_unalign;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_security_apply(&cfg));
  TEST_END("bkup security bad args");
}

/* ---------------- IRQ path ----------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("bkup attach + dispatch");
  prep();

  void* const cookie = (void*)(uintptr_t)0xCAFEU;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_attach_handler(stub_bkup_cb, cookie));

  ra8_bkup_dispatch((uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf2);
  TEST_ASSERT_EQ(1, s_bkup_cb_count);
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf2), s_bkup_cb_last_flags);
  TEST_ASSERT(s_bkup_cb_last_ctx == cookie);

  /* Detach -> dispatch must be a silent no-op. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_attach_handler(nullptr, nullptr));
  ra8_bkup_dispatch((uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf0);
  TEST_ASSERT_EQ(1, s_bkup_cb_count);
  TEST_END("bkup attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_isr_handle_armed_and_fired(void)
{
  TEST_BEGIN("bkup isr_handle armed+fired");
  prep();
  const ra8_bkup_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_init(&cfg));

  /* Enable IE on channels 0 + 2; flag both 0 and 1. Expect dispatch
   * mask == 0x01 (only ch0 is both armed and flagged). */
  *ra8_bkup_vbtadcr1() = (uint8_t)((uint8_t)k_ra8_bkup_vbtadcr1_mask_vbtadie0 |
                                   (uint8_t)k_ra8_bkup_vbtadcr1_mask_vbtadie2);
  *ra8_bkup_vbtadsr() =
    (uint8_t)((uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf0 | (uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf1);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_attach_handler(stub_bkup_cb, (void*)(uintptr_t)1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_isr_handle());
  TEST_ASSERT_EQ(1, s_bkup_cb_count);
  TEST_ASSERT_EQ(((uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf0), s_bkup_cb_last_flags);
  /* VBTADF0 W0Ced; VBTADF1 left set (not armed). */
  TEST_ASSERT((*ra8_bkup_vbtadsr() & (uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf0) == 0U);
  TEST_ASSERT((*ra8_bkup_vbtadsr() & (uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf1) != 0U);
  TEST_END("bkup isr_handle armed+fired");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_isr_handle_no_fire(void)
{
  TEST_BEGIN("bkup isr_handle no fire");
  prep();
  const ra8_bkup_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_init(&cfg));

  /* Flag set but IE = 0 -> no dispatch, no clear. */
  *ra8_bkup_vbtadcr1() = 0U;
  *ra8_bkup_vbtadsr()  = (uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf2;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_attach_handler(stub_bkup_cb, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_isr_handle());
  TEST_ASSERT_EQ(0, s_bkup_cb_count);
  TEST_ASSERT((*ra8_bkup_vbtadsr() & (uint8_t)k_ra8_bkup_vbtadsr_mask_vbtadf2) != 0U);
  TEST_END("bkup isr_handle no fire");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_isr_handle_not_initialized(void)
{
  TEST_BEGIN("bkup isr_handle not initialized");
  prep();
  /* Without init, isr_handle should refuse. We need to deinit first to
   * make sure the static state flag is clear. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_deinit());
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_bkup_isr_handle());
  TEST_END("bkup isr_handle not initialized");
}

/* ---------------- VDET level coverage ----------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_all_vdet_levels(void)
{
  TEST_BEGIN("bkup all vdet levels accepted");
  prep();
  const ra8_bkup_vdet_level_t levels[] = {
    k_ra8_bkup_vdet_2p80v,
    k_ra8_bkup_vdet_2p53v,
    k_ra8_bkup_vdet_2p10v,
    k_ra8_bkup_vdet_1p95v,
    k_ra8_bkup_vdet_1p85v,
    k_ra8_bkup_vdet_1p75v,
  };
  for (uint8_t i = 0U; i < (uint8_t)(sizeof(levels) / sizeof(levels[0])); ++i) {
    ra8_bkup_config_t cfg = make_cfg();
    cfg.vdet_level        = levels[i];
    TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_init(&cfg));
    /* HUM Ch 12.2.12 "VBTBPCR2 : VBATT Battery Power Supply Control Register 2", p 508 */
    TEST_ASSERT_EQ(((uint8_t)levels[i]),
                   (*ra8_bkup_vbtbpcr2() & (uint8_t)k_ra8_bkup_vbtbpcr2_mask_lvl));
  }
  TEST_END("bkup all vdet levels accepted");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_all_nc_widths(void)
{
  TEST_BEGIN("bkup all nc widths accepted");
  prep();
  for (uint8_t i = 0U; i <= (uint8_t)k_ra8_bkup_nc_width_1hz; ++i) {
    ra8_bkup_tamper_config_t cfg = make_tamper_cfg();
    cfg.nc_width                 = (ra8_bkup_nc_width_t)i;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_tamper_init(&cfg));
    /* HUM Ch 12.2.18 "VBTNCWCR : VBATT Noise Canceler Width Control Register", p 512 */
    TEST_ASSERT_EQ(i, *ra8_bkup_vbtncwcr());
  }
  TEST_END("bkup all nc widths accepted");
}

/**
 * @test test_mcdc_ra8_bkup
 *
 * @par MC/DC:
 * Decision A: ``internal_validate_chan`` line 115,
 * libs/ra8_hal/src/ra8_bkup.c:
 * ``if ((edge != FALLING) && (edge != RISING))`` (2 conditions, ``&&``).
 * N+1 = 3:
 * - V1: edge=FALLING -> dec F (accept)
 * - V2: edge=RISING  -> dec F (accept)
 * - V3: edge=0xFF    -> dec T (reject)
 *
 * Decision B: ``internal_validate_chan`` line 119,
 * ``if ((capture_src != PIN) && (capture_src != VBTADF))``
 * (2 conditions, ``&&``). N+1 = 3:
 * - V1: src=PIN     -> dec F (accept)
 * - V2: src=VBTADF  -> dec F (accept)
 * - V3: src=0xFF    -> dec T (reject)
 * DO-178C 6.4.4.3 met.
 */
static void test_mcdc_ra8_bkup(void)
{
  TEST_BEGIN("bkup MC/DC: validate_chan edge + capture_src decisions");
  prep();
  ra8_bkup_tamper_config_t cfg = make_tamper_cfg();
  cfg.channels[0].edge         = k_ra8_bkup_edge_falling;
  cfg.channels[1].edge         = k_ra8_bkup_edge_rising;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_tamper_init(&cfg));
  cfg                  = make_tamper_cfg();
  cfg.channels[0].edge = (ra8_bkup_edge_t)k_bkup_field_invalid;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_tamper_init(&cfg));
  cfg                         = make_tamper_cfg();
  cfg.channels[0].capture_src = k_ra8_bkup_capture_src_pin;
  cfg.channels[1].capture_src = k_ra8_bkup_capture_src_vbtadf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bkup_tamper_init(&cfg));
  cfg                         = make_tamper_cfg();
  cfg.channels[2].capture_src = (ra8_bkup_capture_src_t)k_bkup_field_invalid;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_bkup_tamper_init(&cfg));
  TEST_END("bkup MC/DC: validate_chan edge + capture_src decisions");
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
  test_word_read_write_roundtrip,
  test_word_bad_args,
  test_word_access_requires_vbae_arm,
  test_byte_read_write_roundtrip,
  test_byte_bad_args,
  test_zero_all,
  test_tamper_init_happy,
  test_tamper_init_bad_args,
  test_tamper_disable,
  test_read_input,
  test_set_input_enable,
  test_voltage_monitor,
  test_security_apply_get,
  test_security_bad_args,
  test_attach_and_dispatch,
  test_isr_handle_armed_and_fired,
  test_isr_handle_no_fire,
  test_isr_handle_not_initialized,
  test_all_vdet_levels,
  test_all_nc_widths,
  test_mcdc_ra8_bkup,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
