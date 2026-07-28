/**
 * @file test_ra8_gpt_capture.c
 * @brief Unit tests for the GPT input-capture and external-event-count API.
 *
 * @details
 * Exercises ``ra8_gpt_capture_configure`` / ``ra8_gpt_capture_read`` (input
 * capture via GTICASR / GTICBSR -> GTCCRA / GTCCRB) and
 * ``ra8_gpt_event_count_configure`` (external edge counting via GTUPSR /
 * GTDNSR). Each register write is observed through the fake MMIO
 * window provided by ``ra8_fake_mmap``; the two compound argument guards
 * carry minimal MC/DC vector sets.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpt.h"
#include "ra8_gpt_capture.h"
#include "ra8_gpt_regs.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum ra8_gpt_cap_test_channel_t
 * @brief Channel ids used by the capture / event-count tests.
 */
typedef enum : uint8_t {
  k_ra8_gpt_cap_test_channel_valid = 0U,   /**< First valid channel. */
  k_ra8_gpt_cap_test_channel_last  = 13U,  /**< Last valid channel.  */
  k_ra8_gpt_cap_test_channel_bad   = 14U,  /**< One past the last.   */
  k_ra8_gpt_cap_test_channel_huge  = 200U, /**< Far out of range.    */
} ra8_gpt_cap_test_channel_t;

/**
 * @enum ra8_gpt_cap_test_const_t
 * @brief Capture / count masks and latched values used by the tests.
 */
typedef enum : uint32_t {
  k_ra8_gpt_cap_test_latched_a  = 0x0001ABCDUL, /**< GTCCRA capture value.         */
  k_ra8_gpt_cap_test_latched_b  = 0x00027654UL, /**< GTCCRB capture value.         */
  k_ra8_gpt_cap_test_reserved   = 0x80000000UL, /**< Bit 31: reserved/illegal.     */
  k_ra8_gpt_cap_test_gtwp_lock  = 0x0000A501UL, /**< GTWP key with WP re-locked.   */
  k_ra8_gpt_cap_test_bad_select = 9U,           /**< Out-of-range ra8_gpt_ccr_sel. */
} ra8_gpt_cap_test_const_t;

/**
 * @brief Reset the fake MMIO window and the MSTP model before a test.
 *
 * @details Mirrors the ``prep_w35`` helper in ``test_ra8_gpt.c``: a clean
 * peripheral RAM image plus an initialized module-stop model so the
 * GTWP write-protect bracket and the source-select registers read back
 * the values the driver writes.
 * @pre The host MMIO substrate is linked into the test binary.
 * @pre ``ra8_mstp_init`` is safe to call repeatedly.
 * @post All GPT registers in the window read as zero.
 * @post The MSTP model is initialized.
 */
static void prep_capture(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path; the configure guard is covered by its dedicated MC/DC
 * case below)
 */
static void test_capture_configure_a_writes_gticasr(void)
{
  TEST_BEGIN("gpt capture_configure A -> GTICASR");
  prep_capture();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_capture_configure((uint8_t)k_ra8_gpt_cap_test_channel_valid,
                                           k_ra8_gpt_ccr_a,
                                           (uint32_t)k_ra8_gpt_cap_src_ioca_rising));
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_cap_test_channel_valid);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(k_ra8_gpt_cap_src_ioca_rising, reg->GTICASR);
  TEST_ASSERT_EQ(0U, reg->GTICBSR);
  /* GTWP write-protect bracket must end re-locked. */
  TEST_ASSERT_EQ(k_ra8_gpt_cap_test_gtwp_lock, reg->GTWP);
  TEST_END("gpt capture_configure A -> GTICASR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path; the configure guard is covered by its dedicated MC/DC
 * case below)
 */
static void test_capture_configure_b_writes_gticbsr(void)
{
  TEST_BEGIN("gpt capture_configure B -> GTICBSR");
  prep_capture();

  const uint32_t mask =
    (uint32_t)k_ra8_gpt_cap_src_iocb_falling | (uint32_t)k_ra8_gpt_cap_src_elc_a;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gpt_capture_configure((uint8_t)k_ra8_gpt_cap_test_channel_last, k_ra8_gpt_ccr_b, mask));
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_cap_test_channel_last);
  TEST_ASSERT_EQ(mask, reg->GTICBSR);
  TEST_ASSERT_EQ(0U, reg->GTICASR);
  TEST_END("gpt capture_configure B -> GTICBSR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * disable path: a none mask returns the register to compare use)
 */
static void test_capture_configure_none_disables(void)
{
  TEST_BEGIN("gpt capture_configure none clears GTICASR");
  prep_capture();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_capture_configure((uint8_t)k_ra8_gpt_cap_test_channel_valid,
                                           k_ra8_gpt_ccr_a,
                                           (uint32_t)k_ra8_gpt_cap_src_ioca_rising));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_capture_configure((uint8_t)k_ra8_gpt_cap_test_channel_valid,
                                           k_ra8_gpt_ccr_a,
                                           (uint32_t)k_ra8_gpt_cap_src_none));
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_cap_test_channel_valid);
  TEST_ASSERT_EQ(0U, reg->GTICASR);
  TEST_END("gpt capture_configure none clears GTICASR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the bad-channel
 * null-guard contract)
 */
static void test_capture_configure_bad_channel(void)
{
  TEST_BEGIN("gpt capture_configure bad channel");
  prep_capture();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_gpt_capture_configure((uint8_t)k_ra8_gpt_cap_test_channel_bad,
                                           k_ra8_gpt_ccr_a,
                                           (uint32_t)k_ra8_gpt_cap_src_ioca_rising));
  TEST_END("gpt capture_configure bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the happy-path read
 * of both latched capture registers)
 */
static void test_capture_read_a_and_b(void)
{
  TEST_BEGIN("gpt capture_read A/B returns latched GTCNT");
  prep_capture();

  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_cap_test_channel_valid);
  reg->GTCCR[0]                      = (uint32_t)k_ra8_gpt_cap_test_latched_a;
  reg->GTCCR[1]                      = (uint32_t)k_ra8_gpt_cap_test_latched_b;

  uint32_t value_a = 0U;
  uint32_t value_b = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gpt_capture_read((uint8_t)k_ra8_gpt_cap_test_channel_valid, k_ra8_gpt_ccr_a, &value_a));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gpt_capture_read((uint8_t)k_ra8_gpt_cap_test_channel_valid, k_ra8_gpt_ccr_b, &value_b));
  TEST_ASSERT_EQ(k_ra8_gpt_cap_test_latched_a, value_a);
  TEST_ASSERT_EQ(k_ra8_gpt_cap_test_latched_b, value_b);
  TEST_END("gpt capture_read A/B returns latched GTCNT");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the read-path
 * null / range / selector guards individually)
 */
static void test_capture_read_arg_guards(void)
{
  TEST_BEGIN("gpt capture_read arg guards");
  prep_capture();

  uint32_t value = 0U;
  /* NULL out pointer. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_gpt_capture_read((uint8_t)k_ra8_gpt_cap_test_channel_valid, k_ra8_gpt_ccr_a, nullptr));
  /* Out-of-range channel. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_gpt_capture_read((uint8_t)k_ra8_gpt_cap_test_channel_huge, k_ra8_gpt_ccr_a, &value));
  /* Out-of-range selector. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gpt_capture_read((uint8_t)k_ra8_gpt_cap_test_channel_valid,
                                      (ra8_gpt_ccr_sel_t)k_ra8_gpt_cap_test_bad_select,
                                      &value));
  TEST_END("gpt capture_read arg guards");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the event-count
 * happy path; the configure guard is covered by its dedicated MC/DC
 * case below)
 */
static void test_event_count_configure_writes_up_down(void)
{
  TEST_BEGIN("gpt event_count_configure -> GTUPSR/GTDNSR");
  prep_capture();

  /* Quadrature-style: count up on GTIOCnA rising, down on GTIOCnA falling. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_event_count_configure((uint8_t)k_ra8_gpt_cap_test_channel_valid,
                                               (uint32_t)k_ra8_gpt_cnt_src_ioca_rising,
                                               (uint32_t)k_ra8_gpt_cnt_src_ioca_falling));
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_cap_test_channel_valid);
  TEST_ASSERT_EQ(k_ra8_gpt_cnt_src_ioca_rising, reg->GTUPSR);
  TEST_ASSERT_EQ(k_ra8_gpt_cnt_src_ioca_falling, reg->GTDNSR);
  TEST_ASSERT_EQ(k_ra8_gpt_cap_test_gtwp_lock, reg->GTWP);
  TEST_END("gpt event_count_configure -> GTUPSR/GTDNSR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the disable path:
 * none/none restores internal-clock counting)
 */
static void test_event_count_configure_none_disables(void)
{
  TEST_BEGIN("gpt event_count_configure none clears GTUPSR/GTDNSR");
  prep_capture();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_event_count_configure((uint8_t)k_ra8_gpt_cap_test_channel_valid,
                                               (uint32_t)k_ra8_gpt_cnt_src_gtetrga_rising,
                                               (uint32_t)k_ra8_gpt_cnt_src_gtetrgb_falling));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_event_count_configure((uint8_t)k_ra8_gpt_cap_test_channel_valid,
                                               (uint32_t)k_ra8_gpt_cnt_src_none,
                                               (uint32_t)k_ra8_gpt_cnt_src_none));
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_cap_test_channel_valid);
  TEST_ASSERT_EQ(0U, reg->GTUPSR);
  TEST_ASSERT_EQ(0U, reg->GTDNSR);
  TEST_END("gpt event_count_configure none clears GTUPSR/GTDNSR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the bad-channel
 * null-guard contract)
 */
static void test_event_count_configure_bad_channel(void)
{
  TEST_BEGIN("gpt event_count_configure bad channel");
  prep_capture();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_gpt_event_count_configure((uint8_t)k_ra8_gpt_cap_test_channel_bad,
                                               (uint32_t)k_ra8_gpt_cnt_src_gtetrga_rising,
                                               (uint32_t)k_ra8_gpt_cnt_src_none));
  TEST_END("gpt event_count_configure bad channel");
}

/**
 * @test test_mcdc_capture_configure_arg_guard
 *
 * @par MC/DC:
 * Decision: `if (((uint8_t)which > (uint8_t)k_ra8_gpt_ccr_b) ||
 *               (((source_mask & ~k_ra8_gpt_cap_src_valid_mask)) != 0U))`
 * (2 conditions, in ra8_gpt_capture_configure)
 * - Vector 1: which=A,   mask=ioca_rising (valid) -> F,F decision F -> ok.
 * - Vector 2: which=9,   mask=ioca_rising (valid) -> T,_ decision T -> invalid_arg.
 * - Vector 3: which=A,   mask=reserved bit 31     -> F,T decision T -> invalid_arg.
 * MC/DC pair for C1: V1(F,F)->F vs V2(T,_)->T (decision flips, C2 masked
 * by short-circuit). MC/DC pair for C2: V1(F,F)->F vs V3(F,T)->T
 * (decision flips, C1 held F). N+1 = 3 vectors for N=2 conditions:
 * minimal MC/DC.
 */
static void test_mcdc_capture_configure_arg_guard(void)
{
  TEST_BEGIN("gpt capture_configure MC/DC: which>B || mask&~valid");
  prep_capture();

  /* Vector 1: valid selector + valid mask -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_capture_configure((uint8_t)k_ra8_gpt_cap_test_channel_valid,
                                           k_ra8_gpt_ccr_a,
                                           (uint32_t)k_ra8_gpt_cap_src_ioca_rising));
  /* Vector 2: bad selector -> C1=T short-circuit. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gpt_capture_configure((uint8_t)k_ra8_gpt_cap_test_channel_valid,
                                           (ra8_gpt_ccr_sel_t)k_ra8_gpt_cap_test_bad_select,
                                           (uint32_t)k_ra8_gpt_cap_src_ioca_rising));
  /* Vector 3: reserved mask bit -> C1=F, C2=T. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gpt_capture_configure((uint8_t)k_ra8_gpt_cap_test_channel_valid,
                                           k_ra8_gpt_ccr_a,
                                           (uint32_t)k_ra8_gpt_cap_test_reserved));
  TEST_END("gpt capture_configure MC/DC: which>B || mask&~valid");
}

/**
 * @test test_mcdc_event_count_configure_arg_guard
 *
 * @par MC/DC:
 * Decision: `if ((((up_source & ~k_ra8_gpt_cnt_src_valid_mask)) != 0U) ||
 *               (((down_source & ~k_ra8_gpt_cnt_src_valid_mask)) != 0U))`
 * (2 conditions, in ra8_gpt_event_count_configure)
 * - Vector 1: up=ioca_rising(valid), down=ioca_falling(valid) -> F,F -> ok.
 * - Vector 2: up=reserved bit 31,    down=ioca_falling(valid) -> T,_ -> invalid_arg.
 * - Vector 3: up=ioca_rising(valid), down=reserved bit 31     -> F,T -> invalid_arg.
 * MC/DC pair for C1: V1(F,F)->F vs V2(T,_)->T (decision flips, C2 masked
 * by short-circuit). MC/DC pair for C2: V1(F,F)->F vs V3(F,T)->T
 * (decision flips, C1 held F). N+1 = 3 vectors for N=2 conditions:
 * minimal MC/DC.
 */
static void test_mcdc_event_count_configure_arg_guard(void)
{
  TEST_BEGIN("gpt event_count_configure MC/DC: up&~valid || down&~valid");
  prep_capture();

  /* Vector 1: both masks valid -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_event_count_configure((uint8_t)k_ra8_gpt_cap_test_channel_valid,
                                               (uint32_t)k_ra8_gpt_cnt_src_ioca_rising,
                                               (uint32_t)k_ra8_gpt_cnt_src_ioca_falling));
  /* Vector 2: reserved up bit -> C1=T short-circuit. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gpt_event_count_configure((uint8_t)k_ra8_gpt_cap_test_channel_valid,
                                               (uint32_t)k_ra8_gpt_cap_test_reserved,
                                               (uint32_t)k_ra8_gpt_cnt_src_ioca_falling));
  /* Vector 3: reserved down bit -> C1=F, C2=T. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gpt_event_count_configure((uint8_t)k_ra8_gpt_cap_test_channel_valid,
                                               (uint32_t)k_ra8_gpt_cnt_src_ioca_rising,
                                               (uint32_t)k_ra8_gpt_cap_test_reserved));
  TEST_END("gpt event_count_configure MC/DC: up&~valid || down&~valid");
}

int32_t main(void)
{
  test_capture_configure_a_writes_gticasr();
  test_capture_configure_b_writes_gticbsr();
  test_capture_configure_none_disables();
  test_capture_configure_bad_channel();
  test_capture_read_a_and_b();
  test_capture_read_arg_guards();
  test_event_count_configure_writes_up_down();
  test_event_count_configure_none_disables();
  test_event_count_configure_bad_channel();
  test_mcdc_capture_configure_arg_guard();
  test_mcdc_event_count_configure_arg_guard();
  (void)fprintf(stderr, "[OK ] test_ra8_gpt_capture.c\n");
  return 0;
}
