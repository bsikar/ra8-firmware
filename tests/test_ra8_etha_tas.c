/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_etha_tas.c
 * @brief Unit tests for ra8_etha_tas.c (802.1Qbv time-aware shaper flows)
 *
 * @details Covers the three hardware FLOWS the TAS half of the ETHA driver
 * implements -- the setting flow of HUM Figure 32.11, the TAS RAM reset flow
 * of Figure 32.8, and the entry read flow of Figure 32.15. Split out of
 * test_ra8_etha.c to keep both files under the per-file line cap.
 *
 * The assertions are written against what the MANUAL says each register
 * carries, which is the point: the previous driver put gate states into
 * EATASGL0 (the entry ADDRESS register) and a cut-through flag into
 * EATASGL1.TASGSL (the gate STATE bit), and every call still returned
 * k_ra8_ok (#539).
 */

#include "ra8_err.h"
#include "ra8_etha.h"
#include "ra8_etha_regs.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum etha_tas_test_lit_t
 * @brief Named literals for the register values these cases seed and expect.
 *
 * @details The fake MMIO window is plain RAM, so a read-back test has to seed
 * EATASGRR the way hardware would present it: the gate state in TASGSR at bit
 * 28 and the gate time in TASGTR[27:0].
 */
typedef enum : uint32_t {
  k_etha_tas_test_gsr_bit = 28U,   /**< EATASGRR.TASGSR bit position. */
  k_etha_tas_test_gate_ns = 4242U, /**< Gate time seeded into TASGTR. */
} etha_tas_test_lit_t;

/**
 * @brief Reset the fake MMIO window and the module-stop model before a case.
 *
 * @pre Host test binary with the fake MMIO map available.
 * @post Every ETHA register reads as its reset value.
 * @since 0.1.0
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_tas_schedule_and_enable(void)
{
  TEST_BEGIN("etha tas schedule + enable");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config,
                                 .eaeie0_mask  = 0U,
                                 .eaeie1_mask  = 0U,
                                 .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  static const ra8_etha_tas_entry_t entries[2] = {
    {.gate_time_ns = 1000U, .gate_open = true},
    {.gate_time_ns = 2000U, .gate_open = false},
  };
  ra8_etha_tas_queue_t queues[k_ra8_etha_tc_count] = {};
  queues[k_ra8_etha_tc_7].entries                  = entries;
  queues[k_ra8_etha_tc_7].count                    = 2U;

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_etha_set_tas_schedule(k_ra8_etha_port_0, queues, 0x80U, 100000U, 0xCAFEBABEDEADBEEFULL));
  /* HUM Ch 32.3.5.7 "EATASCSTC0 : TAS Cycle Start Time Configuration Register 0" p 1649 */
  TEST_ASSERT_EQ(0xDEADBEEFU, ra8_etha(k_ra8_etha_port_0)->EATASCSTC0);
  /* HUM Ch 32.3.5.8 "EATASCSTC1 : TAS Cycle Start Time Configuration Register 1" p 1650 */
  TEST_ASSERT_EQ(0xCAFEBABEU, ra8_etha(k_ra8_etha_port_0)->EATASCSTC1);
  /* HUM Ch 32.3.5.11 "EATASCTC : TAS Cycle Time Configuration Register" p 1651 */
  TEST_ASSERT_EQ(100000U, ra8_etha(k_ra8_etha_port_0)->EATASCTC);
  /* EATASIGSC is the per-queue INITIAL gate-state bitmap, not entry data. */
  /* HUM Ch 32.3.5.2 "EATASIGSC : TAS Initial Gate State Configuration Register" p 1647 */
  TEST_ASSERT_EQ(0x80U, ra8_etha(k_ra8_etha_port_0)->EATASIGSC);
  /* Every queue's entry count reaches its own EATASENCi. */
  /* HUM Ch 32.3.5.3 "EATASENCi : TAS Entry Number Configuration Register i (i = 0 to 7)" p 1647 */
  TEST_ASSERT_EQ(2U, ra8_etha(k_ra8_etha_port_0)->EATASENC[k_ra8_etha_tc_7]);
  TEST_ASSERT_EQ(0U, ra8_etha(k_ra8_etha_port_0)->EATASENC[k_ra8_etha_tc_0]);
  /* EATASGL0 holds the entry ADDRESS (the second entry, base 0 + 1), and
   * EATASGL1 holds {TASGSL gate state, TASGTL gate time} -- the second
   * entry is closed, so bit 28 is clear. */
  /* HUM Ch 32.3.5.13 "EATASGL0 : TAS Gate Learn Register 0" p 1652 */
  TEST_ASSERT_EQ(1U, ra8_etha(k_ra8_etha_port_0)->EATASGL0);
  /* HUM Ch 32.3.5.14 "EATASGL1 : TAS Gate Learn Register 1" p 1652 */
  TEST_ASSERT_EQ(2000U, ra8_etha(k_ra8_etha_port_0)->EATASGL1);

  /* First commit of a schedule leaves TASCC clear and sets TASE. */
  /* HUM Ch 32.3.5.1 "EATASC : TAS Configuration Register" p 1646 */
  TEST_ASSERT(((ra8_etha(k_ra8_etha_port_0)->EATASC >> 1) & 1U) == 0U);
  /* HUM Ch 32.3.5.1 "EATASC : TAS Configuration Register" p 1646 */
  TEST_ASSERT(((ra8_etha(k_ra8_etha_port_0)->EATASC >> 0) & 1U) == 1U);

  TEST_END("etha tas schedule + enable");
}

/**
 * @test etha tas live re-programming sets TASCC, and the main switch toggles TASE
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_tas_live_reprogram_and_enable(void)
{
  TEST_BEGIN("etha tas live reprogram + enable");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config,
                                 .eaeie0_mask  = 0U,
                                 .eaeie1_mask  = 0U,
                                 .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  static const ra8_etha_tas_entry_t entries[2] = {
    {.gate_time_ns = 1000U, .gate_open = true},
    {.gate_time_ns = 2000U, .gate_open = false},
  };
  ra8_etha_tas_queue_t queues[k_ra8_etha_tc_count] = {};
  queues[k_ra8_etha_tc_7].entries                  = entries;
  queues[k_ra8_etha_tc_7].count                    = 2U;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_etha_set_tas_schedule(k_ra8_etha_port_0, queues, 0x80U, 100000U, 0ULL));

  /* A second programming, with TASE already set, is a live change: TASCC. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_etha_set_tas_schedule(k_ra8_etha_port_0, queues, 0x01U, 200000U, 0ULL));
  /* HUM Ch 32.3.5.1 "EATASC : TAS Configuration Register" p 1646 */
  TEST_ASSERT(((ra8_etha(k_ra8_etha_port_0)->EATASC >> 1) & 1U) == 1U);

  /* Enable / disable. */
  /* HUM Ch 32.3.5.1 "EATASC : TAS Configuration Register" p 1646 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_enable_tas(k_ra8_etha_port_0, 1U));
  /* HUM Ch 32.3.5.1 "EATASC : TAS Configuration Register" p 1646 */
  TEST_ASSERT(((ra8_etha(k_ra8_etha_port_0)->EATASC >> 0) & 1U) == 1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_enable_tas(k_ra8_etha_port_0, 0U));
  /* HUM Ch 32.3.5.1 "EATASC : TAS Configuration Register" p 1646 */
  TEST_ASSERT(((ra8_etha(k_ra8_etha_port_0)->EATASC >> 0) & 1U) == 0U);
  TEST_END("etha tas live reprogram + enable");
}

/**
 * @test etha tas schedule rejects what hardware cannot hold
 *
 * @par MC/DC:
 * Decision: `if (queues[queue].entries[index].gate_time_ns > k_ra8_etha_mask_tas_gtl)`
 * is single-condition, so one true and one false vector suffice; the
 * capacity and null-pointer rejections are likewise single-condition and
 * each gets its own vector below.
 */
static void test_tas_schedule_rejects_bad_input(void)
{
  TEST_BEGIN("etha tas schedule rejects bad input");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config,
                                 .eaeie0_mask  = 0U,
                                 .eaeie1_mask  = 0U,
                                 .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));

  ra8_etha_tas_queue_t queues[k_ra8_etha_tc_count] = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_etha_set_tas_schedule(k_ra8_etha_port_0, nullptr, 0U, 0U, 0ULL));
  /* Port range is checked AFTER the null guard, so this case has to pass a
   * real queue array to reach it. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_set_tas_schedule((ra8_etha_port_t)(uint8_t)k_ra8_etha_port_count,
                                           queues,
                                           0U,
                                           0U,
                                           0ULL));

  /* A queue that declares entries but supplies no list. */
  queues[k_ra8_etha_tc_1].count = 1U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_etha_set_tas_schedule(k_ra8_etha_port_0, queues, 0U, 0U, 0ULL));

  /* A gate time wider than TASGTL[27:0]. */
  static const ra8_etha_tas_entry_t too_long[1] = {
    {.gate_time_ns = 0x10000000U, .gate_open = true},
  };
  queues[k_ra8_etha_tc_1].entries = too_long;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_set_tas_schedule(k_ra8_etha_port_0, queues, 0U, 0U, 0ULL));

  /* More entries in total than the TAS RAM can hold. */
  static const ra8_etha_tas_entry_t fine[1] = {{.gate_time_ns = 1U, .gate_open = true}};
  queues[k_ra8_etha_tc_1].entries           = fine;
  queues[k_ra8_etha_tc_1].count             = (uint16_t)(k_ra8_etha_tas_entries_max + 1U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_set_tas_schedule(k_ra8_etha_port_0, queues, 0U, 0U, 0ULL));

  /* Nothing was written: the rejection happens before any register write. */
  /* HUM Ch 32.3.5.11 "EATASCTC : TAS Cycle Time Configuration Register" p 1651 */
  TEST_ASSERT_EQ(0U, ra8_etha(k_ra8_etha_port_0)->EATASCTC);
  TEST_END("etha tas schedule rejects bad input");
}

/**
 * @test etha tas entry read-back returns what was learned
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_tas_entry_read_back(void)
{
  TEST_BEGIN("etha tas entry read back");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config,
                                 .eaeie0_mask  = 0U,
                                 .eaeie1_mask  = 0U,
                                 .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));

  /* EATASGRR reads back {TASGSR at bit 28, TASGTR[27:0]}; the fake MMIO
   * window is plain RAM, so seed it the way hardware would present it. */
  /* HUM Ch 32.3.5.17 "EATASGRR : TAS Gate Read Result Register" p 1654 */
  ra8_etha(k_ra8_etha_port_0)->EATASGRR =
    (1U << (uint32_t)k_etha_tas_test_gsr_bit) | (uint32_t)k_etha_tas_test_gate_ns;
  ra8_etha_tas_entry_t got = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_read_tas_entry(k_ra8_etha_port_0, 7U, &got));
  TEST_ASSERT_EQ(k_etha_tas_test_gate_ns, got.gate_time_ns);
  TEST_ASSERT(got.gate_open);
  /* The requested address reached EATASGR.TASGAR. */
  /* HUM Ch 32.3.5.16 "EATASGR : TAS Gate Read Register" p 1653 */
  TEST_ASSERT_EQ(7U, ra8_etha(k_ra8_etha_port_0)->EATASGR);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_etha_read_tas_entry(k_ra8_etha_port_0, 0U, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_etha_read_tas_entry((ra8_etha_port_t)(uint8_t)k_ra8_etha_port_count, 0U, &got));
  TEST_END("etha tas entry read back");
}

/**
 * @test etha tas RAM reset drives EATASRIRM and waits for TASRR
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_tas_ram_reset(void)
{
  TEST_BEGIN("etha tas ram reset");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config,
                                 .eaeie0_mask  = 0U,
                                 .eaeie1_mask  = 0U,
                                 .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));

  /* Happy path: the wait seam is unarmed, so TASRR is taken as asserted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_tas_ram_reset(k_ra8_etha_port_0));
  /* HUM Ch 32.3.5.19 "EATASRIRM : TAS RAM Initialization Register Monitoring Register" p 1655 */
  TEST_ASSERT_EQ(1U, ra8_etha(k_ra8_etha_port_0)->EATASRIRM & 1U);

  /* Timeout leg: arm the seam so EATASRIRM.TASRR never satisfies. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_wait((const volatile void*)&ra8_etha(k_ra8_etha_port_0)->EATASRIRM));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_etha_tas_ram_reset(k_ra8_etha_port_0));
  ra8_fake_mmio_reset();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_tas_ram_reset((ra8_etha_port_t)(uint8_t)k_ra8_etha_port_count));
  TEST_END("etha tas ram reset");
}

/**
 * @test etha tas schedule refuses to program while EATASC.TASCI is set
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_tas_schedule_config_impossible(void)
{
  TEST_BEGIN("etha tas schedule honours TASCI");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config,
                                 .eaeie0_mask  = 0U,
                                 .eaeie1_mask  = 0U,
                                 .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));

  /* HUM Figure 32.11 aborts the whole flow when TASCI reads 1. */
  ra8_etha(k_ra8_etha_port_0)->EATASC              = 1U << (uint32_t)k_ra8_etha_eatasc_tasci_pos;
  ra8_etha_tas_queue_t queues[k_ra8_etha_tc_count] = {};
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 ra8_etha_set_tas_schedule(k_ra8_etha_port_0, queues, 0U, 1234U, 0ULL));
  /* Nothing was programmed: the abort happens before the first write. */
  /* HUM Ch 32.3.5.11 "EATASCTC : TAS Cycle Time Configuration Register" p 1651 */
  TEST_ASSERT_EQ(0U, ra8_etha(k_ra8_etha_port_0)->EATASCTC);
  TEST_END("etha tas schedule honours TASCI");
}

/**
 * @test etha tas learn and read report a stuck hardware flag
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_tas_flow_timeouts(void)
{
  TEST_BEGIN("etha tas flow timeouts");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config,
                                 .eaeie0_mask  = 0U,
                                 .eaeie1_mask  = 0U,
                                 .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));

  static const ra8_etha_tas_entry_t entries[1] = {
    {.gate_time_ns = 1000U, .gate_open = true},
  };
  ra8_etha_tas_queue_t queues[k_ra8_etha_tc_count] = {};
  queues[k_ra8_etha_tc_0].entries                  = entries;
  queues[k_ra8_etha_tc_0].count                    = 1U;

  /* EATASGLR.GL stuck high: the learn never lands, so no commit is issued. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    /* HUM Ch 32.3.5.15 "EATASGLR : TAS Gate Learn Result Register" p 1653 */
    ra8_fake_mmio_fail_wait((const volatile void*)&ra8_etha(k_ra8_etha_port_0)->EATASGLR));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_etha_set_tas_schedule(k_ra8_etha_port_0, queues, 0U, 4321U, 0ULL));
  /* HUM Ch 32.3.5.1 "EATASC : TAS Configuration Register" p 1646 */
  TEST_ASSERT(((ra8_etha(k_ra8_etha_port_0)->EATASC >> 0) & 1U) == 0U);
  ra8_fake_mmio_reset();

  /* EATASGRR.GR stuck high: the read-back reports rather than returning junk. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    /* HUM Ch 32.3.5.17 "EATASGRR : TAS Gate Read Result Register" p 1654 */
    ra8_fake_mmio_fail_wait((const volatile void*)&ra8_etha(k_ra8_etha_port_0)->EATASGRR));
  ra8_etha_tas_entry_t got = {};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_etha_read_tas_entry(k_ra8_etha_port_0, 0U, &got));
  ra8_fake_mmio_reset();
  TEST_END("etha tas flow timeouts");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom.
 */
static void (*const s_test_roster[])(void) = {
  test_tas_schedule_and_enable,
  test_tas_live_reprogram_and_enable,
  test_tas_schedule_rejects_bad_input,
  test_tas_entry_read_back,
  test_tas_ram_reset,
  test_tas_schedule_config_impossible,
  test_tas_flow_timeouts,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_etha_tas.c\n");
  return 0;
}
