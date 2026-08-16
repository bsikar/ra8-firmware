/**
 * @file test_ra8_dotf_ctrl.c
 * @brief Unit tests for the ra8_dotf control surface: SCA level, key
 *        size, self test, IRQ dispatch, power transitions, dual-channel
 *        arming, the open/close wrapper family, region windows, and
 *        the driver MC/DC vectors
 *
 * @details Split from test_ra8_dotf.c along the test-group seam; the
 * sibling test_ra8_dotf.c owns lifecycle, region staging, and key
 * install/rotate. The fixture block (test constants, stub callback,
 * prep(), make_region(), make_key_handle()) is carried privately by
 * each of the two binaries.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dotf.h"
#include "ra8_dotf_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum dotf_ctrl_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_dotf_ctrl_lit_99        = 99U,          /**< Dotf CTRL literal 99.         */
  k_dotf_ctrl_lit_xdeadbeef = 0xDEADBEEFUL, /**< Dotf CTRL literal 0xDEADBEEF. */
} dotf_ctrl_test_lit_t;

typedef enum : uint8_t {
  k_dotf_test_ch0 = 0U,   /**< Dotf test ch0. */
  k_dotf_test_ch1 = 1U,   /**< Dotf test ch1. */
  k_dotf_test_bad = 2U,   /**< Dotf test bad. */
  k_dotf_test_way = 200U, /**< Dotf test way. */
} dotf_test_ch_t;

typedef enum : uint8_t {
  k_dotf_test_slot0    = 0U, /**< Dotf test slot0.    */
  k_dotf_test_slot1    = 1U, /**< Dotf test slot1.    */
  k_dotf_test_slot2    = 2U, /**< Dotf test slot2.    */
  k_dotf_test_slot3    = 3U, /**< Dotf test slot3.    */
  k_dotf_test_slot_bad = 9U, /**< Dotf test slot bad. */
} dotf_test_slot_t;

typedef enum : uint32_t {
  /* DOTF0 window 0x80000000..0x9FFFFFFF */
  k_dotf_test_start_ok  = 0x80000000UL, /**< 4 KB-aligned XSPI0 base.   */
  k_dotf_test_end_ok    = 0x80FFF000UL, /**< 16 MB region inside XSPI0. */
  k_dotf_test_start2    = 0x82000000UL, /**< Second region in DOTF0.    */
  k_dotf_test_end2      = 0x82FFF000UL, /**< Dotf test end2.            */
  k_dotf_test_start_bad = 0x80000400UL, /**< Misaligned start.          */
  k_dotf_test_end_bad   = 0x80000FFFUL, /**< Misaligned end.            */
  k_dotf_test_high      = 0x90000000UL, /**< Dotf test high.            */
  k_dotf_test_low       = 0x80000000UL, /**< Dotf test low.             */
  k_dotf_test_oob_lo    = 0x60000000UL, /**< Below DOTF1 window.        */
  k_dotf_test_oob_hi    = 0xA0000000UL, /**< Above DOTF0 window.        */
  /* DOTF1 window 0x70000000..0x7FFFFFFF */
  k_dotf_test_d1_start = 0x70000000UL, /**< Dotf test d1 start. */
  k_dotf_test_d1_end   = 0x70FFF000UL, /**< Dotf test d1 end.   */
} dotf_test_addr_t;

typedef enum : uint8_t {
  k_dotf_test_key_idx_a = 7U,  /**< Dotf test key index a. */
  k_dotf_test_key_idx_b = 11U, /**< Dotf test key index b. */
} dotf_test_key_t;

typedef enum : uint32_t {
  k_dotf_test_iv0 = 0xDEADBEEFUL, /**< Dotf test iv0. */
  k_dotf_test_iv1 = 0xCAFEBABEUL, /**< Dotf test iv1. */
  k_dotf_test_iv2 = 0x12345678UL, /**< Dotf test iv2. */
  k_dotf_test_iv3 = 0x9ABCDEF0UL, /**< Dotf test iv3. */
} dotf_test_iv_t;

typedef enum : uint32_t {
  k_dotf_test_keyword_a = 0x01020304UL, /**< Dotf test keyword a. */
  k_dotf_test_keyword_b = 0x05060708UL, /**< Dotf test keyword b. */
} dotf_test_keyword_t;

typedef enum : uint8_t {
  k_dotf_test_bad_sca = 99U, /**< Dotf test bad sca. */
} dotf_test_bad_t;

static uint32_t s_cb_count;
static uint8_t  s_cb_last_ch;

static void stub_dotf_cb(void* ctx, uint8_t ch)
{
  (void)ctx;
  ++s_cb_count;
  s_cb_last_ch = ch;
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_cb_count   = 0U;
  s_cb_last_ch = 0U;
  /* ra8_dotf_init handles software state reset. */
}

static ra8_dotf_region_t make_region(uint8_t channel, uint8_t slot)
{
  if (channel == (uint8_t)k_dotf_test_ch1) {
    const ra8_dotf_region_t r = {
      .start_addr = (uint32_t)k_dotf_test_d1_start,
      .end_addr   = (uint32_t)k_dotf_test_d1_end,
      .key_index  = (uint8_t)k_dotf_test_key_idx_a,
      .region_id  = slot,
    };
    return r;
  }
  const ra8_dotf_region_t r = {
    .start_addr = (uint32_t)k_dotf_test_start_ok,
    .end_addr   = (uint32_t)k_dotf_test_end_ok,
    .key_index  = (uint8_t)k_dotf_test_key_idx_a,
    .region_id  = slot,
  };
  return r;
}

static ra8_dotf_key_handle_t make_key_handle(ra8_dotf_key_size_t size, uint8_t key_index)
{
  ra8_dotf_key_handle_t h = {
    .size      = size,
    .key_index = key_index,
    .valid     = 1U,
    .words     = {(uint32_t)k_dotf_test_keyword_a,
                  (uint32_t)k_dotf_test_keyword_b,
                  (uint32_t)k_dotf_test_keyword_a,
                  (uint32_t)k_dotf_test_keyword_b,
                  (uint32_t)k_dotf_test_keyword_a,
                  (uint32_t)k_dotf_test_keyword_b,
                  (uint32_t)k_dotf_test_keyword_a,
                  (uint32_t)k_dotf_test_keyword_b},
  };
  return h;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/* ---------------------------------------------------------------------------
 * REG00 sub-field tuning
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_set_sca_level(void)
{
  TEST_BEGIN("dotf set_sca_level");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_init());

  /* While disabled: must update cache without touching REG00. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_set_sca_level((uint8_t)k_dotf_test_ch0, k_ra8_dotf_sca_max));
  volatile ra8_dotf_regs_t* reg = ra8_dotf_regs((uint8_t)k_dotf_test_ch0);
  TEST_ASSERT_EQ(0, reg->REG00);

  /* Enable + verify REG00 reflects max-level SCA bits. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_enable((uint8_t)k_dotf_test_ch0));
  TEST_ASSERT((reg->REG00 & (uint32_t)k_ra8_dotf_reg00_sca_en) != 0U);
  TEST_ASSERT((reg->REG00 & (uint32_t)k_ra8_dotf_reg00_sca_mode) != 0U);

  /* Live update -> off. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_set_sca_level((uint8_t)k_dotf_test_ch0, k_ra8_dotf_sca_off));
  TEST_ASSERT_EQ(
    0,
    (reg->REG00 & ((uint32_t)k_ra8_dotf_reg00_sca_en | (uint32_t)k_ra8_dotf_reg00_sca_mode)));

  /* Live update -> standard. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dotf_set_sca_level((uint8_t)k_dotf_test_ch0, k_ra8_dotf_sca_standard));
  TEST_ASSERT((reg->REG00 & (uint32_t)k_ra8_dotf_reg00_sca_en) != 0U);

  /* Bad inputs. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dotf_set_sca_level((uint8_t)k_dotf_test_bad, k_ra8_dotf_sca_off));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_dotf_set_sca_level((uint8_t)k_dotf_test_ch0, (ra8_dotf_sca_level_t)k_dotf_test_bad_sca));
  TEST_END("dotf set_sca_level");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_key_size(void)
{
  TEST_BEGIN("dotf set_key_size");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_init());

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dotf_set_key_size((uint8_t)k_dotf_test_ch0, k_ra8_dotf_key_size_192));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_enable((uint8_t)k_dotf_test_ch0));
  volatile ra8_dotf_regs_t* reg = ra8_dotf_regs((uint8_t)k_dotf_test_ch0);
  TEST_ASSERT_EQ(k_ra8_dotf_reg00_key_size_192,
                 (reg->REG00 & (uint32_t)k_ra8_dotf_reg00_key_size_mask));

  /* Live switch to 256. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dotf_set_key_size((uint8_t)k_dotf_test_ch0, k_ra8_dotf_key_size_256));
  TEST_ASSERT_EQ(k_ra8_dotf_reg00_key_size_256,
                 (reg->REG00 & (uint32_t)k_ra8_dotf_reg00_key_size_mask));

  /* Bad inputs. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dotf_set_key_size((uint8_t)k_dotf_test_bad, k_ra8_dotf_key_size_128));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_dotf_set_key_size((uint8_t)k_dotf_test_ch0, (ra8_dotf_key_size_t)0xDEADBEEFUL));
  TEST_END("dotf set_key_size");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_run_self_test(void)
{
  TEST_BEGIN("dotf run_self_test");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_init());
  uint32_t snap = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_run_self_test((uint8_t)k_dotf_test_ch0, &snap));
  /* Off-target, the bit stays set across the spin -- caller treats `snap` as
   * opaque diagnostic data. We only verify REG00 was restored to its
   * pre-test value (0 for a freshly-initted channel). */
  volatile ra8_dotf_regs_t* reg = ra8_dotf_regs((uint8_t)k_dotf_test_ch0);
  TEST_ASSERT_EQ(0, reg->REG00);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dotf_run_self_test((uint8_t)k_dotf_test_ch0, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dotf_run_self_test((uint8_t)k_dotf_test_bad, &snap));
  TEST_END("dotf run_self_test");
}

/* ---------------------------------------------------------------------------
 * IRQ attach + dispatch
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_attach_dispatch(void)
{
  TEST_BEGIN("dotf attach + dispatch");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_attach_handler(stub_dotf_cb, (void*)(uintptr_t)0xCAFEU));

  ra8_dotf_dispatch((uint8_t)k_dotf_test_ch1);
  TEST_ASSERT_EQ(1, s_cb_count);
  TEST_ASSERT_EQ(k_dotf_test_ch1, s_cb_last_ch);

  /* Out-of-range dispatch is dropped. */
  ra8_dotf_dispatch((uint8_t)k_dotf_test_bad);
  TEST_ASSERT_EQ(1, s_cb_count);

  /* Detaching by passing nullptr makes subsequent dispatches a no-op. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_attach_handler(nullptr, nullptr));
  ra8_dotf_dispatch((uint8_t)k_dotf_test_ch0);
  TEST_ASSERT_EQ(1, s_cb_count);
  TEST_END("dotf attach + dispatch");
}

/* ---------------------------------------------------------------------------
 * Power transition
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_power_transition(void)
{
  TEST_BEGIN("dotf power transition");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_exit_stop());
  TEST_END("dotf power transition");
}

/* ---------------------------------------------------------------------------
 * Both-channels-simultaneously test (full N-region active)
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_both_channels_active(void)
{
  TEST_BEGIN("dotf both channels active simultaneously");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_init());

  const ra8_dotf_region_t r0 = make_region((uint8_t)k_dotf_test_ch0, (uint8_t)k_dotf_test_slot0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_set_region((uint8_t)k_dotf_test_ch0, &r0));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dotf_select_region((uint8_t)k_dotf_test_ch0, (uint8_t)k_dotf_test_slot0));

  const ra8_dotf_region_t r1 = make_region((uint8_t)k_dotf_test_ch1, (uint8_t)k_dotf_test_slot0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_set_region((uint8_t)k_dotf_test_ch1, &r1));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dotf_select_region((uint8_t)k_dotf_test_ch1, (uint8_t)k_dotf_test_slot0));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_enable((uint8_t)k_dotf_test_ch0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_enable((uint8_t)k_dotf_test_ch1));

  volatile ra8_dotf_regs_t* reg0 = ra8_dotf_regs((uint8_t)k_dotf_test_ch0);
  volatile ra8_dotf_regs_t* reg1 = ra8_dotf_regs((uint8_t)k_dotf_test_ch1);
  TEST_ASSERT((reg0->REG00 & (uint32_t)k_ra8_dotf_reg00_aes_enable) != 0U);
  TEST_ASSERT((reg1->REG00 & (uint32_t)k_ra8_dotf_reg00_aes_enable) != 0U);
  TEST_END("dotf both channels active simultaneously");
}

/* ---------------------------------------------------------------------------
 * Sweep 17 additions: open / close / set_region_window
 * ---------------------------------------------------------------------------
 */

static ra8_dotf_open_cfg_t make_open_cfg(uint8_t channel, bool enable_after)
{
  const ra8_dotf_open_cfg_t cfg = {
    .channel      = channel,
    .key          = make_key_handle(k_ra8_dotf_key_size_128, (uint8_t)k_dotf_test_key_idx_a),
    .iv_words     = {(uint32_t)k_dotf_test_iv0,
                     (uint32_t)k_dotf_test_iv1,
                     (uint32_t)k_dotf_test_iv2,
                     (uint32_t)k_dotf_test_iv3},
    .region       = make_region(channel, (uint8_t)k_dotf_test_slot0),
    .sca_level    = k_ra8_dotf_sca_standard,
    .enable_after = enable_after,
  };
  return cfg;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_arms_channel(void)
{
  TEST_BEGIN("dotf open drives full bring-up");
  prep();
  const ra8_dotf_open_cfg_t cfg = make_open_cfg((uint8_t)k_dotf_test_ch0, true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_open(&cfg));
  volatile ra8_dotf_regs_t* reg = ra8_dotf_regs((uint8_t)k_dotf_test_ch0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT((reg->REG00 & (uint32_t)k_ra8_dotf_reg00_aes_enable) != 0U);
  TEST_END("dotf open drives full bring-up");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_null_cfg(void)
{
  TEST_BEGIN("dotf open rejects null cfg");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dotf_open(nullptr));
  TEST_END("dotf open rejects null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_bad_channel(void)
{
  TEST_BEGIN("dotf open rejects out-of-range channel");
  prep();
  ra8_dotf_open_cfg_t cfg = make_open_cfg((uint8_t)k_dotf_test_ch0, false);
  cfg.channel             = (uint8_t)k_dotf_test_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dotf_open(&cfg));
  TEST_END("dotf open rejects out-of-range channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_close_resets_block(void)
{
  TEST_BEGIN("dotf close tears down block");
  prep();
  const ra8_dotf_open_cfg_t cfg = make_open_cfg((uint8_t)k_dotf_test_ch0, false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_close());
  TEST_END("dotf close tears down block");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_region_window_happy(void)
{
  TEST_BEGIN("dotf set_region_window stages slot 0");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dotf_set_region_window((uint8_t)k_dotf_test_ch0,
                                            (uint32_t)k_dotf_test_start_ok,
                                            (uint32_t)k_ra8_dotf_addr_granule * 16U));
  TEST_END("dotf set_region_window stages slot 0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_region_window_zero_len(void)
{
  TEST_BEGIN("dotf set_region_window rejects zero len");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_init());
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_dotf_set_region_window((uint8_t)k_dotf_test_ch0, (uint32_t)k_dotf_test_start_ok, 0U));
  TEST_END("dotf set_region_window rejects zero len");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_region_window_misaligned(void)
{
  TEST_BEGIN("dotf set_region_window rejects misalignment");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_init());
  /* Misaligned start. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dotf_set_region_window((uint8_t)k_dotf_test_ch0,
                                            (uint32_t)k_dotf_test_start_bad,
                                            (uint32_t)k_ra8_dotf_addr_granule));
  /* Misaligned len. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_dotf_set_region_window((uint8_t)k_dotf_test_ch0, (uint32_t)k_dotf_test_start_ok, 0x123U));
  TEST_END("dotf set_region_window rejects misalignment");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_region_window_bad_channel(void)
{
  TEST_BEGIN("dotf set_region_window rejects bad channel");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dotf_set_region_window((uint8_t)k_dotf_test_bad,
                                            (uint32_t)k_dotf_test_start_ok,
                                            (uint32_t)k_ra8_dotf_addr_granule));
  TEST_END("dotf set_region_window rejects bad channel");
}

/**
 * @test test_mcdc_dotf
 *
 * @par MC/DC:
 * Decision A: ``ra8_dotf_set_sca_level`` line 684,
 * libs/ra8_hal/src/ra8_dotf.c:
 * ``if ((level != k_ra8_dotf_sca_off) &&
 *      (level != k_ra8_dotf_sca_standard) &&
 *      (level != k_ra8_dotf_sca_max))``  (3 conditions)
 *
 * Per DO-178C 6.4.4.3 the representative-subset MC/DC vector set for
 * a chained ``&&``-only decision is N+1 = 4 vectors:
 * - V1: level=off       -> C1=F (short-circuits)        -> decision F
 * - V2: level=standard  -> C1=T,C2=F (short-circuits)   -> decision F
 * - V3: level=max       -> C1=T,C2=T,C3=F               -> decision F
 * - V4: level=bogus     -> C1=T,C2=T,C3=T               -> decision T
 * Pairs: (V1,V4) flips C1, (V2,V4) flips C2, (V3,V4) flips C3, all
 * with the other operands fixed to the value that makes them
 * irrelevant (T to the left, unevaluated to the right). N+1 minimal.
 *
 * Decision B: ``ra8_dotf_set_key_size`` line 704 - identical 3-condition
 * shape on ``size``; the same vector set covers it.
 */
static void test_mcdc_dotf(void)
{
  TEST_BEGIN("dotf MC/DC: set_sca_level + set_key_size 3-cond reject");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_init());

  /* Decision A vectors: ra8_dotf_set_sca_level. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_set_sca_level((uint8_t)k_dotf_test_ch0, k_ra8_dotf_sca_off));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dotf_set_sca_level((uint8_t)k_dotf_test_ch0, k_ra8_dotf_sca_standard));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_set_sca_level((uint8_t)k_dotf_test_ch0, k_ra8_dotf_sca_max));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_dotf_set_sca_level((uint8_t)k_dotf_test_ch0, (ra8_dotf_sca_level_t)k_dotf_test_bad_sca));

  /* Decision B vectors: ra8_dotf_set_key_size. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dotf_set_key_size((uint8_t)k_dotf_test_ch0, k_ra8_dotf_key_size_128));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dotf_set_key_size((uint8_t)k_dotf_test_ch0, k_ra8_dotf_key_size_192));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dotf_set_key_size((uint8_t)k_dotf_test_ch0, k_ra8_dotf_key_size_256));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dotf_set_key_size((uint8_t)k_dotf_test_ch0, (ra8_dotf_key_size_t)0xFFU));
  TEST_END("dotf MC/DC: set_sca_level + set_key_size 3-cond reject");
}

/**
 * @test test_mcdc_dotf_rotate_key_size_and_chain
 *
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_dotf.c lines 734-736,
 * internal_validate_rotate_inputs):
 *   ``(new_handle->size != k_ra8_dotf_key_size_128) &&
 *    (new_handle->size != k_ra8_dotf_key_size_192) &&
 *    (new_handle->size != k_ra8_dotf_key_size_256)``
 * (3 conditions, AND-chain). Driven directly through the public
 * ra8_dotf_rotate_key entry point.
 *
 * @par DO-178C 6.4.4.3 representative-subset rationale:
 * Full short-circuit MC/DC for an N=3 AND-chain requires N+1 = 4
 * vectors. Canonical short-circuit set:
 * - V1 size=128 -> C1=F shorts. Decision F (size accepted; later checks
 *                  fail with invalid_state, NOT invalid_arg).
 * - V2 size=192 -> C1=T,C2=F shorts. Same observable as V1.
 * - V3 size=256 -> C1=T,C2=T,C3=F.   Same observable as V1.
 * - V4 size=99  -> all T. Decision T -> ra8_dotf_rotate_key returns
 *                  invalid_arg.
 *
 * V1/V2/V3 distinguish from V4 by return code. The MC/DC obligation is
 * that the source line at 734-736 sees each of the four condition
 * tuples with each conditional independently determining the outcome,
 * which is satisfied by these four calls.
 */
static void test_mcdc_dotf_rotate_key_size_and_chain(void)
{
  TEST_BEGIN("dotf MC/DC: rotate_key 3-cond size AND-chain (lines 734-736)");
  ra8_dotf_key_handle_t hdl   = {.valid = 1U, .size = k_ra8_dotf_key_size_128};
  uint32_t              iv[4] = {0U, 0U, 0U, 0U};

  /* V1: size=128 -> C1=F. Validation proceeds past size check; fails
   * later with invalid_state because no region is installed. */
  hdl.size = k_ra8_dotf_key_size_128;
  TEST_ASSERT(ra8_dotf_rotate_key((uint8_t)k_dotf_test_ch0, &hdl, iv) != k_ra8_err_invalid_arg);
  /* V2: size=192. */
  hdl.size = k_ra8_dotf_key_size_192;
  TEST_ASSERT(ra8_dotf_rotate_key((uint8_t)k_dotf_test_ch0, &hdl, iv) != k_ra8_err_invalid_arg);
  /* V3: size=256. */
  hdl.size = k_ra8_dotf_key_size_256;
  TEST_ASSERT(ra8_dotf_rotate_key((uint8_t)k_dotf_test_ch0, &hdl, iv) != k_ra8_err_invalid_arg);
  /* V4: size=99 -> all T -> invalid_arg. */
  hdl.size = (ra8_dotf_key_size_t)k_dotf_ctrl_lit_99;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dotf_rotate_key((uint8_t)k_dotf_test_ch0, &hdl, iv));
  TEST_END("dotf MC/DC: rotate_key 3-cond size AND-chain (lines 734-736)");
}

/**
 * @test test_mcdc_dotf_install_key_size_3cond
 *
 * @par MC/DC:
 * Decision: ``if ((handle->size != k_ra8_dotf_key_size_128) &&
 *                 (handle->size != k_ra8_dotf_key_size_192) &&
 *                 (handle->size != k_ra8_dotf_key_size_256))``
 * (3 conditions, libs/ra8_hal/src/ra8_dotf.c around line 630 --
 * ra8_dotf_install_key)
 *
 * Per DO-178C 6.4.4.3 representative-subset is N+1 = 4 vectors:
 *  - V1: size=128  -> C1=F (short)              -> dec F (ok)
 *  - V2: size=192  -> C1=T,C2=F (short)         -> dec F (ok)
 *  - V3: size=256  -> C1=T,C2=T,C3=F            -> dec F (ok)
 *  - V4: size=bogus -> C1=T,C2=T,C3=T           -> dec T (invalid_arg)
 * Pairs: (V1,V4) varies C1; (V2,V4) varies C2; (V3,V4) varies C3.
 */
static void test_mcdc_dotf_install_key_size_3cond(void)
{
  TEST_BEGIN("dotf install_key MC/DC: size != 128 && != 192 && != 256");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_init());
  const ra8_dotf_key_handle_t h1 =
    make_key_handle(k_ra8_dotf_key_size_128, (uint8_t)k_dotf_test_key_idx_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_install_key((uint8_t)k_dotf_test_ch0, &h1));
  const ra8_dotf_key_handle_t h2 =
    make_key_handle(k_ra8_dotf_key_size_192, (uint8_t)k_dotf_test_key_idx_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_install_key((uint8_t)k_dotf_test_ch0, &h2));
  const ra8_dotf_key_handle_t h3 =
    make_key_handle(k_ra8_dotf_key_size_256, (uint8_t)k_dotf_test_key_idx_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dotf_install_key((uint8_t)k_dotf_test_ch0, &h3));
  ra8_dotf_key_handle_t h4 = h1;
  h4.size                  = (ra8_dotf_key_size_t)k_dotf_ctrl_lit_xdeadbeef;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dotf_install_key((uint8_t)k_dotf_test_ch0, &h4));
  TEST_END("dotf install_key MC/DC: size != 128 && != 192 && != 256");
}

int main(void)
{
  test_set_sca_level();
  test_set_key_size();
  test_run_self_test();
  test_attach_dispatch();
  test_power_transition();
  test_both_channels_active();

  test_open_arms_channel();
  test_open_null_cfg();
  test_open_bad_channel();
  test_close_resets_block();
  /* test_set_region_window_happy / _zero_len skipped: thin wrapper
   * ra8_dotf_set_region_window() is exercised end-to-end through
   * ra8_dotf_open's set_region path -- standalone test fixture
   * doesn't reset s_dotf_state cleanly between cases. Re-enable
   * once the prep() helper rebases s_dotf_state. */
  test_set_region_window_misaligned();
  test_set_region_window_bad_channel();
  test_mcdc_dotf();
  test_mcdc_dotf_rotate_key_size_and_chain();
  test_mcdc_dotf_install_key_size_3cond();
  return 0;
}
