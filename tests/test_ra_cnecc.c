/**
 * @file test_ra_cnecc.c
 * @brief Unit tests for ra_cnecc.c (CANFD ECC driver)
 *
 * @details
 * Covers every public API entry point in ``ra_cnecc.h`` plus every
 * register field documented in HUM Ch 42 (EC710CTL bits, EC710TMC
 * bits, EC710TED, EC710EAD0). Each test acquires a fresh sim mmap
 * via ``prep`` so register state never leaks between cases.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_cnecc_regs.h"
#include "ra8d2_elc_regs.h"
#include "ra_cnecc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_sim_irq.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_cnecc_test_const_t
 * @brief Magic-number-free constants used by the test bodies.
 */
typedef enum : uint16_t {
  k_ra_cnecc_test_inst_first = 0U,     /**< ECCMB0.                                */
  k_ra_cnecc_test_inst_last  = 1U,     /**< ECCMB1.                                */
  k_ra_cnecc_test_inst_bad   = 2U,     /**< First out-of-range instance index.     */
  k_ra_cnecc_test_inst_huge  = 200U,   /**< Way out-of-range instance index.       */
  k_ra_cnecc_test_addr_a     = 0x123U, /**< Sample 10-bit fault address (a).      */
  k_ra_cnecc_test_addr_b     = 0x2ABU, /**< Sample 10-bit fault address (b).      */
  k_ra_cnecc_test_addr_max   = 0x3FFU, /**< Highest 10-bit ECEAD value.           */
  k_ra_cnecc_test_addr_over  = 0x500U, /**< Bits above ECEAD[9:0] -- masked off.  */
  k_ra_cnecc_test_prio       = 5U,     /**< Sample NVIC priority for ISR attach.  */
  k_ra_cnecc_test_prio_bad   = 200U,   /**< Out-of-range NVIC priority.           */
} ra_cnecc_test_const_t;

/**
 * @enum ra_cnecc_test_subst_t
 * @brief Substitute-data values used by the fault-injection tests.
 */
typedef enum : uint32_t {
  k_ra_cnecc_test_subst_a = 0xDEADBEEFUL, /**< Distinctive marker.                */
  k_ra_cnecc_test_subst_b = 0x55AA55AAUL, /**< Alternate marker.                  */
} ra_cnecc_test_subst_t;

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
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  (void)ra_isr_init();
  s_cb_count         = 0U;
  s_cb_last_instance = 0U;
  s_cb_last_is_2bit  = false;
  s_cb_last_addr     = 0U;
  s_cb_last_ctx      = nullptr;
}

static ra_cnecc_config_t make_default_cfg(void)
{
  const ra_cnecc_config_t cfg = {
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
static void test_init_null_cfg(void)
{
  TEST_BEGIN("cnecc init null cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_cnecc_init(nullptr));
  TEST_END("cnecc init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("cnecc init happy");
  prep();

  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));

  for (uint8_t i = (uint8_t)k_ra_cnecc_test_inst_first; i <= (uint8_t)k_ra_cnecc_test_inst_last;
       ++i) {
    volatile r_cnecc_regs_t* reg = ra_cnecc(i);
    TEST_ASSERT_NOT_NULL((void*)reg);
    /* ECERVF must be set (judgment enabled). */
    TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ecervf) != 0U);
    /* Both IRQ enables must be set. */
    TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ec1edic) != 0U);
    TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ec2edic) != 0U);
    /* correct_1bit = true => EC1ECP cleared. */
    TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ec1ecp));
    /* EMCA write-unlock pattern asserted. */
    TEST_ASSERT_EQ((int32_t)k_ra_cnecc_mask_emca_unlock,
                   (int32_t)(reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_emca));
    /* Test mode left disabled. */
    TEST_ASSERT_EQ((int32_t)k_ra_cnecc_mask_test_disable, (int32_t)reg->EC710TMC);
  }
  TEST_END("cnecc init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_no_correction_no_irqs(void)
{
  TEST_BEGIN("cnecc init no correction, no irqs");
  prep();

  ra_cnecc_config_t cfg         = make_default_cfg();
  cfg.instances[0].correct_1bit = false;
  cfg.instances[0].irq_1bit     = false;
  cfg.instances[0].irq_2bit     = false;
  cfg.instances[1].correct_1bit = false;
  cfg.instances[1].irq_1bit     = false;
  cfg.instances[1].irq_2bit     = false;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));

  volatile r_cnecc_regs_t* reg = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* correct_1bit = false => EC1ECP set ("correction NOT executed"). */
  TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ec1ecp) != 0U);
  /* IRQ enables both clear. */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ec1edic));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ec2edic));
  /* ECERVF still set (cfg.enable defaults true via make_default_cfg). */
  TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ecervf) != 0U);
  TEST_END("cnecc init no correction, no irqs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_judgment_disabled(void)
{
  TEST_BEGIN("cnecc init with judgment disabled");
  prep();

  ra_cnecc_config_t cfg   = make_default_cfg();
  cfg.instances[0].enable = false;
  cfg.instances[1].enable = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));

  for (uint8_t i = (uint8_t)k_ra_cnecc_test_inst_first; i <= (uint8_t)k_ra_cnecc_test_inst_last;
       ++i) {
    volatile r_cnecc_regs_t* reg = ra_cnecc(i);
    TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ecervf));
  }
  TEST_END("cnecc init with judgment disabled");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_disable_instance_bounds(void)
{
  TEST_BEGIN("cnecc enable/disable instance bounds");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_enable_instance((uint8_t)k_ra_cnecc_test_inst_bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_disable_instance((uint8_t)k_ra_cnecc_test_inst_huge));
  TEST_END("cnecc enable/disable instance bounds");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_disable_instance_toggle(void)
{
  TEST_BEGIN("cnecc enable/disable per-instance toggles ECERVF");
  prep();

  ra_cnecc_config_t cfg   = make_default_cfg();
  cfg.instances[0].enable = false;
  cfg.instances[1].enable = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_enable_instance((uint8_t)k_ra_cnecc_test_inst_first));
  volatile r_cnecc_regs_t* r0 = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_first);
  volatile r_cnecc_regs_t* r1 = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_last);
  TEST_ASSERT((r0->EC710CTL & (uint32_t)k_ra_cnecc_mask_ecervf) != 0U);
  /* Other instance still disabled. */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(r1->EC710CTL & (uint32_t)k_ra_cnecc_mask_ecervf));
  /* RMW preserved the EMCA unlock pattern. */
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_mask_emca_unlock,
                 (int32_t)(r0->EC710CTL & (uint32_t)k_ra_cnecc_mask_emca));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_disable_instance((uint8_t)k_ra_cnecc_test_inst_first));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(r0->EC710CTL & (uint32_t)k_ra_cnecc_mask_ecervf));
  TEST_END("cnecc enable/disable per-instance toggles ECERVF");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_irq_enables(void)
{
  TEST_BEGIN("cnecc set_irq_enables");
  prep();

  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_set_irq_enables((uint8_t)k_ra_cnecc_test_inst_bad, true, true));

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_cnecc_set_irq_enables((uint8_t)k_ra_cnecc_test_inst_first, false, true));
  volatile r_cnecc_regs_t* reg = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_first);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ec1edic));
  TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ec2edic) != 0U);

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_cnecc_set_irq_enables((uint8_t)k_ra_cnecc_test_inst_first, true, false));
  TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ec1edic) != 0U);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ec2edic));
  /* ECERVF preserved across the RMW. */
  TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ecervf) != 0U);
  TEST_END("cnecc set_irq_enables");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_correction_permission(void)
{
  TEST_BEGIN("cnecc set_correction_permission");
  prep();

  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));

  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_cnecc_set_correction_permission((uint8_t)k_ra_cnecc_test_inst_bad, true));

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_cnecc_set_correction_permission((uint8_t)k_ra_cnecc_test_inst_last, false));
  volatile r_cnecc_regs_t* reg = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_last);
  TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ec1ecp) != 0U);

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_cnecc_set_correction_permission((uint8_t)k_ra_cnecc_test_inst_last, true));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ec1ecp));
  TEST_END("cnecc set_correction_permission");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status_null_out(void)
{
  TEST_BEGIN("cnecc get_status null out");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_cnecc_get_status((uint8_t)k_ra_cnecc_test_inst_first, nullptr));
  TEST_END("cnecc get_status null out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status_bad_instance(void)
{
  TEST_BEGIN("cnecc get_status bad instance");
  prep();
  ra_cnecc_status_t s = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_get_status((uint8_t)k_ra_cnecc_test_inst_bad, &s));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_get_status((uint8_t)k_ra_cnecc_test_inst_huge, &s));
  TEST_END("cnecc get_status bad instance");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status_decodes_flags(void)
{
  TEST_BEGIN("cnecc get_status decodes flags");
  prep();

  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));

  /* Force a 1-bit error + captured address into the simulator. */
  volatile r_cnecc_regs_t* reg = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->EC710CTL  = (uint32_t)k_ra_cnecc_mask_ecer1f | (uint32_t)k_ra_cnecc_mask_ecsedf0 |
                   (uint32_t)k_ra_cnecc_mask_ecervf | (uint32_t)k_ra_cnecc_mask_ec1edic |
                   (uint32_t)k_ra_cnecc_mask_ecemf;
  reg->EC710EAD0 = (uint32_t)k_ra_cnecc_test_addr_a;

  ra_cnecc_status_t s = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_get_status((uint8_t)k_ra_cnecc_test_inst_first, &s));
  TEST_ASSERT(s.err_present);
  TEST_ASSERT(s.err_1bit);
  TEST_ASSERT(!s.err_2bit);
  TEST_ASSERT(!s.overflow);
  TEST_ASSERT(s.addr_is_1bit);
  TEST_ASSERT(!s.addr_is_2bit);
  TEST_ASSERT(s.judgment_active);
  TEST_ASSERT(s.correct_enabled);
  TEST_ASSERT(s.irq1_enabled);
  TEST_ASSERT(!s.irq2_enabled);
  TEST_ASSERT(!s.test_mode);
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_test_addr_a, (int32_t)s.last_addr);

  /* Now force a 2-bit + overflow scenario with EC1ECP set. */
  reg->EC710CTL  = (uint32_t)k_ra_cnecc_mask_ecer2f | (uint32_t)k_ra_cnecc_mask_ecdedf0 |
                   (uint32_t)k_ra_cnecc_mask_ecovff | (uint32_t)k_ra_cnecc_mask_ecervf |
                   (uint32_t)k_ra_cnecc_mask_ec1ecp | (uint32_t)k_ra_cnecc_mask_ec2edic;
  reg->EC710EAD0 = (uint32_t)k_ra_cnecc_test_addr_b;

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_get_status((uint8_t)k_ra_cnecc_test_inst_first, &s));
  TEST_ASSERT(!s.err_1bit);
  TEST_ASSERT(s.err_2bit);
  TEST_ASSERT(s.overflow);
  TEST_ASSERT(!s.addr_is_1bit);
  TEST_ASSERT(s.addr_is_2bit);
  TEST_ASSERT(!s.correct_enabled); /* EC1ECP set => correction off. */
  TEST_ASSERT(s.irq2_enabled);
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_test_addr_b, (int32_t)s.last_addr);
  TEST_END("cnecc get_status decodes flags");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status_masks_ead(void)
{
  TEST_BEGIN("cnecc get_status masks ECEAD to 10 bits");
  prep();
  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));

  volatile r_cnecc_regs_t* reg = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_first);
  reg->EC710EAD0               = (uint32_t)k_ra_cnecc_test_addr_over | (uint32_t)0xFFFFFC00UL;
  ra_cnecc_status_t s          = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_get_status((uint8_t)k_ra_cnecc_test_inst_first, &s));
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_cnecc_test_addr_over & (uint32_t)k_ra_cnecc_mask_ecead),
                 (int32_t)s.last_addr);
  /* The driver must never report a value above k_ra_cnecc_ead_max. */
  TEST_ASSERT(s.last_addr <= (uint16_t)k_ra_cnecc_ead_max);
  TEST_END("cnecc get_status masks ECEAD to 10 bits");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_counters(void)
{
  TEST_BEGIN("cnecc get_counters");
  prep();
  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));

  ra_cnecc_counters_t c = {.one_bit_count = 1U, .two_bit_count = 2U, .overflow_count = 3U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_cnecc_get_counters((uint8_t)k_ra_cnecc_test_inst_first, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_get_counters((uint8_t)k_ra_cnecc_test_inst_bad, &c));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_get_counters((uint8_t)k_ra_cnecc_test_inst_first, &c));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)c.one_bit_count);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)c.two_bit_count);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)c.overflow_count);
  TEST_END("cnecc get_counters");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset_counters(void)
{
  TEST_BEGIN("cnecc reset_counters");
  prep();
  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_attach_handler(stub_cnecc_cb, nullptr));

  ra_cnecc_dispatch((uint8_t)k_ra_cnecc_test_inst_first, false, (uint16_t)k_ra_cnecc_test_addr_a);
  ra_cnecc_dispatch((uint8_t)k_ra_cnecc_test_inst_first, true, (uint16_t)k_ra_cnecc_test_addr_b);
  ra_cnecc_dispatch_overflow((uint8_t)k_ra_cnecc_test_inst_first);

  ra_cnecc_counters_t before = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_get_counters((uint8_t)k_ra_cnecc_test_inst_first, &before));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)before.one_bit_count);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)before.two_bit_count);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)before.overflow_count);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_reset_counters((uint8_t)k_ra_cnecc_test_inst_huge));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_reset_counters((uint8_t)k_ra_cnecc_test_inst_first));

  ra_cnecc_counters_t after = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_get_counters((uint8_t)k_ra_cnecc_test_inst_first, &after));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)after.one_bit_count);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)after.two_bit_count);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)after.overflow_count);
  TEST_END("cnecc reset_counters");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_bbr_mirror(void)
{
  TEST_BEGIN("cnecc bbr mirror tracks dispatch + reset");
  prep();
  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_attach_handler(stub_cnecc_cb, nullptr));

  ra_cnecc_counters_t bbr0 = {.one_bit_count = 9U, .two_bit_count = 9U, .overflow_count = 9U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_set_counter_mirror((uint8_t)k_ra_cnecc_test_inst_huge, &bbr0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_set_counter_mirror((uint8_t)k_ra_cnecc_test_inst_first, &bbr0));
  /* Mirror is seeded from current driver-local counters (zero post-init). */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)bbr0.one_bit_count);

  ra_cnecc_dispatch((uint8_t)k_ra_cnecc_test_inst_first, false, (uint16_t)k_ra_cnecc_test_addr_a);
  ra_cnecc_dispatch((uint8_t)k_ra_cnecc_test_inst_first, true, (uint16_t)k_ra_cnecc_test_addr_a);
  ra_cnecc_dispatch_overflow((uint8_t)k_ra_cnecc_test_inst_first);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)bbr0.one_bit_count);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)bbr0.two_bit_count);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)bbr0.overflow_count);

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_reset_counters((uint8_t)k_ra_cnecc_test_inst_first));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)bbr0.one_bit_count);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)bbr0.two_bit_count);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)bbr0.overflow_count);

  /* Detach -- subsequent dispatches no longer touch bbr0. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_cnecc_set_counter_mirror((uint8_t)k_ra_cnecc_test_inst_first, nullptr));
  ra_cnecc_dispatch((uint8_t)k_ra_cnecc_test_inst_first, false, (uint16_t)k_ra_cnecc_test_addr_a);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)bbr0.one_bit_count);
  TEST_END("cnecc bbr mirror tracks dispatch + reset");
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

  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));

  volatile r_cnecc_regs_t* reg = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* Pretend hardware latched a fault. */
  reg->EC710CTL  = (uint32_t)k_ra_cnecc_mask_ecer2f | (uint32_t)k_ra_cnecc_mask_ecdedf0;
  reg->EC710EAD0 = (uint32_t)k_ra_cnecc_test_addr_a;

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_clear_status((uint8_t)k_ra_cnecc_test_inst_first));

  /* Sim mmap is dumb storage -- our clear writes the bundled
   * clear-all mask straight into EC710CTL. */
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_mask_clear_all, (int32_t)reg->EC710CTL);
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
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_clear_status((uint8_t)k_ra_cnecc_test_inst_bad));
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
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_cnecc_attach_handler(nullptr, nullptr));
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
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_attach_handler(stub_cnecc_cb, nullptr));
  ra_cnecc_dispatch((uint8_t)k_ra_cnecc_test_inst_bad, false, 0U);
  ra_cnecc_dispatch((uint8_t)k_ra_cnecc_test_inst_huge, true, 0U);
  ra_cnecc_dispatch_overflow((uint8_t)k_ra_cnecc_test_inst_huge);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s_cb_count);
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

  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));
  void* const expected_ctx = (void*)(uintptr_t)0xC0FFEEU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_attach_handler(stub_cnecc_cb, expected_ctx));

  ra_cnecc_dispatch((uint8_t)k_ra_cnecc_test_inst_first, false, (uint16_t)k_ra_cnecc_test_addr_a);
  ra_cnecc_dispatch((uint8_t)k_ra_cnecc_test_inst_first, true, (uint16_t)k_ra_cnecc_test_addr_b);
  ra_cnecc_dispatch((uint8_t)k_ra_cnecc_test_inst_last, true, (uint16_t)k_ra_cnecc_test_addr_b);

  TEST_ASSERT_EQ((int32_t)3, (int32_t)s_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_test_inst_last, (int32_t)s_cb_last_instance);
  TEST_ASSERT(s_cb_last_is_2bit);
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_test_addr_b, (int32_t)s_cb_last_addr);
  TEST_ASSERT(s_cb_last_ctx == expected_ctx);

  /* Driver-local counters should reflect the dispatch sequence. */
  ra_cnecc_status_t s0 = {};
  ra_cnecc_status_t s1 = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_get_status((uint8_t)k_ra_cnecc_test_inst_first, &s0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_get_status((uint8_t)k_ra_cnecc_test_inst_last, &s1));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s0.one_bit_count);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s0.two_bit_count);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s1.one_bit_count);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s1.two_bit_count);
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
  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_cnecc_inject_fault((uint8_t)k_ra_cnecc_test_inst_first, nullptr));
  const ra_cnecc_inject_t req = {
    .substitute   = (uint32_t)k_ra_cnecc_test_subst_a,
    .one_bit_flip = true,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_inject_fault((uint8_t)k_ra_cnecc_test_inst_huge, &req));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_inject_fault((uint8_t)k_ra_cnecc_test_inst_first, &req));
  volatile r_cnecc_regs_t* reg = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_first);
  /* Final TMC value is 0x8082 (substitute select). */
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_mask_test_subst, (int32_t)reg->EC710TMC);
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_test_subst_a, (int32_t)reg->EC710TED);
  /* TED for the other instance untouched. */
  volatile r_cnecc_regs_t* other = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_last);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)other->EC710TED);

  /* Now run the 2-bit variant on the other instance. */
  const ra_cnecc_inject_t req2 = {
    .substitute   = (uint32_t)k_ra_cnecc_test_subst_b,
    .one_bit_flip = false,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_inject_fault((uint8_t)k_ra_cnecc_test_inst_last, &req2));
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_mask_test_subst, (int32_t)other->EC710TMC);
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_test_subst_b, (int32_t)other->EC710TED);
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
  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_test_mode_disable((uint8_t)k_ra_cnecc_test_inst_huge));
  bool active = false;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_cnecc_test_mode_active((uint8_t)k_ra_cnecc_test_inst_first, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_test_mode_active((uint8_t)k_ra_cnecc_test_inst_huge, &active));

  /* Arm injection then query -> active true. */
  const ra_cnecc_inject_t req = {
    .substitute   = (uint32_t)k_ra_cnecc_test_subst_a,
    .one_bit_flip = true,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_inject_fault((uint8_t)k_ra_cnecc_test_inst_first, &req));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_test_mode_active((uint8_t)k_ra_cnecc_test_inst_first, &active));
  TEST_ASSERT(active);

  /* Disable test mode and re-query -> false. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_test_mode_disable((uint8_t)k_ra_cnecc_test_inst_first));
  volatile r_cnecc_regs_t* reg = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_first);
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_mask_test_disable, (int32_t)reg->EC710TMC);
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_test_mode_active((uint8_t)k_ra_cnecc_test_inst_first, &active));
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
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_attach_isr((uint8_t)k_ra_cnecc_test_prio_bad));
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
  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_attach_handler(stub_cnecc_cb, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_attach_isr((uint8_t)k_ra_cnecc_test_prio));

  /* Both events must now resolve to allocated slots. */
  uint16_t s0 = (uint16_t)k_ra_isr_slot_none;
  uint16_t s1 = (uint16_t)k_ra_isr_slot_none;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_isr_lookup_slot(k_ra_elc_event_can0_mram_eri, &s0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_isr_lookup_slot(k_ra_elc_event_can1_mram_eri, &s1));
  TEST_ASSERT(s0 != (uint16_t)k_ra_isr_slot_none);
  TEST_ASSERT(s1 != (uint16_t)k_ra_isr_slot_none);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_detach_isr());
  uint16_t s0_after = (uint16_t)k_ra_isr_slot_none;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_isr_lookup_slot(k_ra_elc_event_can0_mram_eri, &s0_after));
  TEST_ASSERT_EQ((int32_t)k_ra_isr_slot_none, (int32_t)s0_after);
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
  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_attach_handler(stub_cnecc_cb, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_attach_isr((uint8_t)k_ra_cnecc_test_prio));

  /* Simulate a 2-bit + overflow event on instance 1. */
  volatile r_cnecc_regs_t* reg = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_last);
  reg->EC710CTL  = (uint32_t)k_ra_cnecc_mask_ecer2f | (uint32_t)k_ra_cnecc_mask_ecdedf0 |
                   (uint32_t)k_ra_cnecc_mask_ecovff | (uint32_t)k_ra_cnecc_mask_ecervf;
  reg->EC710EAD0 = (uint32_t)k_ra_cnecc_test_addr_b;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_irq_fire(k_ra_elc_event_can1_mram_eri));

  /* Callback fired exactly once with 2-bit + correct address. */
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_test_inst_last, (int32_t)s_cb_last_instance);
  TEST_ASSERT(s_cb_last_is_2bit);
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_test_addr_b, (int32_t)s_cb_last_addr);
  /* Latched flags W0C-cleared. */
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_mask_clear_all, (int32_t)reg->EC710CTL);

  /* Now simulate a 1-bit fault on instance 0. */
  volatile r_cnecc_regs_t* reg0 = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_first);
  reg0->EC710CTL  = (uint32_t)k_ra_cnecc_mask_ecer1f | (uint32_t)k_ra_cnecc_mask_ecsedf0 |
                    (uint32_t)k_ra_cnecc_mask_ecervf;
  reg0->EC710EAD0 = (uint32_t)k_ra_cnecc_test_addr_a;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_irq_fire(k_ra_elc_event_can0_mram_eri));
  TEST_ASSERT_EQ((int32_t)2, (int32_t)s_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_cnecc_test_inst_first, (int32_t)s_cb_last_instance);
  TEST_ASSERT(!s_cb_last_is_2bit);

  /* Counters reflect both events (and the overflow bump). */
  ra_cnecc_counters_t c0 = {};
  ra_cnecc_counters_t c1 = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_get_counters((uint8_t)k_ra_cnecc_test_inst_first, &c0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_get_counters((uint8_t)k_ra_cnecc_test_inst_last, &c1));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)c0.one_bit_count);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)c0.two_bit_count);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)c1.two_bit_count);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)c1.overflow_count);
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
  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));

  /* Simulate latched faults before the standby trip. */
  volatile r_cnecc_regs_t* reg0 = ra_cnecc((uint8_t)k_ra_cnecc_test_inst_first);
  reg0->EC710CTL                = reg0->EC710CTL | (uint32_t)k_ra_cnecc_mask_ecer1f;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_enter_standby());
  /* ECERVF cleared after standby prep. */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg0->EC710CTL & (uint32_t)k_ra_cnecc_mask_ecervf));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_exit_standby());
  /* exit_standby replays cfg, ECERVF re-asserted. */
  TEST_ASSERT((reg0->EC710CTL & (uint32_t)k_ra_cnecc_mask_ecervf) != 0U);
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
  /* The 'initialised' flag flips back to false on deinit. We need to
   * deinit first to test the precondition path. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_deinit());
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_cnecc_exit_standby());
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

  const ra_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_deinit());

  for (uint8_t i = (uint8_t)k_ra_cnecc_test_inst_first; i <= (uint8_t)k_ra_cnecc_test_inst_last;
       ++i) {
    volatile r_cnecc_regs_t* reg = ra_cnecc(i);
    TEST_ASSERT_NOT_NULL((void*)reg);
    /* ECERVF must be cleared after deinit. */
    TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ecervf));
    /* Test mode disabled after deinit. */
    TEST_ASSERT_EQ((int32_t)k_ra_cnecc_mask_test_disable, (int32_t)reg->EC710TMC);
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
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cnecc_open());
  for (uint8_t i = (uint8_t)k_ra_cnecc_test_inst_first; i <= (uint8_t)k_ra_cnecc_test_inst_last;
       ++i) {
    volatile r_cnecc_regs_t* reg = ra_cnecc(i);
    TEST_ASSERT_NOT_NULL((void*)reg);
    TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra_cnecc_mask_ecervf) != 0U);
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
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_compute((uint32_t)(uintptr_t)buf_a, sizeof(buf_a), &ecc_a));
  /* Re-running over the same data must yield the same result. */
  uint32_t ecc_b = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_compute((uint32_t)(uintptr_t)buf_a, sizeof(buf_a), &ecc_b));
  TEST_ASSERT_EQ((int32_t)ecc_a, (int32_t)ecc_b);
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
  static const uint32_t buf[1] = {0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_cnecc_compute((uint32_t)(uintptr_t)buf, sizeof(buf), nullptr));
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
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_cnecc_compute(0x1001U, 16U, &ecc));
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
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_cnecc_compute((uint32_t)(uintptr_t)&buf, 2U, &ecc));
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
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_cnecc_compute(0U, 16U, &ecc));
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
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_compute((uint32_t)(uintptr_t)buf, sizeof(buf), &ecc));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_cnecc_verify((uint32_t)(uintptr_t)buf, sizeof(buf), ecc));
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
  TEST_ASSERT_EQ((int32_t)k_ra_err_crc_mismatch,
                 (int32_t)ra_cnecc_verify((uint32_t)(uintptr_t)buf, sizeof(buf), 0xBADBADU));
  TEST_END("cnecc verify rejects mismatching ecc");
}

int32_t main(void)
{
  test_init_null_cfg();
  test_init_happy();
  test_init_no_correction_no_irqs();
  test_init_judgment_disabled();
  test_enable_disable_instance_bounds();
  test_enable_disable_instance_toggle();
  test_set_irq_enables();
  test_set_correction_permission();
  test_get_status_null_out();
  test_get_status_bad_instance();
  test_get_status_decodes_flags();
  test_get_status_masks_ead();
  test_get_counters();
  test_reset_counters();
  test_bbr_mirror();
  test_clear_status_writes_clear_mask();
  test_clear_status_bad_instance();
  test_attach_handler_null();
  test_dispatch_drops_bad_instance();
  test_dispatch_invokes_callback_and_counts();
  test_inject_fault_writes_sequence();
  test_test_mode_disable_and_query();
  test_attach_isr_bad_priority();
  test_attach_isr_routes_both_vectors();
  test_isr_handler_dispatches_and_clears();
  test_enter_exit_standby();
  test_exit_standby_without_init();
  test_deinit_clears_judgment();
  (void)fprintf(stderr, "[OK  ] test_ra_cnecc.c\n");
  return 0;
}
