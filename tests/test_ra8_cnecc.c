/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_cnecc.c
 * @brief Unit tests for ra8_cnecc.c (CANFD ECC driver)
 *
 * @details
 * Covers every public API entry point in ``ra8_cnecc.h`` plus every
 * register field documented in HUM Ch 42 (EC710CTL bits, EC710TMC
 * bits, EC710TED, EC710EAD0). Each test acquires a fresh fake mmap
 * via ``prep`` so register state never leaks between cases. This
 * sibling owns the init / status / counter contract tests; the
 * clear-status, dispatch, fault-injection, ISR, standby, deinit, and
 * open / compute / verify tests live in test_ra8_cnecc_isr.c.
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
 * @enum cnecc_fixture_t
 * @brief The payload generators and their seeds.
 */
typedef enum : uint8_t {
  k_cnecc_counter_seed =
    9U, /**< Seed in every ECC counter; only a cleared counter differs from a wrong one. */
} cnecc_fixture_t;

/**
 * @enum cnecc_addr_t
 * @brief High address bits that push the fault address outside the monitored region.
 */
typedef enum : uint32_t {
  k_cnecc_addr_high_bits =
    0xFFFFFC00UL, /**< High bits ORed in, pushing the address outside the monitored range. */
} cnecc_addr_t;

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
static void test_init_null_cfg(void)
{
  TEST_BEGIN("cnecc init null cfg");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cnecc_init(nullptr));
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

  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  for (uint8_t i = (uint8_t)k_ra8_cnecc_test_inst_first; i <= (uint8_t)k_ra8_cnecc_test_inst_last;
       ++i) {
    volatile r_cnecc_regs_t* reg = ra8_cnecc(i);
    TEST_ASSERT_NOT_NULL((void*)reg);
    /* ECERVF must be set (judgment enabled). */
    TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ecervf) != 0U);
    /* Both IRQ enables must be set. */
    TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec1edic) != 0U);
    TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec2edic) != 0U);
    /* correct_1bit = true => EC1ECP cleared. */
    TEST_ASSERT_EQ(0, (reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec1ecp));
    /* EMCA write-unlock pattern asserted. */
    TEST_ASSERT_EQ(k_ra8_cnecc_mask_emca_unlock, (reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_emca));
    /* Test mode left disabled. */
    TEST_ASSERT_EQ(k_ra8_cnecc_mask_test_disable, reg->EC710TMC);
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

  ra8_cnecc_config_t cfg        = make_default_cfg();
  cfg.instances[0].correct_1bit = false;
  cfg.instances[0].irq_1bit     = false;
  cfg.instances[0].irq_2bit     = false;
  cfg.instances[1].correct_1bit = false;
  cfg.instances[1].irq_1bit     = false;
  cfg.instances[1].irq_2bit     = false;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  volatile r_cnecc_regs_t* reg = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* correct_1bit = false => EC1ECP set ("correction NOT executed"). */
  TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec1ecp) != 0U);
  /* IRQ enables both clear. */
  TEST_ASSERT_EQ(0, (reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec1edic));
  TEST_ASSERT_EQ(0, (reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec2edic));
  /* ECERVF still set (cfg.enable defaults true via make_default_cfg). */
  TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ecervf) != 0U);
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

  ra8_cnecc_config_t cfg  = make_default_cfg();
  cfg.instances[0].enable = false;
  cfg.instances[1].enable = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  for (uint8_t i = (uint8_t)k_ra8_cnecc_test_inst_first; i <= (uint8_t)k_ra8_cnecc_test_inst_last;
       ++i) {
    volatile r_cnecc_regs_t* reg = ra8_cnecc(i);
    TEST_ASSERT_EQ(0, (reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ecervf));
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
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_enable_instance((uint8_t)k_ra8_cnecc_test_inst_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_disable_instance((uint8_t)k_ra8_cnecc_test_inst_huge));
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

  ra8_cnecc_config_t cfg  = make_default_cfg();
  cfg.instances[0].enable = false;
  cfg.instances[1].enable = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_enable_instance((uint8_t)k_ra8_cnecc_test_inst_first));
  volatile r_cnecc_regs_t* r0 = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_first);
  volatile r_cnecc_regs_t* r1 = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_last);
  TEST_ASSERT((r0->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ecervf) != 0U);
  /* Other instance still disabled. */
  TEST_ASSERT_EQ(0, (r1->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ecervf));
  /* RMW preserved the EMCA unlock pattern. */
  TEST_ASSERT_EQ(k_ra8_cnecc_mask_emca_unlock, (r0->EC710CTL & (uint32_t)k_ra8_cnecc_mask_emca));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_disable_instance((uint8_t)k_ra8_cnecc_test_inst_first));
  TEST_ASSERT_EQ(0, (r0->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ecervf));
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

  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_set_irq_enables((uint8_t)k_ra8_cnecc_test_inst_bad, true, true));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_set_irq_enables((uint8_t)k_ra8_cnecc_test_inst_first, false, true));
  volatile r_cnecc_regs_t* reg = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_first);
  TEST_ASSERT_EQ(0, (reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec1edic));
  TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec2edic) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_set_irq_enables((uint8_t)k_ra8_cnecc_test_inst_first, true, false));
  TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec1edic) != 0U);
  TEST_ASSERT_EQ(0, (reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec2edic));
  /* ECERVF preserved across the RMW. */
  TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ecervf) != 0U);
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

  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_set_correction_permission((uint8_t)k_ra8_cnecc_test_inst_bad, true));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_set_correction_permission((uint8_t)k_ra8_cnecc_test_inst_last, false));
  volatile r_cnecc_regs_t* reg = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_last);
  TEST_ASSERT((reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec1ecp) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_set_correction_permission((uint8_t)k_ra8_cnecc_test_inst_last, true));
  TEST_ASSERT_EQ(0, (reg->EC710CTL & (uint32_t)k_ra8_cnecc_mask_ec1ecp));
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
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_cnecc_get_status((uint8_t)k_ra8_cnecc_test_inst_first, nullptr));
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
  ra8_cnecc_status_t s = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_get_status((uint8_t)k_ra8_cnecc_test_inst_bad, &s));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_get_status((uint8_t)k_ra8_cnecc_test_inst_huge, &s));
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

  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  /* Force a 1-bit error + captured address into the fake. */
  volatile r_cnecc_regs_t* reg = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->EC710CTL  = (uint32_t)k_ra8_cnecc_mask_ecer1f | (uint32_t)k_ra8_cnecc_mask_ecsedf0 |
                   (uint32_t)k_ra8_cnecc_mask_ecervf | (uint32_t)k_ra8_cnecc_mask_ec1edic |
                   (uint32_t)k_ra8_cnecc_mask_ecemf;
  reg->EC710EAD0 = (uint32_t)k_ra8_cnecc_test_addr_a;

  ra8_cnecc_status_t s = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_get_status((uint8_t)k_ra8_cnecc_test_inst_first, &s));
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
  TEST_ASSERT_EQ(k_ra8_cnecc_test_addr_a, s.last_addr);

  /* Now force a 2-bit + overflow scenario with EC1ECP set. */
  reg->EC710CTL  = (uint32_t)k_ra8_cnecc_mask_ecer2f | (uint32_t)k_ra8_cnecc_mask_ecdedf0 |
                   (uint32_t)k_ra8_cnecc_mask_ecovff | (uint32_t)k_ra8_cnecc_mask_ecervf |
                   (uint32_t)k_ra8_cnecc_mask_ec1ecp | (uint32_t)k_ra8_cnecc_mask_ec2edic;
  reg->EC710EAD0 = (uint32_t)k_ra8_cnecc_test_addr_b;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_get_status((uint8_t)k_ra8_cnecc_test_inst_first, &s));
  TEST_ASSERT(!s.err_1bit);
  TEST_ASSERT(s.err_2bit);
  TEST_ASSERT(s.overflow);
  TEST_ASSERT(!s.addr_is_1bit);
  TEST_ASSERT(s.addr_is_2bit);
  TEST_ASSERT(!s.correct_enabled); /* EC1ECP set => correction off. */
  TEST_ASSERT(s.irq2_enabled);
  TEST_ASSERT_EQ(k_ra8_cnecc_test_addr_b, s.last_addr);
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
  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  volatile r_cnecc_regs_t* reg = ra8_cnecc((uint8_t)k_ra8_cnecc_test_inst_first);
  reg->EC710EAD0       = (uint32_t)k_ra8_cnecc_test_addr_over | (uint32_t)k_cnecc_addr_high_bits;
  ra8_cnecc_status_t s = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_get_status((uint8_t)k_ra8_cnecc_test_inst_first, &s));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_cnecc_test_addr_over & (uint32_t)k_ra8_cnecc_mask_ecead),
                 s.last_addr);
  /* The driver must never report a value above k_ra8_cnecc_ead_max. */
  TEST_ASSERT(s.last_addr <= (uint16_t)k_ra8_cnecc_ead_max);
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
  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));

  ra8_cnecc_counters_t c = {.one_bit_count = 1U, .two_bit_count = 2U, .overflow_count = 3U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_cnecc_get_counters((uint8_t)k_ra8_cnecc_test_inst_first, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_get_counters((uint8_t)k_ra8_cnecc_test_inst_bad, &c));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_get_counters((uint8_t)k_ra8_cnecc_test_inst_first, &c));
  TEST_ASSERT_EQ(0, c.one_bit_count);
  TEST_ASSERT_EQ(0, c.two_bit_count);
  TEST_ASSERT_EQ(0, c.overflow_count);
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
  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_attach_handler(stub_cnecc_cb, nullptr));

  ra8_cnecc_dispatch((uint8_t)k_ra8_cnecc_test_inst_first,
                     false,
                     (uint16_t)k_ra8_cnecc_test_addr_a);
  ra8_cnecc_dispatch((uint8_t)k_ra8_cnecc_test_inst_first, true, (uint16_t)k_ra8_cnecc_test_addr_b);
  ra8_cnecc_dispatch_overflow((uint8_t)k_ra8_cnecc_test_inst_first);

  ra8_cnecc_counters_t before = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_get_counters((uint8_t)k_ra8_cnecc_test_inst_first, &before));
  TEST_ASSERT_EQ(1, before.one_bit_count);
  TEST_ASSERT_EQ(1, before.two_bit_count);
  TEST_ASSERT_EQ(1, before.overflow_count);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_reset_counters((uint8_t)k_ra8_cnecc_test_inst_huge));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_reset_counters((uint8_t)k_ra8_cnecc_test_inst_first));

  ra8_cnecc_counters_t after = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_get_counters((uint8_t)k_ra8_cnecc_test_inst_first, &after));
  TEST_ASSERT_EQ(0, after.one_bit_count);
  TEST_ASSERT_EQ(0, after.two_bit_count);
  TEST_ASSERT_EQ(0, after.overflow_count);
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
  const ra8_cnecc_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_attach_handler(stub_cnecc_cb, nullptr));

  ra8_cnecc_counters_t bbr0 = {.one_bit_count  = k_cnecc_counter_seed,
                               .two_bit_count  = k_cnecc_counter_seed,
                               .overflow_count = k_cnecc_counter_seed};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cnecc_set_counter_mirror((uint8_t)k_ra8_cnecc_test_inst_huge, &bbr0));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_set_counter_mirror((uint8_t)k_ra8_cnecc_test_inst_first, &bbr0));
  /* Mirror is seeded from current driver-local counters (zero post-init). */
  TEST_ASSERT_EQ(0, bbr0.one_bit_count);

  ra8_cnecc_dispatch((uint8_t)k_ra8_cnecc_test_inst_first,
                     false,
                     (uint16_t)k_ra8_cnecc_test_addr_a);
  ra8_cnecc_dispatch((uint8_t)k_ra8_cnecc_test_inst_first, true, (uint16_t)k_ra8_cnecc_test_addr_a);
  ra8_cnecc_dispatch_overflow((uint8_t)k_ra8_cnecc_test_inst_first);
  TEST_ASSERT_EQ(1, bbr0.one_bit_count);
  TEST_ASSERT_EQ(1, bbr0.two_bit_count);
  TEST_ASSERT_EQ(1, bbr0.overflow_count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cnecc_reset_counters((uint8_t)k_ra8_cnecc_test_inst_first));
  TEST_ASSERT_EQ(0, bbr0.one_bit_count);
  TEST_ASSERT_EQ(0, bbr0.two_bit_count);
  TEST_ASSERT_EQ(0, bbr0.overflow_count);

  /* Detach -- subsequent dispatches no longer touch bbr0. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cnecc_set_counter_mirror((uint8_t)k_ra8_cnecc_test_inst_first, nullptr));
  ra8_cnecc_dispatch((uint8_t)k_ra8_cnecc_test_inst_first,
                     false,
                     (uint16_t)k_ra8_cnecc_test_addr_a);
  TEST_ASSERT_EQ(0, bbr0.one_bit_count);
  TEST_END("cnecc bbr mirror tracks dispatch + reset");
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
  (void)fprintf(stderr, "[OK  ] test_ra8_cnecc.c\n");
  return 0;
}
