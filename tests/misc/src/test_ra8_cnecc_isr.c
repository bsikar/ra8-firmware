/**
 * @file test_ra8_cnecc_isr.c
 * @brief Fault-handling tests for ra8_cnecc.c (CANFD ECC driver)
 *
 * @details
 * Split out of test_ra8_cnecc.c to keep each test translation unit under
 * the repository file-size cap. This sibling owns the clear-status,
 * dispatch, fault-injection, test-mode, ISR attach / demux, standby,
 * deinit, and open / compute / verify tests; the init / status / counter
 * contract tests stay in test_ra8_cnecc.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_cnecc.h"
#include "ra8_cnecc_regs.h"
#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_fake_irq.h"
#include "ra8_fake_mmap.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum ra8_cnecc_test_const_t
 * @brief Magic-number-free constants used by the test bodies.
 */
typedef enum : uint16_t {
  k_ra8_cnecc_test_inst_first = 0U,     /**< ECCMB0.                              */
  k_ra8_cnecc_test_inst_last  = 1U,     /**< ECCMB1.                              */
  k_ra8_cnecc_test_inst_bad   = 2U,     /**< First out-of-range instance index.   */
  k_ra8_cnecc_test_inst_huge  = 200U,   /**< Way out-of-range instance index.     */
  k_ra8_cnecc_test_addr_a     = 0x123U, /**< Sample 10-bit fault address (a).     */
  k_ra8_cnecc_test_addr_b     = 0x2ABU, /**< Sample 10-bit fault address (b).     */
  k_ra8_cnecc_test_addr_max   = 0x3FFU, /**< Highest 10-bit ECEAD value.          */
  k_ra8_cnecc_test_addr_over  = 0x500U, /**< Bits above ECEAD[9:0] -- masked off. */
  k_ra8_cnecc_test_prio       = 5U,     /**< Sample NVIC priority for ISR attach. */
  k_ra8_cnecc_test_prio_bad   = 200U,   /**< Out-of-range NVIC priority.          */
} ra8_cnecc_test_const_t;

/**
 * @enum ra8_cnecc_test_subst_t
 * @brief Substitute-data values used by the fault-injection tests.
 */
typedef enum : uint32_t {
  k_ra8_cnecc_test_subst_a = 0xDEADBEEFUL, /**< Distinctive marker. */
  k_ra8_cnecc_test_subst_b = 0x55AA55AAUL, /**< Alternate marker.   */
} ra8_cnecc_test_subst_t;

static uint32_t s_cb_count;
static uint8_t  s_cb_last_instance;
static bool     s_cb_last_is_2bit;
static uint16_t s_cb_last_addr;
static void*    s_cb_last_ctx;

static void stub_cnecc_cb(void* ctx, uint8_t instance, bool is_2bit, uint16_t err_addr)
{
  ++s_cb_count;
  s_cb_last_instance = instance;
  s_cb_last_is_2bit  = is_2bit;
  s_cb_last_addr     = err_addr;
  s_cb_last_ctx      = ctx;
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_isr_init();
  s_cb_count         = 0U;
  s_cb_last_instance = 0U;
  s_cb_last_is_2bit  = false;
  s_cb_last_addr     = 0U;
  s_cb_last_ctx      = nullptr;
}

static ra8_cnecc_config_t make_default_cfg(void)
{
  const ra8_cnecc_config_t cfg = {
    .instances =
      {
        [0] = {.correct_1bit = true, .irq_1bit = true, .irq_2bit = true, .enable = true},
        [1] = {.correct_1bit = true, .irq_1bit = true, .irq_2bit = true, .enable = true},
      },
  };
  return cfg;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_writes_clear_mask(void)
{
  TEST_BEGIN("cnecc clear_status writes clear mask");
  prep();

  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  volatile r_cnecc_regs_t* reg = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* Pretend hardware latched a fault. */
  reg->EC710CTL  = (uint32_t)k_ra8_cnecc_mask_ecer2f | (uint32_t)k_ra8_cnecc_mask_ecdedf0;
  reg->EC710EAD0 = (uint32_t)k_ra8_cnecc_test_addr_a;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_clear_status((uint8_t)k_ra8_cnecc_test_inst_first));

  /* Fake mmap is dumb storage -- our clear writes the bundled
   * clear-all mask straight into EC710CTL. */
  TEST_ASSERT_EQ(k_ra8_cnecc_mask_clear_all, reg->EC710CTL);
  TEST_END("cnecc clear_status writes clear mask");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_bad_instance(void)
{
  TEST_BEGIN("cnecc clear_status bad instance");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_cnecc_clear_status((uint8_t)k_ra8_cnecc_test_inst_bad));
  TEST_END("cnecc clear_status bad instance");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_handler_null(void)
{
  TEST_BEGIN("cnecc attach null handler");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cnecc_attach_handler(nullptr, nullptr));
  TEST_END("cnecc attach null handler");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_drops_bad_instance(void)
{
  TEST_BEGIN("cnecc dispatch drops bad instance");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_attach_handler(stub_cnecc_cb, nullptr));
  ra8_cnecc_dispatch((uint8_t)k_ra8_cnecc_test_inst_bad, false, 0U);
  ra8_cnecc_dispatch((uint8_t)k_ra8_cnecc_test_inst_huge, true, 0U);
  ra8_cnecc_dispatch_overflow((uint8_t)k_ra8_cnecc_test_inst_huge);
  TEST_ASSERT_EQ(0, s_cb_count);
  TEST_END("cnecc dispatch drops bad instance");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_invokes_callback_and_counts(void)
{
  TEST_BEGIN("cnecc dispatch invokes callback");
  prep();

  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));
  void* const expected_ctx = (void*)(uintptr_t)0xC0FFEEU;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_attach_handler(stub_cnecc_cb, expected_ctx));

  ra8_cnecc_dispatch((uint8_t)k_ra8_cnecc_test_inst_first,
                     false,
                     (uint16_t)k_ra8_cnecc_test_addr_a);
  ra8_cnecc_dispatch((uint8_t)k_ra8_cnecc_test_inst_first, true, (uint16_t)k_ra8_cnecc_test_addr_b);
  ra8_cnecc_dispatch((uint8_t)k_ra8_cnecc_test_inst_last, true, (uint16_t)k_ra8_cnecc_test_addr_b);

  TEST_ASSERT_EQ(3, s_cb_count);
  TEST_ASSERT_EQ(k_ra8_cnecc_test_inst_last, s_cb_last_instance);
  TEST_ASSERT(s_cb_last_is_2bit);
  TEST_ASSERT_EQ(k_ra8_cnecc_test_addr_b, s_cb_last_addr);
  TEST_ASSERT(s_cb_last_ctx == expected_ctx);

  /* Driver-local counters should reflect the dispatch sequence. */
  ra8_cnecc_status_t s0 = {};
  ra8_cnecc_status_t s1 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_get_status((uint8_t)k_ra8_cnecc_test_inst_first, &s0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_get_status((uint8_t)k_ra8_cnecc_test_inst_last, &s1));
  TEST_ASSERT_EQ(1, s0.one_bit_count);
  TEST_ASSERT_EQ(1, s0.two_bit_count);
  TEST_ASSERT_EQ(0, s1.one_bit_count);
  TEST_ASSERT_EQ(1, s1.two_bit_count);
  TEST_END("cnecc dispatch invokes callback");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_inject_fault_writes_sequence(void)
{
  TEST_BEGIN("cnecc inject_fault writes HUM 42.3.2 sequence");
  prep();
  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_cnecc_inject_fault((uint8_t)k_ra8_cnecc_test_inst_first, nullptr));
  const ra8_cnecc_inject_t req = {
    .substitute   = (uint32_t)k_ra8_cnecc_test_subst_a,
    .one_bit_flip = true,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_inject_fault((uint8_t)k_ra8_cnecc_test_inst_huge, &req));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_inject_fault((uint8_t)k_ra8_cnecc_test_inst_first, &req));
  volatile r_cnecc_regs_t* reg = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_first);
  /* Final TMC value is 0x8082 (substitute select). */
  TEST_ASSERT_EQ(k_ra8_cnecc_mask_test_subst, reg->EC710TMC);
  TEST_ASSERT_EQ(k_ra8_cnecc_test_subst_a, reg->EC710TED);
  /* TED for the other instance untouched. */
  volatile r_cnecc_regs_t* other = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_last);
  TEST_ASSERT_EQ(0, other->EC710TED);

  /* Now run the 2-bit variant on the other instance. */
  const ra8_cnecc_inject_t req2 = {
    .substitute   = (uint32_t)k_ra8_cnecc_test_subst_b,
    .one_bit_flip = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_inject_fault((uint8_t)k_ra8_cnecc_test_inst_last, &req2));
  TEST_ASSERT_EQ(k_ra8_cnecc_mask_test_subst, other->EC710TMC);
  TEST_ASSERT_EQ(k_ra8_cnecc_test_subst_b, other->EC710TED);
  TEST_END("cnecc inject_fault writes HUM 42.3.2 sequence");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_test_mode_disable_and_query(void)
{
  TEST_BEGIN("cnecc test_mode_disable + test_mode_active");
  prep();
  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_test_mode_disable((uint8_t)k_ra8_cnecc_test_inst_huge));
  bool active = false;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_cnecc_test_mode_active((uint8_t)k_ra8_cnecc_test_inst_first, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_test_mode_active((uint8_t)k_ra8_cnecc_test_inst_huge, &active));

  /* Arm injection then query -> active true. */
  const ra8_cnecc_inject_t req = {
    .substitute   = (uint32_t)k_ra8_cnecc_test_subst_a,
    .one_bit_flip = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_inject_fault((uint8_t)k_ra8_cnecc_test_inst_first, &req));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_test_mode_active((uint8_t)k_ra8_cnecc_test_inst_first, &active));
  TEST_ASSERT(active);

  /* Disable test mode and re-query -> false. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_test_mode_disable((uint8_t)k_ra8_cnecc_test_inst_first));
  volatile r_cnecc_regs_t* reg = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_first);
  TEST_ASSERT_EQ(k_ra8_cnecc_mask_test_disable, reg->EC710TMC);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_test_mode_active((uint8_t)k_ra8_cnecc_test_inst_first, &active));
  TEST_ASSERT(!active);
  TEST_END("cnecc test_mode_disable + test_mode_active");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_isr_bad_priority(void)
{
  TEST_BEGIN("cnecc attach_isr bad priority");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_cnecc_attach_isr((uint8_t)k_ra8_cnecc_test_prio_bad));
  TEST_END("cnecc attach_isr bad priority");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_isr_routes_both_vectors(void)
{
  TEST_BEGIN("cnecc attach_isr routes CAN0/CAN1 MRAM_ERI");
  prep();
  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_attach_handler(stub_cnecc_cb, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_attach_isr((uint8_t)k_ra8_cnecc_test_prio));

  /* Both events must now resolve to allocated slots. */
  uint16_t s0 = (uint16_t)k_ra8_isr_slot_none;
  uint16_t s1 = (uint16_t)k_ra8_isr_slot_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_lookup_slot(k_ra8_elc_event_can0_mram_eri, &s0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_lookup_slot(k_ra8_elc_event_can1_mram_eri, &s1));
  TEST_ASSERT(s0 != (uint16_t)k_ra8_isr_slot_none);
  TEST_ASSERT(s1 != (uint16_t)k_ra8_isr_slot_none);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_detach_isr());
  uint16_t s0_after = (uint16_t)k_ra8_isr_slot_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_lookup_slot(k_ra8_elc_event_can0_mram_eri, &s0_after));
  TEST_ASSERT_EQ(k_ra8_isr_slot_none, s0_after);
  TEST_END("cnecc attach_isr routes CAN0/CAN1 MRAM_ERI");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_isr_handler_dispatches_and_clears(void)
{
  TEST_BEGIN("cnecc isr_handler dispatches faults and clears flags");
  prep();
  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_attach_handler(stub_cnecc_cb, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_attach_isr((uint8_t)k_ra8_cnecc_test_prio));

  /* Simulate a 2-bit + overflow event on instance 1. */
  volatile r_cnecc_regs_t* reg = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_last);
  reg->EC710CTL  = (uint32_t)k_ra8_cnecc_mask_ecer2f | (uint32_t)k_ra8_cnecc_mask_ecdedf0 |
                   (uint32_t)k_ra8_cnecc_mask_ecovff | (uint32_t)k_ra8_cnecc_mask_ecervf;
  reg->EC710EAD0 = (uint32_t)k_ra8_cnecc_test_addr_b;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_irq_fire(k_ra8_elc_event_can1_mram_eri));

  /* Callback fired exactly once with 2-bit + correct address. */
  TEST_ASSERT_EQ(1, s_cb_count);
  TEST_ASSERT_EQ(k_ra8_cnecc_test_inst_last, s_cb_last_instance);
  TEST_ASSERT(s_cb_last_is_2bit);
  TEST_ASSERT_EQ(k_ra8_cnecc_test_addr_b, s_cb_last_addr);
  /* Latched flags W0C-cleared. */
  TEST_ASSERT_EQ(k_ra8_cnecc_mask_clear_all, reg->EC710CTL);

  /* Now simulate a 1-bit fault on instance 0. */
  volatile r_cnecc_regs_t* reg0 = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_first);
  reg0->EC710CTL  = (uint32_t)k_ra8_cnecc_mask_ecer1f | (uint32_t)k_ra8_cnecc_mask_ecsedf0 |
                    (uint32_t)k_ra8_cnecc_mask_ecervf;
  reg0->EC710EAD0 = (uint32_t)k_ra8_cnecc_test_addr_a;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_irq_fire(k_ra8_elc_event_can0_mram_eri));
  TEST_ASSERT_EQ(2, s_cb_count);
  TEST_ASSERT_EQ(k_ra8_cnecc_test_inst_first, s_cb_last_instance);
  TEST_ASSERT(!s_cb_last_is_2bit);

  /* Counters reflect both events (and the overflow bump). */
  ra8_cnecc_counters_t c0 = {};
  ra8_cnecc_counters_t c1 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_get_counters((uint8_t)k_ra8_cnecc_test_inst_first, &c0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_get_counters((uint8_t)k_ra8_cnecc_test_inst_last, &c1));
  TEST_ASSERT_EQ(1, c0.one_bit_count);
  TEST_ASSERT_EQ(0, c0.two_bit_count);
  TEST_ASSERT_EQ(1, c1.two_bit_count);
  TEST_ASSERT_EQ(1, c1.overflow_count);
  TEST_END("cnecc isr_handler dispatches faults and clears flags");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enter_exit_standby(void)
{
  TEST_BEGIN("cnecc enter_standby clears, exit_standby restores");
  prep();
  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  /* Simulate latched faults before the standby trip. */
  volatile r_cnecc_regs_t* reg0 = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_first);
  reg0->EC710CTL                = reg0->EC710CTL | (uint32_t)k_ra8_cnecc_mask_ecer1f;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_enter_standby());
  /* ECERVF cleared after standby prep. */
  TEST_ASSERT_EQ(0, (reg0->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ecervf));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_exit_standby());
  /* exit_standby replays cfg, ECERVF re-asserted. */
  TEST_ASSERT((reg0->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ecervf) != 0U);
  TEST_END("cnecc enter_standby clears, exit_standby restores");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_exit_standby_without_init(void)
{
  TEST_BEGIN("cnecc exit_standby without init");
  prep();
  /* The 'initialized' flag flips back to false on deinit. We need to
   * deinit first to test the precondition path. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_deinit());
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_cnecc_exit_standby());
  TEST_END("cnecc exit_standby without init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_clears_judgment(void)
{
  TEST_BEGIN("cnecc deinit clears judgment");
  prep();

  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_deinit());

  for (uint8_t i = (uint8_t)k_ra8_cnecc_test_inst_first; i <= (uint8_t)k_ra8_cnecc_test_inst_last;
       ++i) {
    volatile r_cnecc_regs_t* reg = ra8_cnecc(i);
    TEST_ASSERT_NOT_NULL((void*)reg);
    /* ECERVF must be cleared after deinit. */
    TEST_ASSERT_EQ(0, (reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ecervf));
    /* Test mode disabled after deinit. */
    TEST_ASSERT_EQ(k_ra8_cnecc_mask_test_disable, reg->EC710TMC);
  }
  TEST_END("cnecc deinit clears judgment");
}

/* ---------------------------------------------------------------------------
 * Sweep 17 additions: open + compute + verify
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_open_brings_up_with_defaults(void)
{
  TEST_BEGIN("cnecc open arms both instances with defaults");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_open());
  for (uint8_t i = (uint8_t)k_ra8_cnecc_test_inst_first; i <= (uint8_t)k_ra8_cnecc_test_inst_last;
       ++i) {
    volatile r_cnecc_regs_t* reg = ra8_cnecc(i);
    TEST_ASSERT_NOT_NULL((void*)reg);
    TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ecervf) != 0U);
  }
  TEST_END("cnecc open arms both instances with defaults");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_compute_happy(void)
{
  TEST_BEGIN("cnecc compute returns deterministic CRC");
  prep();
  static const uint32_t buf_a[4] = {0xDEADBEEFU, 0xCAFEBABEU, 0x12345678U, 0x9ABCDEF0U};
  uint32_t              ecc_a    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_compute((uint32_t)(uintptr_t)buf_a, sizeof(buf_a), &ecc_a));
  /* Re-running over the same data must yield the same result. */
  uint32_t ecc_b = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_compute((uint32_t)(uintptr_t)buf_a, sizeof(buf_a), &ecc_b));
  TEST_ASSERT_EQ(ecc_a, ecc_b);
  TEST_END("cnecc compute returns deterministic CRC");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_compute_null_out(void)
{
  TEST_BEGIN("cnecc compute rejects null out_ecc");
  prep();
  static const uint32_t buf[1] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_cnecc_compute((uint32_t)(uintptr_t)buf, sizeof(buf), nullptr));
  TEST_END("cnecc compute rejects null out_ecc");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_compute_misaligned_addr(void)
{
  TEST_BEGIN("cnecc compute rejects misaligned addr");
  prep();
  uint32_t ecc = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_cnecc_compute(0x1001U, 16U, &ecc));
  TEST_END("cnecc compute rejects misaligned addr");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_compute_short_len(void)
{
  TEST_BEGIN("cnecc compute rejects len < 4");
  prep();
  static const uint32_t buf = 0U;
  uint32_t              ecc = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_cnecc_compute((uint32_t)(uintptr_t)&buf, 2U, &ecc));
  TEST_END("cnecc compute rejects len < 4");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_compute_zero_addr(void)
{
  TEST_BEGIN("cnecc compute rejects zero addr");
  prep();
  uint32_t ecc = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cnecc_compute(0U, 16U, &ecc));
  TEST_END("cnecc compute rejects zero addr");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_verify_match(void)
{
  TEST_BEGIN("cnecc verify accepts matching ecc");
  prep();
  static const uint32_t buf[2] = {0x11223344U, 0x55667788U};
  uint32_t              ecc    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_compute((uint32_t)(uintptr_t)buf, sizeof(buf), &ecc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_verify((uint32_t)(uintptr_t)buf, sizeof(buf), ecc));
  TEST_END("cnecc verify accepts matching ecc");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_verify_mismatch(void)
{
  TEST_BEGIN("cnecc verify rejects mismatching ecc");
  prep();
  static const uint32_t buf[2] = {0x11223344U, 0x55667788U};
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch,
                 ra8_cnecc_verify((uint32_t)(uintptr_t)buf, sizeof(buf), 0xBADBADU));
  TEST_END("cnecc verify rejects mismatching ecc");
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
  test_clear_status_writes_clear_mask,
  test_clear_status_bad_instance,
  test_attach_handler_null,
  test_dispatch_drops_bad_instance,
  test_dispatch_invokes_callback_and_counts,
  test_inject_fault_writes_sequence,
  test_test_mode_disable_and_query,
  test_attach_isr_bad_priority,
  test_attach_isr_routes_both_vectors,
  test_isr_handler_dispatches_and_clears,
  test_enter_exit_standby,
  test_exit_standby_without_init,
  test_deinit_clears_judgment,
  test_open_brings_up_with_defaults,
  test_compute_happy,
  test_compute_null_out,
  test_compute_misaligned_addr,
  test_compute_short_len,
  test_compute_zero_addr,
  test_verify_match,
  test_verify_mismatch,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
