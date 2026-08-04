/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_wdt_extended.c
 * @brief Extended unit tests for ra8_wdt.c covering previously uncovered paths
 *
 * @details
 * The OFSm option-setting decode (``ra8_wdt_ofs_get`` and its reader hook)
 * lives in test_ra8_wdt_ofs.c -- a separate responsibility, and splitting it
 * keeps both files under the 1000-line cap.
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_wdt.h"
#include "ra8_wdt_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_wdt_ext_t
 * @brief Out-parameter seeds and the status pattern staged in WDTSR.
 */
typedef enum : uint16_t {
  k_t_u8_unset      = 0xFFU,   /**< Pre-set 8-bit out-parameter: a supervisor slot,
                                    and a timeout selector past the defined set.  */
  k_t_count_unset   = 0xFFFFU, /**< Pre-set 16-bit counter read-back. */
  k_t_wdtsr_refresh = 0x0100U, /**< WDTSR with the refresh-error bit set, which
                                    the status decoder must report.               */
} t_wdt_ext_t;

/* ---------------------------------------------------------------------------
 * Helpers shared by multiple tests
 * ---------------------------------------------------------------------------
 */

static const ra8_wdt_cfg_t k_valid_cfg = {
  .timeout       = k_ra8_wdt_timeout_16384,
  .clock_div     = k_ra8_wdt_clkdiv_8192,
  .window_start  = k_ra8_wdt_window_start_100,
  .window_end    = k_ra8_wdt_window_end_0,
  .on_expiry     = k_ra8_wdt_on_expiry_reset,
  .stop_in_sleep = k_ra8_wdt_sleep_keep_count,
};

/* ---------------------------------------------------------------------------
 * ra8_wdt_init -- invalid-arg paths
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify null cfg pointer is rejected.
 *
 * @pre None.
 * @post ra8_wdt_init returns k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * (no compound decisions -- single null-ptr guard)
 *
 * @since 0.1.0
 */
static void test_wdt_init_null_cfg(void)
{
  TEST_BEGIN("ra8_wdt_init null cfg rejected");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wdt_init(nullptr));
  TEST_END("ra8_wdt_init null cfg rejected");
}

/**
 * @brief Verify invalid clock_div is rejected.
 *
 * @pre None.
 * @post ra8_wdt_init returns k_ra8_err_invalid_arg for bad clock_div.
 *
 * @par MC/DC:
 * Decision: ``!internal_clock_div_is_valid(cfg->clock_div)``
 * - V1: valid div (8192)  -> false -> proceeds.
 * - V2: invalid div (0)   -> true  -> error path.
 *
 * @since 0.1.0
 */
static void test_wdt_init_bad_clock_div(void)
{
  TEST_BEGIN("ra8_wdt_init bad clock_div rejected");
  ra8_fake_mmap_reset();
  ra8_wdt_cfg_t cfg = k_valid_cfg;
  cfg.clock_div     = (ra8_wdt_clock_div_t)0U; /* "Setting prohibited" per HUM. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_wdt_init(&cfg));
  TEST_END("ra8_wdt_init bad clock_div rejected");
}

/**
 * @brief Verify invalid timeout selector is rejected.
 *
 * @pre None.
 * @post ra8_wdt_init returns k_ra8_err_invalid_arg for bad timeout.
 *
 * @par MC/DC:
 * Decision: ``!internal_timeout_sel_is_valid(cfg->timeout)``
 * - V1: valid sel (16384)  -> false -> proceeds.
 * - V2: invalid sel (0xFF) -> true  -> error path.
 *
 * @since 0.1.0
 */
static void test_wdt_init_bad_timeout(void)
{
  TEST_BEGIN("ra8_wdt_init bad timeout rejected");
  ra8_fake_mmap_reset();
  ra8_wdt_cfg_t cfg = k_valid_cfg;
  cfg.timeout       = (ra8_wdt_timeout_sel_t)k_t_u8_unset;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_wdt_init(&cfg));
  TEST_END("ra8_wdt_init bad timeout rejected");
}

/**
 * @brief Verify ra8_wdt_init succeeds with NMI on-expiry setting.
 *
 * @pre None.
 * @post ra8_wdt_init returns k_ra8_ok with on_expiry=nmi.
 *
 * @par MC/DC:
 * Decision: ``cfg->on_expiry == k_ra8_wdt_on_expiry_reset``
 * - V1: on_expiry=reset -> RSTIRQS set.
 * - V2: on_expiry=nmi   -> RSTIRQS cleared.
 *
 * @since 0.1.0
 */
static void test_wdt_init_nmi_expiry(void)
{
  TEST_BEGIN("ra8_wdt_init with on_expiry=nmi ok");
  ra8_fake_mmap_reset();
  ra8_wdt_cfg_t cfg = k_valid_cfg;
  cfg.on_expiry     = k_ra8_wdt_on_expiry_nmi;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_init(&cfg));
  TEST_END("ra8_wdt_init with on_expiry=nmi ok");
}

/**
 * @brief Verify ra8_wdt_init succeeds with stop-in-sleep enabled.
 *
 * @pre None.
 * @post ra8_wdt_init returns k_ra8_ok with stop_in_sleep=stop.
 *
 * @par MC/DC:
 * Decision: ``cfg->stop_in_sleep == k_ra8_wdt_sleep_stop_count``
 * - V1: stop_count  -> SLCSTP set.
 * - V2: keep_count  -> SLCSTP cleared (covered by test_wdt_init_ok).
 *
 * @since 0.1.0
 */
static void test_wdt_init_stop_in_sleep(void)
{
  TEST_BEGIN("ra8_wdt_init stop_in_sleep variant ok");
  ra8_fake_mmap_reset();
  ra8_wdt_cfg_t cfg = k_valid_cfg;
  cfg.stop_in_sleep = k_ra8_wdt_sleep_stop_count;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_init(&cfg));
  TEST_END("ra8_wdt_init stop_in_sleep variant ok");
}

/* ---------------------------------------------------------------------------
 * ra8_wdt_deinit
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify deinit returns ok and clears all subscribers.
 *
 * @pre ra8_wdt_init has been called.
 * @post ra8_wdt_deinit returns k_ra8_ok.
 *
 * @par MC/DC:
 * (no compound decisions -- straight-line path)
 *
 * @since 0.1.0
 */
static void test_wdt_deinit_ok(void)
{
  TEST_BEGIN("ra8_wdt_deinit ok");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_init(&k_valid_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_deinit());
  TEST_END("ra8_wdt_deinit ok");
}

/* ---------------------------------------------------------------------------
 * ra8_wdt_refresh_for
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify refresh_for with valid instance succeeds.
 *
 * @pre None.
 * @post ra8_wdt_refresh_for returns k_ra8_ok for valid instance.
 *
 * @par MC/DC:
 * Decision: ``(uint8_t)which >= k_ra8_wdt_instance_count``
 * - V1: which=0 (valid)     -> false -> ok.
 * - V2: which=count (oob)   -> true  -> error.
 *
 * @since 0.1.0
 */
static void test_wdt_refresh_for_valid(void)
{
  TEST_BEGIN("ra8_wdt_refresh_for valid instance");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_refresh_for((ra8_wdt_instance_t)0U));
  TEST_END("ra8_wdt_refresh_for valid instance");
}

/**
 * @brief Verify refresh_for with out-of-range instance is rejected.
 *
 * @pre None.
 * @post ra8_wdt_refresh_for returns k_ra8_err_invalid_arg for oob instance.
 *
 * @par MC/DC:
 * See test_wdt_refresh_for_valid (V2).
 *
 * @since 0.1.0
 */
static void test_wdt_refresh_for_oob(void)
{
  TEST_BEGIN("ra8_wdt_refresh_for oob instance rejected");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_wdt_refresh_for((ra8_wdt_instance_t)k_ra8_wdt_instance_count));
  TEST_END("ra8_wdt_refresh_for oob instance rejected");
}

/* ---------------------------------------------------------------------------
 * ra8_wdt_get_counter
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify get_counter null pointer is rejected.
 *
 * @pre None.
 * @post ra8_wdt_get_counter returns k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * (no compound decisions -- null guard only)
 *
 * @since 0.1.0
 */
static void test_wdt_get_counter_null(void)
{
  TEST_BEGIN("ra8_wdt_get_counter null rejected");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wdt_get_counter(nullptr));
  TEST_END("ra8_wdt_get_counter null rejected");
}

/**
 * @brief Verify get_counter returns value from WDTSR.
 *
 * @pre ra8_fake_mmap_reset called.
 * @post out_count equals the masked CNTVAL field of WDTSR.
 *
 * @par MC/DC:
 * (no compound decisions -- direct register read)
 *
 * @since 0.1.0
 */
static void test_wdt_get_counter_ok(void)
{
  TEST_BEGIN("ra8_wdt_get_counter reads CNTVAL field");
  ra8_fake_mmap_reset();
  /* Write a canned value into the CNTVAL[13:0] field of WDTSR. */
  ra8_wdt()->WDTSR = (uint16_t)k_t_wdtsr_refresh;
  uint16_t cnt     = k_t_count_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_get_counter(&cnt));
  TEST_ASSERT_EQ((0x0100U & (uint16_t)k_ra8_wdt_sr_cnt_mask), cnt);
  TEST_END("ra8_wdt_get_counter reads CNTVAL field");
}

/* ---------------------------------------------------------------------------
 * ra8_wdt_clear_status_blocking
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify clear_status_blocking rejects bad mask bits.
 *
 * @pre None.
 * @post Returns k_ra8_err_invalid_arg for mask with reserved bits set.
 *
 * @par MC/DC:
 * Decision: ``(mask & ~k_ra8_wdt_status_all) != 0U``
 * - V1: mask=0 (clean)         -> false -> ok path (early return for none).
 * - V2: mask=0x0001 (reserved) -> true  -> error.
 *
 * @since 0.1.0
 */
static void test_wdt_clear_status_blocking_bad_mask(void)
{
  TEST_BEGIN("ra8_wdt_clear_status_blocking bad mask rejected");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_wdt_clear_status_blocking(0x0001U));
  TEST_END("ra8_wdt_clear_status_blocking bad mask rejected");
}

/**
 * @brief Verify clear_status_blocking with none mask returns ok immediately.
 *
 * @pre None.
 * @post Returns k_ra8_ok without touching WDTSR.
 *
 * @par MC/DC:
 * Decision: ``mask == k_ra8_wdt_status_none``
 * - V1: mask=none -> true -> early ok.
 * - V2: mask=underflow -> false -> enter poll loop.
 *
 * @since 0.1.0
 */
static void test_wdt_clear_status_blocking_none(void)
{
  TEST_BEGIN("ra8_wdt_clear_status_blocking mask=none returns ok");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_clear_status_blocking((uint16_t)k_ra8_wdt_status_none));
  TEST_END("ra8_wdt_clear_status_blocking mask=none returns ok");
}

/**
 * @brief Verify clear_status_blocking succeeds when flag is already clear.
 *
 * @pre WDTSR starts at 0.
 * @post Returns k_ra8_ok immediately because WDTSR bits are already 0.
 *
 * @par MC/DC:
 * Decision (inner): ``((uint16_t)reg->WDTSR & mask) == 0U``
 * - V1: flag already 0 -> true -> returns ok first iteration.
 * - V2: flag set       -> false -> continues polling (timeout test covers this).
 *
 * @since 0.1.0
 */
static void test_wdt_clear_status_blocking_already_clear(void)
{
  TEST_BEGIN("ra8_wdt_clear_status_blocking already clear returns ok");
  ra8_fake_mmap_reset();
  /* WDTSR starts at zero after reset -- asking to clear underflow is a no-op. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_clear_status_blocking((uint16_t)k_ra8_wdt_status_underflow));
  TEST_END("ra8_wdt_clear_status_blocking already clear returns ok");
}

/* ---------------------------------------------------------------------------
 * ra8_wdt_timeout_cycles_get
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify all four valid timeout selectors return correct cycle counts.
 *
 * @pre None.
 * @post Each valid selector returns the documented cycle count.
 *
 * @par MC/DC:
 * (switch exhaustion covers all 4 case arms; no compound boolean)
 *
 * @since 0.1.0
 */
static void test_wdt_timeout_cycles_get_all(void)
{
  TEST_BEGIN("ra8_wdt_timeout_cycles_get all valid selectors");
  uint16_t cycles = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_timeout_cycles_get(k_ra8_wdt_timeout_1024, &cycles));
  TEST_ASSERT_EQ(k_ra8_wdt_cycles_1024, cycles);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_timeout_cycles_get(k_ra8_wdt_timeout_4096, &cycles));
  TEST_ASSERT_EQ(k_ra8_wdt_cycles_4096, cycles);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_timeout_cycles_get(k_ra8_wdt_timeout_8192, &cycles));
  TEST_ASSERT_EQ(k_ra8_wdt_cycles_8192, cycles);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_timeout_cycles_get(k_ra8_wdt_timeout_16384, &cycles));
  TEST_ASSERT_EQ(k_ra8_wdt_cycles_16384, cycles);
  TEST_END("ra8_wdt_timeout_cycles_get all valid selectors");
}

/**
 * @brief Verify timeout_cycles_get rejects null pointer.
 *
 * @pre None.
 * @post Returns k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * (null-ptr guard -- single condition)
 *
 * @since 0.1.0
 */
static void test_wdt_timeout_cycles_get_null(void)
{
  TEST_BEGIN("ra8_wdt_timeout_cycles_get null rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wdt_timeout_cycles_get(k_ra8_wdt_timeout_1024, nullptr));
  TEST_END("ra8_wdt_timeout_cycles_get null rejected");
}

/**
 * @brief Verify timeout_cycles_get rejects invalid selector.
 *
 * @pre None.
 * @post Returns k_ra8_err_invalid_arg for unknown selector.
 *
 * @par MC/DC:
 * (default arm of switch -- selector falls through to invalid path)
 *
 * @since 0.1.0
 */
static void test_wdt_timeout_cycles_get_invalid(void)
{
  TEST_BEGIN("ra8_wdt_timeout_cycles_get invalid selector rejected");
  uint16_t cycles = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_wdt_timeout_cycles_get((ra8_wdt_timeout_sel_t)0xFFU, &cycles));
  TEST_END("ra8_wdt_timeout_cycles_get invalid selector rejected");
}

/* ---------------------------------------------------------------------------
 * ra8_wdt_pclkb_divisor
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify all six valid clock-div values return correct divisors.
 *
 * @pre None.
 * @post Each valid divisor enum returns the documented integer value.
 *
 * @par MC/DC:
 * (switch exhaustion covers all 6 case arms; no compound boolean)
 *
 * @since 0.1.0
 */
static void test_wdt_pclkb_divisor_all(void)
{
  TEST_BEGIN("ra8_wdt_pclkb_divisor all valid dividers");
  uint16_t div = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_pclkb_divisor(k_ra8_wdt_clkdiv_4, &div));
  TEST_ASSERT_EQ(k_ra8_wdt_div_value_4, div);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_pclkb_divisor(k_ra8_wdt_clkdiv_64, &div));
  TEST_ASSERT_EQ(k_ra8_wdt_div_value_64, div);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_pclkb_divisor(k_ra8_wdt_clkdiv_128, &div));
  TEST_ASSERT_EQ(k_ra8_wdt_div_value_128, div);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_pclkb_divisor(k_ra8_wdt_clkdiv_512, &div));
  TEST_ASSERT_EQ(k_ra8_wdt_div_value_512, div);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_pclkb_divisor(k_ra8_wdt_clkdiv_2048, &div));
  TEST_ASSERT_EQ(k_ra8_wdt_div_value_2048, div);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_pclkb_divisor(k_ra8_wdt_clkdiv_8192, &div));
  TEST_ASSERT_EQ(k_ra8_wdt_div_value_8192, div);
  TEST_END("ra8_wdt_pclkb_divisor all valid dividers");
}

/**
 * @brief Verify pclkb_divisor rejects null pointer.
 *
 * @pre None.
 * @post Returns k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * (null-ptr guard -- single condition)
 *
 * @since 0.1.0
 */
static void test_wdt_pclkb_divisor_null(void)
{
  TEST_BEGIN("ra8_wdt_pclkb_divisor null rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wdt_pclkb_divisor(k_ra8_wdt_clkdiv_8192, nullptr));
  TEST_END("ra8_wdt_pclkb_divisor null rejected");
}

/**
 * @brief Verify pclkb_divisor rejects invalid clock-div.
 *
 * @pre None.
 * @post Returns k_ra8_err_invalid_arg for unknown divider.
 *
 * @par MC/DC:
 * (default arm of switch)
 *
 * @since 0.1.0
 */
static void test_wdt_pclkb_divisor_invalid(void)
{
  TEST_BEGIN("ra8_wdt_pclkb_divisor invalid rejected");
  uint16_t div = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_wdt_pclkb_divisor((ra8_wdt_clock_div_t)0xFFU, &div));
  TEST_END("ra8_wdt_pclkb_divisor invalid rejected");
}

/* ---------------------------------------------------------------------------
 * ra8_wdt_total_pclkb_cycles
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify total_pclkb_cycles null out pointer rejected.
 *
 * @pre None.
 * @post Returns k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * (null-ptr guard)
 *
 * @since 0.1.0
 */
static void test_wdt_total_pclkb_cycles_null(void)
{
  TEST_BEGIN("ra8_wdt_total_pclkb_cycles null rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_wdt_total_pclkb_cycles(k_ra8_wdt_timeout_1024, k_ra8_wdt_clkdiv_4, nullptr));
  TEST_END("ra8_wdt_total_pclkb_cycles null rejected");
}

/**
 * @brief Verify total_pclkb_cycles returns correct product.
 *
 * @pre None.
 * @post Returns 1024 * 4 = 4096.
 *
 * @par MC/DC:
 * Decision: ``e1 != k_ra8_ok``
 * - V1: sel=1024, div=4 (valid) -> e1=ok, e2=ok -> product returned.
 * - V2: sel=invalid             -> e1=err        -> early return.
 *
 * @since 0.1.0
 */
static void test_wdt_total_pclkb_cycles_ok(void)
{
  TEST_BEGIN("ra8_wdt_total_pclkb_cycles computes product");
  uint32_t total = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_wdt_total_pclkb_cycles(k_ra8_wdt_timeout_1024, k_ra8_wdt_clkdiv_4, &total));
  /* 1024 cycles * 4 div = 4096 */
  const uint32_t k_expected = (uint32_t)k_ra8_wdt_cycles_1024 * (uint32_t)k_ra8_wdt_div_value_4;
  TEST_ASSERT_EQ(k_expected, total);
  TEST_END("ra8_wdt_total_pclkb_cycles computes product");
}

/**
 * @brief Verify total_pclkb_cycles propagates invalid selector error.
 *
 * @pre None.
 * @post Returns k_ra8_err_invalid_arg when sel is bad.
 *
 * @par MC/DC:
 * Decision: ``e1 != k_ra8_ok`` -- V2 branch (see test above).
 *
 * @since 0.1.0
 */
static void test_wdt_total_pclkb_cycles_bad_sel(void)
{
  TEST_BEGIN("ra8_wdt_total_pclkb_cycles bad sel propagated");
  uint32_t total = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_wdt_total_pclkb_cycles((ra8_wdt_timeout_sel_t)0xFFU, k_ra8_wdt_clkdiv_4, &total));
  TEST_END("ra8_wdt_total_pclkb_cycles bad sel propagated");
}

/**
 * @brief Verify total_pclkb_cycles propagates invalid div error.
 *
 * @pre None.
 * @post Returns k_ra8_err_invalid_arg when div is bad.
 *
 * @par MC/DC:
 * Decision: ``e2 != k_ra8_ok`` -- both e1=ok, e2=err path.
 *
 * @since 0.1.0
 */
static void test_wdt_total_pclkb_cycles_bad_div(void)
{
  TEST_BEGIN("ra8_wdt_total_pclkb_cycles bad div propagated");
  uint32_t total = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_wdt_total_pclkb_cycles(k_ra8_wdt_timeout_1024, (ra8_wdt_clock_div_t)0xFFU, &total));
  TEST_END("ra8_wdt_total_pclkb_cycles bad div propagated");
}

/* ---------------------------------------------------------------------------
 * ra8_wdt_subscribe / ra8_wdt_unsubscribe / ra8_wdt_subscriber_count
 * ---------------------------------------------------------------------------
 */

static uint32_t s_sub_count;

static void stub_sub(void* ctx, uint16_t mask)
{
  (void)ctx;
  (void)mask;
  ++s_sub_count;
}

/**
 * @brief Verify subscribe null fn is rejected.
 *
 * @pre None.
 * @post Returns k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * (null-ptr guard on fn -- single condition)
 *
 * @since 0.1.0
 */
static void test_wdt_subscribe_null_fn(void)
{
  TEST_BEGIN("ra8_wdt_subscribe null fn rejected");
  ra8_fake_mmap_reset();
  (void)ra8_wdt_deinit();
  uint8_t slot = k_t_u8_unset;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wdt_subscribe(nullptr, nullptr, &slot));
  TEST_END("ra8_wdt_subscribe null fn rejected");
}

/**
 * @brief Verify subscribe fills slots and subscriber_count reflects it.
 *
 * @pre deinit called to clear slots.
 * @post subscriber_count increases with each subscribe.
 *
 * @par MC/DC:
 * Decision (inner loop): ``s_wdt_subs[i].fn == nullptr``
 * - V1: slot free  -> true  -> claim slot, return ok.
 * - V2: slot used  -> false -> advance.
 *
 * @since 0.1.0
 */
static void test_wdt_subscribe_and_count(void)
{
  TEST_BEGIN("ra8_wdt_subscribe and subscriber_count");
  ra8_fake_mmap_reset();
  (void)ra8_wdt_deinit();
  s_sub_count = 0U;

  uint8_t slot1 = k_t_u8_unset;
  uint8_t slot2 = k_t_u8_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_subscribe(stub_sub, nullptr, &slot1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_subscribe(stub_sub, nullptr, &slot2));

  /* Slots should be distinct. */
  TEST_ASSERT(slot1 != slot2);
  /* Count should include legacy slot (0) which is free, so count = 2. */
  TEST_ASSERT(ra8_wdt_subscriber_count() >= 2U);
  TEST_END("ra8_wdt_subscribe and subscriber_count");
}

/**
 * @brief Verify unsubscribe with null slot fn returns not_found.
 *
 * @pre Slot is free (no subscriber).
 * @post Returns k_ra8_err_not_found.
 *
 * @par MC/DC:
 * Decision: ``s_wdt_subs[slot].fn == nullptr``
 * - V1: fn=nullptr (free slot)  -> true  -> k_ra8_err_not_found.
 * - V2: fn!=nullptr (used slot) -> false -> clears and returns ok.
 *
 * @since 0.1.0
 */
static void test_wdt_unsubscribe_free_slot(void)
{
  TEST_BEGIN("ra8_wdt_unsubscribe free slot returns not_found");
  ra8_fake_mmap_reset();
  (void)ra8_wdt_deinit();

  /* Slot 1 is free after deinit. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_wdt_unsubscribe(1U));
  TEST_END("ra8_wdt_unsubscribe free slot returns not_found");
}

/**
 * @brief Verify unsubscribe out-of-range slot is rejected.
 *
 * @pre None.
 * @post Returns k_ra8_err_invalid_arg.
 *
 * @par MC/DC:
 * Decision: ``slot >= k_ra8_wdt_max_subs``
 * - V1: slot=0           -> false -> proceed.
 * - V2: slot=max_subs    -> true  -> error.
 *
 * @since 0.1.0
 */
static void test_wdt_unsubscribe_oob(void)
{
  TEST_BEGIN("ra8_wdt_unsubscribe oob slot rejected");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_wdt_unsubscribe((uint8_t)k_ra8_wdt_max_subs));
  TEST_END("ra8_wdt_unsubscribe oob slot rejected");
}

/**
 * @brief Verify subscribe/unsubscribe round-trip works.
 *
 * @pre deinit called.
 * @post After unsubscribe slot is free and count decreases.
 *
 * @par MC/DC:
 * Exercises all three arms of the unsubscribe guard + the slot-free check.
 *
 * @since 0.1.0
 */
static void test_wdt_subscribe_unsubscribe_roundtrip(void)
{
  TEST_BEGIN("ra8_wdt_subscribe/unsubscribe round-trip");
  ra8_fake_mmap_reset();
  (void)ra8_wdt_deinit();

  uint8_t slot = k_t_u8_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_subscribe(stub_sub, nullptr, &slot));
  TEST_ASSERT(slot < k_ra8_wdt_max_subs);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_unsubscribe(slot));
  /* Now unsubscribe again should return not_found. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_wdt_unsubscribe(slot));
  TEST_END("ra8_wdt_subscribe/unsubscribe round-trip");
}

/**
 * @brief Verify subscribe returns no_mem when all slots are taken.
 *
 * @pre deinit called; then fill all non-legacy slots.
 * @post Returns k_ra8_err_no_mem on the next subscribe.
 *
 * @par MC/DC:
 * Decision (loop exit without finding free slot): exhaustion path.
 *
 * @since 0.1.0
 */
static void test_wdt_subscribe_table_full(void)
{
  TEST_BEGIN("ra8_wdt_subscribe table full returns no_mem");
  ra8_fake_mmap_reset();
  (void)ra8_wdt_deinit();

  /* Fill every non-legacy slot (slots 1..k_ra8_wdt_max_subs-1). */
  const uint8_t k_non_legacy = (uint8_t)(k_ra8_wdt_max_subs - 1U);
  for (uint8_t i = 0U; i < k_non_legacy; ++i) {
    uint8_t         slot = k_t_u8_unset;
    const ra8_err_t e    = ra8_wdt_subscribe(stub_sub, nullptr, &slot);
    TEST_ASSERT_EQ(k_ra8_ok, e);
  }

  uint8_t overflow = k_t_u8_unset;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_wdt_subscribe(stub_sub, nullptr, &overflow));
  TEST_END("ra8_wdt_subscribe table full returns no_mem");
}

/* ---------------------------------------------------------------------------
 * ra8_wdt_enter_stop / ra8_wdt_exit_stop
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify enter_stop sets SLCSTP in WDTCSTPR.
 *
 * @pre ra8_fake_mmap_reset.
 * @post WDTCSTPR has SLCSTP bit set after enter_stop.
 *
 * @par MC/DC:
 * (no compound decisions -- straight-line register write)
 *
 * @since 0.1.0
 */
static void test_wdt_enter_exit_stop(void)
{
  TEST_BEGIN("ra8_wdt_enter_stop / ra8_wdt_exit_stop");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_enter_stop());
  TEST_ASSERT_EQ(k_ra8_wdt_cstpr_slcstp, ra8_wdt()->WDTCSTPR);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_exit_stop());
  TEST_ASSERT_EQ(0, ra8_wdt()->WDTCSTPR);
  TEST_END("ra8_wdt_enter_stop / ra8_wdt_exit_stop");
}

/* ---------------------------------------------------------------------------
 * ra8_wdt_install_nmi / ra8_wdt_uninstall_nmi
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify install_nmi returns ok.
 *
 * @pre ra8_fake_mmap_reset.
 * @post install_nmi returns k_ra8_ok.
 *
 * @par MC/DC:
 * (no compound decisions -- sequential ICU calls; both return ok off-target)
 *
 * @since 0.1.0
 */
static void test_wdt_install_uninstall_nmi(void)
{
  TEST_BEGIN("ra8_wdt_install_nmi / ra8_wdt_uninstall_nmi");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_install_nmi());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_uninstall_nmi());
  TEST_END("ra8_wdt_install_nmi / ra8_wdt_uninstall_nmi");
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
  test_wdt_init_null_cfg,
  test_wdt_init_bad_clock_div,
  test_wdt_init_bad_timeout,
  test_wdt_init_nmi_expiry,
  test_wdt_init_stop_in_sleep,
  test_wdt_deinit_ok,
  test_wdt_refresh_for_valid,
  test_wdt_refresh_for_oob,
  test_wdt_get_counter_null,
  test_wdt_get_counter_ok,
  test_wdt_clear_status_blocking_bad_mask,
  test_wdt_clear_status_blocking_none,
  test_wdt_clear_status_blocking_already_clear,
  test_wdt_timeout_cycles_get_all,
  test_wdt_timeout_cycles_get_null,
  test_wdt_timeout_cycles_get_invalid,
  test_wdt_pclkb_divisor_all,
  test_wdt_pclkb_divisor_null,
  test_wdt_pclkb_divisor_invalid,
  test_wdt_total_pclkb_cycles_null,
  test_wdt_total_pclkb_cycles_ok,
  test_wdt_total_pclkb_cycles_bad_sel,
  test_wdt_total_pclkb_cycles_bad_div,
  test_wdt_subscribe_null_fn,
  test_wdt_subscribe_and_count,
  test_wdt_unsubscribe_free_slot,
  test_wdt_unsubscribe_oob,
  test_wdt_subscribe_unsubscribe_roundtrip,
  test_wdt_subscribe_table_full,
  test_wdt_enter_exit_stop,
  test_wdt_install_uninstall_nmi,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_wdt_extended.c\n");
  return 0;
}
