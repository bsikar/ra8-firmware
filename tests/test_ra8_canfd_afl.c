/**
 * @file test_ra8_canfd_afl.c
 * @brief Unit tests for the CANFD Acceptance-Filter-List config API.
 *
 * @details
 * Exercises ``ra8_canfd_set_accept_filter`` (libs/ra8_hal/src/ra8_canfd_afl.c):
 * the paged GAFL programming path that hardware-matches received frames by
 * ID / mask and routes them into an RX FIFO. Each register write is
 * observed through the fake MMIO window provided by ``ra8_fake_mmap``:
 * the CFDGAFL[] slot quartet (ID / M / P0 / P1), the CFDGAFLCFG0.RNC0 rule
 * count, and the CFDGAFLECTR unlock / re-lock bracket. Standard vs extended
 * IDs, RTR routing, and every validation reject path are covered.
 *
 * The code under test contains no compound boolean decisions, so the
 * per-test ``@par MC/DC:`` notes record that explicitly rather than a
 * vector table.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra8_canfd_afl_test_channel_t
 * @brief Channel ids used by the AFL tests.
 */
typedef enum : uint8_t {
  k_ra8_canfd_afl_test_channel_0   = 0U, /**< First valid channel.  */
  k_ra8_canfd_afl_test_channel_1   = 1U, /**< Second valid channel. */
  k_ra8_canfd_afl_test_channel_bad = 2U, /**< One past the last.    */
} ra8_canfd_afl_test_channel_t;

/**
 * @enum ra8_canfd_afl_test_vals_t
 * @brief Rule IDs / masks and expected packed register words.
 */
typedef enum : uint32_t {
  k_afl_std_id       = 0x123U,      /**< 11-bit standard accept ID.       */
  k_afl_std_mask     = 0x7FFU,      /**< Full 11-bit mask.                */
  k_afl_ext_id       = 0x1FABCDEFU, /**< 29-bit extended accept ID.       */
  k_afl_ext_mask     = 0x1FFFFFFFU, /**< Full 29-bit mask.                */
  k_afl_std_overflow = 0x800U,      /**< Bit 11 set: beyond 11-bit range. */
  k_afl_ext_overflow = 0x20000000U, /**< Bit 29 set: beyond 29-bit range. */
  k_afl_ide_bit      = 0x80000000U, /**< GAFLIDE / GAFLIDEM bit 31.       */
  k_afl_rtr_bit      = 0x40000000U, /**< GAFLRTR / GAFLRTRM bit 30.       */
  k_afl_compare_bits = 0xC0000000U, /**< GAFLIDEM | GAFLRTRM in the mask. */
  k_afl_rnc_two      = 0x00020000U, /**< RNC0 = 2 packed at shift 16.     */
  k_afl_p1_fifo0     = 0x00000001U, /**< GAFLFDP0 -> RX FIFO 0.           */
  k_afl_p1_fifo1     = 0x00000002U, /**< GAFLFDP1 -> RX FIFO 1.           */
} ra8_canfd_afl_test_vals_t;

/**
 * @brief Reset the fake MMIO window before a test.
 * @pre The host MMIO substrate is linked into the test binary.
 * @post All CANFD registers in the window read as zero.
 */
static void prep_afl(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the happy path that
 * programs two rules and checks the GAFL slots, RNC0, and the re-lock)
 */
static void test_set_accept_filter_programs_two_rules(void)
{
  TEST_BEGIN("canfd set_accept_filter programs two rules");
  prep_afl();

  ra8_canfd_afl_rule_t rules[2] = {};
  rules[0].id                   = k_afl_std_id;
  rules[0].mask                 = k_afl_std_mask;
  rules[0].extended             = false;
  rules[0].rtr                  = false;
  rules[0].target_rx            = 0U;
  rules[1].id                   = k_afl_ext_id;
  rules[1].mask                 = k_afl_ext_mask;
  rules[1].extended             = true;
  rules[1].rtr                  = false;
  rules[1].target_rx            = 1U;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_canfd_set_accept_filter((uint8_t)k_ra8_canfd_afl_test_channel_0, rules, 2U));

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_afl_test_channel_0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* Rule 0: standard, no IDE bit; mask carries the compare-enable bits. */
  TEST_ASSERT_EQ(k_afl_std_id, reg->CFDGAFL[0].ID);
  TEST_ASSERT_EQ(k_afl_std_mask | k_afl_compare_bits, reg->CFDGAFL[0].M);
  TEST_ASSERT_EQ(0U, reg->CFDGAFL[0].P0);
  TEST_ASSERT_EQ(k_afl_p1_fifo0, reg->CFDGAFL[0].P1);
  /* Rule 1: extended -> IDE set; routed to RX FIFO 1. */
  TEST_ASSERT_EQ(k_afl_ext_id | k_afl_ide_bit, reg->CFDGAFL[1].ID);
  TEST_ASSERT_EQ(k_afl_ext_mask | k_afl_compare_bits, reg->CFDGAFL[1].M);
  TEST_ASSERT_EQ(k_afl_p1_fifo1, reg->CFDGAFL[1].P1);
  /* RNC0 = 2 rules; AFLDAE re-locked at exit. */
  TEST_ASSERT_EQ(k_afl_rnc_two, reg->CFDGAFLCFG0);
  TEST_ASSERT_EQ(0U, reg->CFDGAFLECTR);
  TEST_END("canfd set_accept_filter programs two rules");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- a single standard data-frame
 * rule: IDE clear in the ID word, compare bits present in the mask)
 */
static void test_set_accept_filter_standard_rule(void)
{
  TEST_BEGIN("canfd set_accept_filter standard rule");
  prep_afl();

  ra8_canfd_afl_rule_t rule = {};
  rule.id                   = k_afl_std_id;
  rule.mask                 = k_afl_std_mask;
  rule.extended             = false;
  rule.rtr                  = false;
  rule.target_rx            = 0U;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_canfd_set_accept_filter((uint8_t)k_ra8_canfd_afl_test_channel_0, &rule, 1U));

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_afl_test_channel_0);
  TEST_ASSERT((reg->CFDGAFL[0].ID & k_afl_ide_bit) == 0U);
  TEST_ASSERT((reg->CFDGAFL[0].ID & k_afl_rtr_bit) == 0U);
  TEST_ASSERT((reg->CFDGAFL[0].M & k_afl_compare_bits) == k_afl_compare_bits);
  TEST_END("canfd set_accept_filter standard rule");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- a single extended remote-frame
 * rule routed to RX FIFO 1: IDE + RTR set, P1 picks FIFO 1)
 */
static void test_set_accept_filter_extended_rtr_rule(void)
{
  TEST_BEGIN("canfd set_accept_filter extended RTR rule");
  prep_afl();

  ra8_canfd_afl_rule_t rule = {};
  rule.id                   = k_afl_ext_id;
  rule.mask                 = k_afl_ext_mask;
  rule.extended             = true;
  rule.rtr                  = true;
  rule.target_rx            = 1U;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_canfd_set_accept_filter((uint8_t)k_ra8_canfd_afl_test_channel_1, &rule, 1U));

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_afl_test_channel_1);
  TEST_ASSERT((reg->CFDGAFL[0].ID & k_afl_ide_bit) != 0U);
  TEST_ASSERT((reg->CFDGAFL[0].ID & k_afl_rtr_bit) != 0U);
  TEST_ASSERT_EQ(k_afl_ext_id | k_afl_ide_bit | k_afl_rtr_bit, reg->CFDGAFL[0].ID);
  TEST_ASSERT_EQ(k_afl_p1_fifo1, reg->CFDGAFL[0].P1);
  TEST_END("canfd set_accept_filter extended RTR rule");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- null-pointer guard contract)
 */
static void test_set_accept_filter_null_rules(void)
{
  TEST_BEGIN("canfd set_accept_filter rejects NULL rules");
  prep_afl();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_canfd_set_accept_filter((uint8_t)k_ra8_canfd_afl_test_channel_0, nullptr, 1U));
  TEST_END("canfd set_accept_filter rejects NULL rules");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- bad-channel null guard contract)
 */
static void test_set_accept_filter_bad_channel(void)
{
  TEST_BEGIN("canfd set_accept_filter rejects bad channel");
  prep_afl();

  ra8_canfd_afl_rule_t rule = {};
  rule.mask                 = k_afl_std_mask;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_canfd_set_accept_filter((uint8_t)k_ra8_canfd_afl_test_channel_bad, &rule, 1U));
  TEST_END("canfd set_accept_filter rejects bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the zero-count reject path)
 */
static void test_set_accept_filter_zero_count(void)
{
  TEST_BEGIN("canfd set_accept_filter rejects zero count");
  prep_afl();

  ra8_canfd_afl_rule_t rule = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_set_accept_filter((uint8_t)k_ra8_canfd_afl_test_channel_0, &rule, 0U));
  TEST_END("canfd set_accept_filter rejects zero count");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the over-capacity reject path)
 */
static void test_set_accept_filter_over_capacity(void)
{
  TEST_BEGIN("canfd set_accept_filter rejects count over capacity");
  prep_afl();

  ra8_canfd_afl_rule_t rule = {};
  /* One past the 16-entry page-0 capacity. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_set_accept_filter((uint8_t)k_ra8_canfd_afl_test_channel_0,
                                             &rule,
                                             (uint8_t)(k_ra8_canfd_afl_rule_capacity + 1U)));
  TEST_END("canfd set_accept_filter rejects count over capacity");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- standard id / mask overflow rejects)
 */
static void test_set_accept_filter_standard_overflow(void)
{
  TEST_BEGIN("canfd set_accept_filter rejects 11-bit overflow");
  prep_afl();

  /* Standard ID with bit 11 set. */
  ra8_canfd_afl_rule_t id_rule = {};
  id_rule.id                   = k_afl_std_overflow;
  id_rule.extended             = false;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_canfd_set_accept_filter((uint8_t)k_ra8_canfd_afl_test_channel_0, &id_rule, 1U));

  /* Standard mask with bit 11 set. */
  ra8_canfd_afl_rule_t mask_rule = {};
  mask_rule.mask                 = k_afl_std_overflow;
  mask_rule.extended             = false;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_canfd_set_accept_filter((uint8_t)k_ra8_canfd_afl_test_channel_0, &mask_rule, 1U));
  TEST_END("canfd set_accept_filter rejects 11-bit overflow");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- extended id / mask overflow rejects)
 */
static void test_set_accept_filter_extended_overflow(void)
{
  TEST_BEGIN("canfd set_accept_filter rejects 29-bit overflow");
  prep_afl();

  /* Extended ID with bit 29 set. */
  ra8_canfd_afl_rule_t id_rule = {};
  id_rule.id                   = k_afl_ext_overflow;
  id_rule.extended             = true;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_canfd_set_accept_filter((uint8_t)k_ra8_canfd_afl_test_channel_0, &id_rule, 1U));

  /* Extended mask with bit 29 set. */
  ra8_canfd_afl_rule_t mask_rule = {};
  mask_rule.mask                 = k_afl_ext_overflow;
  mask_rule.extended             = true;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_canfd_set_accept_filter((uint8_t)k_ra8_canfd_afl_test_channel_0, &mask_rule, 1U));
  TEST_END("canfd set_accept_filter rejects 29-bit overflow");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the out-of-range RX-FIFO target reject)
 */
static void test_set_accept_filter_bad_target_rx(void)
{
  TEST_BEGIN("canfd set_accept_filter rejects bad target_rx");
  prep_afl();

  ra8_canfd_afl_rule_t rule = {};
  rule.id                   = k_afl_std_id;
  rule.mask                 = k_afl_std_mask;
  rule.target_rx            = (uint8_t)k_ra8_canfd_rx_fifo_count; /* one past the last FIFO */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_set_accept_filter((uint8_t)k_ra8_canfd_afl_test_channel_0, &rule, 1U));
  TEST_END("canfd set_accept_filter rejects bad target_rx");
}

int main(void)
{
  test_set_accept_filter_programs_two_rules();
  test_set_accept_filter_standard_rule();
  test_set_accept_filter_extended_rtr_rule();
  test_set_accept_filter_null_rules();
  test_set_accept_filter_bad_channel();
  test_set_accept_filter_zero_count();
  test_set_accept_filter_over_capacity();
  test_set_accept_filter_standard_overflow();
  test_set_accept_filter_extended_overflow();
  test_set_accept_filter_bad_target_rx();
  return 0;
}
