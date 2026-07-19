/**
 * @file test_ra8_lvd.c
 * @brief Unit tests for ra8_lvd.c (Programmable Voltage Detection driver)
 *
 * @details
 * Round-3 coverage: every public entry point in `ra8_lvd.h` is exercised
 * with at least one happy-path test plus the relevant bad-arg paths.
 * Sequencing is verified by reading back the registers through the
 * sim mmap. This sibling owns the init / deinit and single-bit setter
 * contract tests; the runtime-control, dispatch, threshold-sweep, and
 * MC/DC vector tests live in test_ra8_lvd_ctrl.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_lvd.h"
#include "ra8_lvd_internal.h"
#include "ra8_lvd_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum lvd_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint8_t {
  k_lvd_probe_cr1 = 0xA5U, /**< Planted in PVD2 CR1 to prove the write reaches the register. */
} lvd_fixture_t;

/**
 * @enum ra8_lvd_test_const_t
 * @brief Test fixtures (literals used across the test file).
 */
typedef enum : uint32_t {
  k_ra8_lvd_test_bogus_ch = 99U, /**< Channel id outside {1,2,4,5}. */
} ra8_lvd_test_const_t;

static void prep(void)
{
  ra8_sim_mmap_reset();
  /* Always clear the static callbacks so leftover state from a previous
   * test doesn't fire when a later test only verifies registers. */
  (void)ra8_lvd_attach_handler(nullptr, nullptr);
  (void)ra8_lvd_attach_channel_handler(k_ra8_lvd_ch1, nullptr, nullptr);
  (void)ra8_lvd_attach_channel_handler(k_ra8_lvd_ch2, nullptr, nullptr);
}

static ra8_lvd_cfg_t make_cfg(void)
{
  const ra8_lvd_cfg_t cfg = {
    .threshold    = k_ra8_lvd_pvdlvl_2_85v,
    .edge         = k_ra8_lvd_edge_fall,
    .irq_type     = k_ra8_lvd_irq_maskable,
    .response     = k_ra8_lvd_response_interrupt,
    .negate       = k_ra8_lvd_negate_after_voltage,
    .hysteresis   = k_ra8_lvd_hysteresis_lvd,
    .filter_div   = k_ra8_lvd_loco_div_2,
    .filter_en    = true,
    .irq_enable   = true,
    .clear_status = true,
  };
  return cfg;
}

/* =============================================================================
 * init / deinit
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_init_happy_ch1(void)
{
  TEST_BEGIN("lvd init happy ch1");
  prep();

  const ra8_lvd_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  /* HUM 8.2.2 -- PVDE bit + PVDLVL value should be present. */
  const uint8_t cmpcr = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cmpcr_off);
  TEST_ASSERT((cmpcr & (uint8_t)k_ra8_lvd_cmpcr_mask_pvde) != 0U);
  TEST_ASSERT_EQ(k_ra8_lvd_pvdlvl_2_85v, (cmpcr & (uint8_t)k_ra8_lvd_cmpcr_mask_pvdlvl));

  /* HUM 8.2.4 -- CMPE + RIE + bit3 + filter enabled (DFDIS clear). */
  const uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_cmpe) != 0U);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_rie) != 0U);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_bit3) != 0U);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_dfdis) == 0U);

  /* HUM 8.2.6 -- IRQSEL = maskable, IDTSEL = fall. */
  const uint8_t cr1 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr1_off);
  TEST_ASSERT((cr1 & (uint8_t)k_ra8_lvd_cr1_mask_irqsel) != 0U);
  TEST_ASSERT_EQ(k_ra8_lvd_edge_fall, (cr1 & (uint8_t)k_ra8_lvd_cr1_mask_idtsel));
  TEST_END("lvd init happy ch1");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_n_channel_no_cr1(void)
{
  TEST_BEGIN("lvd init n-channel: no CR1 / SR write");
  prep();

  /* Seed the CR1 byte (which lives at the PVD2 address space) with a
   * sentinel and confirm the n-channel init path leaves it alone. */
  *ra8_lvd_reg8(k_ra8_lvd_pvd2_cr1_off) = k_lvd_probe_cr1;

  ra8_lvd_cfg_t cfg = make_cfg();
  cfg.response      = k_ra8_lvd_response_reset;
  cfg.irq_enable    = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch4, &cfg));

  const uint8_t cmpcr = *ra8_lvd_reg8(k_ra8_lvd_pvd4_cmpcr_off);
  TEST_ASSERT((cmpcr & (uint8_t)k_ra8_lvd_cmpcr_mask_pvde) != 0U);
  TEST_ASSERT_EQ(0xA5U, *ra8_lvd_reg8(k_ra8_lvd_pvd2_cr1_off));

  /* The n-channel CR0 must carry the read-as-1 bit-6 reserved value. */
  const uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd4_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_n_bit6) != 0U);
  /* RE bit set because response = reset. */
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_re) != 0U);
  TEST_END("lvd init n-channel: no CR1 / SR write");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_n_channel_rejects_irq_response(void)
{
  TEST_BEGIN("lvd init n-channel rejects IRQ/NMI response");
  prep();

  ra8_lvd_cfg_t cfg = make_cfg();
  cfg.response      = k_ra8_lvd_response_interrupt;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_lvd_channel_init(k_ra8_lvd_ch4, &cfg));

  cfg.response = k_ra8_lvd_response_nmi;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_lvd_channel_init(k_ra8_lvd_ch5, &cfg));
  TEST_END("lvd init n-channel rejects IRQ/NMI response");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("lvd init null cfg");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lvd_channel_init(k_ra8_lvd_ch1, nullptr));
  TEST_END("lvd init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_channel(void)
{
  TEST_BEGIN("lvd init bad channel");
  prep();

  const ra8_lvd_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lvd_channel_init((ra8_lvd_channel_t)k_ra8_lvd_test_bogus_ch, &cfg));
  TEST_END("lvd init bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_threshold(void)
{
  TEST_BEGIN("lvd init bad threshold");
  prep();

  ra8_lvd_cfg_t cfg = make_cfg();
  cfg.threshold     = (ra8_lvd_pvdlvl_t)0x00U; /* below k_ra8_lvd_pvdlvl_min */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  cfg.threshold = (ra8_lvd_pvdlvl_t)0x10U; /* above k_ra8_lvd_pvdlvl_max */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));
  TEST_END("lvd init bad threshold");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_edge(void)
{
  TEST_BEGIN("lvd init bad edge (0b11 prohibited)");
  prep();

  ra8_lvd_cfg_t cfg = make_cfg();
  cfg.edge          = (ra8_lvd_edge_t)0x3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));
  TEST_END("lvd init bad edge (0b11 prohibited)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_filter_div(void)
{
  TEST_BEGIN("lvd init bad filter_div");
  prep();

  ra8_lvd_cfg_t cfg = make_cfg();
  cfg.filter_div    = (ra8_lvd_loco_div_t)0x4U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));
  TEST_END("lvd init bad filter_div");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_rn_rhsel_conflict(void)
{
  TEST_BEGIN("lvd init RN=1 + RHSEL=HVD conflict rejected");
  prep();

  ra8_lvd_cfg_t cfg = make_cfg();
  cfg.hysteresis    = k_ra8_lvd_hysteresis_hvd;
  cfg.negate        = k_ra8_lvd_negate_after_assert;
  cfg.response      = k_ra8_lvd_response_reset_on_rise;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));
  TEST_END("lvd init RN=1 + RHSEL=HVD conflict rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_response_none_no_rie(void)
{
  TEST_BEGIN("lvd init response=none leaves RIE clear");
  prep();

  ra8_lvd_cfg_t cfg = make_cfg();
  cfg.response      = k_ra8_lvd_response_none;
  cfg.irq_enable    = true; /* should be ignored when response=none */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  const uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_rie) == 0U);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_cmpe) != 0U);
  TEST_END("lvd init response=none leaves RIE clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_filter_off_sets_dfdis(void)
{
  TEST_BEGIN("lvd init filter_en=false sets DFDIS");
  prep();

  ra8_lvd_cfg_t cfg = make_cfg();
  cfg.filter_en     = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  const uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_dfdis) != 0U);
  TEST_END("lvd init filter_en=false sets DFDIS");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_reset_response_sets_ri(void)
{
  TEST_BEGIN("lvd init response=reset sets RI on m channel");
  prep();

  ra8_lvd_cfg_t cfg = make_cfg();
  cfg.response      = k_ra8_lvd_response_reset;
  cfg.irq_enable    = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  const uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_ri) != 0U);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_rie) != 0U);
  TEST_END("lvd init response=reset sets RI on m channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_clears_regs(void)
{
  TEST_BEGIN("lvd deinit clears regs");
  prep();

  const ra8_lvd_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch2, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_deinit(k_ra8_lvd_ch2));

  TEST_ASSERT_EQ(0U, *ra8_lvd_reg8(k_ra8_lvd_pvd2_cmpcr_off));
  /* CR0 carries the bit3 always-write-1 marker even at "deinit". */
  const uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd2_cr0_off);
  TEST_ASSERT_EQ(k_ra8_lvd_cr0_mask_bit3, (cr0 & (uint8_t)k_ra8_lvd_cr0_mask_bit3));
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_cmpe) == 0U);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_rie) == 0U);
  TEST_ASSERT_EQ(0U, *ra8_lvd_reg8(k_ra8_lvd_pvd2_cr1_off));
  TEST_ASSERT_EQ(0U, *ra8_lvd_reg8(k_ra8_lvd_pvd2_sr_off));
  TEST_ASSERT_EQ(0U, *ra8_lvd_reg8(k_ra8_lvd_pvd2_fcr_off));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lvd_channel_deinit((ra8_lvd_channel_t)k_ra8_lvd_test_bogus_ch));
  TEST_END("lvd deinit clears regs");
}

/* =============================================================================
 * Threshold / edge / kind setters
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_set_threshold_preserves_pvde(void)
{
  TEST_BEGIN("lvd set_threshold preserves PVDE");
  prep();

  const ra8_lvd_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_threshold(k_ra8_lvd_ch1, k_ra8_lvd_pvdlvl_1_71v));

  const uint8_t cmpcr = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cmpcr_off);
  TEST_ASSERT((cmpcr & (uint8_t)k_ra8_lvd_cmpcr_mask_pvde) != 0U);
  TEST_ASSERT_EQ(k_ra8_lvd_pvdlvl_1_71v, (cmpcr & (uint8_t)k_ra8_lvd_cmpcr_mask_pvdlvl));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lvd_set_threshold(k_ra8_lvd_ch1, (ra8_lvd_pvdlvl_t)0x20U));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_lvd_set_threshold((ra8_lvd_channel_t)k_ra8_lvd_test_bogus_ch, k_ra8_lvd_pvdlvl_2_85v));
  TEST_END("lvd set_threshold preserves PVDE");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_irq_edge(void)
{
  TEST_BEGIN("lvd set_irq_edge happy + reject 0b11 + n-chan unsupported");
  prep();

  const ra8_lvd_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_irq_edge(k_ra8_lvd_ch1, k_ra8_lvd_edge_both));
  const uint8_t cr1 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr1_off);
  TEST_ASSERT_EQ(k_ra8_lvd_edge_both, (cr1 & (uint8_t)k_ra8_lvd_cr1_mask_idtsel));
  /* IRQSEL still set from init (maskable). */
  TEST_ASSERT((cr1 & (uint8_t)k_ra8_lvd_cr1_mask_irqsel) != 0U);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lvd_set_irq_edge(k_ra8_lvd_ch1, (ra8_lvd_edge_t)0x3U));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_lvd_set_irq_edge(k_ra8_lvd_ch4, k_ra8_lvd_edge_rise));
  TEST_END("lvd set_irq_edge happy + reject 0b11 + n-chan unsupported");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_irq_kind(void)
{
  TEST_BEGIN("lvd set_irq_kind nmi/maskable preserves IDTSEL");
  prep();

  ra8_lvd_cfg_t cfg = make_cfg();
  cfg.edge          = k_ra8_lvd_edge_rise;
  cfg.irq_type      = k_ra8_lvd_irq_maskable;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_set_irq_kind(k_ra8_lvd_ch1, k_ra8_lvd_irq_nmi));
  const uint8_t cr1 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr1_off);
  TEST_ASSERT((cr1 & (uint8_t)k_ra8_lvd_cr1_mask_irqsel) == 0U);
  TEST_ASSERT_EQ(k_ra8_lvd_edge_rise, (cr1 & (uint8_t)k_ra8_lvd_cr1_mask_idtsel));

  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_lvd_set_irq_kind(k_ra8_lvd_ch5, k_ra8_lvd_irq_maskable));
  TEST_END("lvd set_irq_kind nmi/maskable preserves IDTSEL");
}

/* =============================================================================
 * IRQ / reset / CMPE single-bit toggles
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_enable_disable_irq(void)
{
  TEST_BEGIN("lvd enable_irq / disable_irq");
  prep();

  ra8_lvd_cfg_t cfg = make_cfg();
  cfg.irq_enable    = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_rie) == 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_enable_irq(k_ra8_lvd_ch1));
  cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_rie) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_disable_irq(k_ra8_lvd_ch1));
  cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_rie) == 0U);

  /* n-channels reject IRQ enable. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_lvd_enable_irq(k_ra8_lvd_ch4));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_lvd_disable_irq(k_ra8_lvd_ch5));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lvd_enable_irq((ra8_lvd_channel_t)k_ra8_lvd_test_bogus_ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lvd_disable_irq((ra8_lvd_channel_t)k_ra8_lvd_test_bogus_ch));
  TEST_END("lvd enable_irq / disable_irq");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_disable_reset_m(void)
{
  TEST_BEGIN("lvd enable_reset / disable_reset (m channel)");
  prep();

  ra8_lvd_cfg_t cfg = make_cfg();
  cfg.response      = k_ra8_lvd_response_none;
  cfg.irq_enable    = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_enable_reset(k_ra8_lvd_ch1));
  uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_ri) != 0U);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_rie) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_disable_reset(k_ra8_lvd_ch1));
  cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_rie) == 0U);
  TEST_END("lvd enable_reset / disable_reset (m channel)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_disable_reset_n(void)
{
  TEST_BEGIN("lvd enable_reset / disable_reset (n channel uses RE)");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_unlock_n_channels());

  ra8_lvd_cfg_t cfg = make_cfg();
  cfg.response      = k_ra8_lvd_response_reset;
  cfg.irq_enable    = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch4, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_enable_reset(k_ra8_lvd_ch4));
  uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd4_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_re) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_disable_reset(k_ra8_lvd_ch4));
  cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd4_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_re) == 0U);
  /* Reserved bit-6 must still be 1. */
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_n_bit6) != 0U);
  TEST_END("lvd enable_reset / disable_reset (n channel uses RE)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_disable_cmpe(void)
{
  TEST_BEGIN("lvd enable_cmpe / disable_cmpe");
  prep();

  ra8_lvd_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_channel_init(k_ra8_lvd_ch1, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_disable_cmpe(k_ra8_lvd_ch1));
  uint8_t cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_cmpe) == 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lvd_enable_cmpe(k_ra8_lvd_ch1));
  cr0 = *ra8_lvd_reg8(k_ra8_lvd_pvd1_cr0_off);
  TEST_ASSERT((cr0 & (uint8_t)k_ra8_lvd_cr0_mask_cmpe) != 0U);

  /* Bad channel still rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lvd_enable_cmpe((ra8_lvd_channel_t)k_ra8_lvd_test_bogus_ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lvd_disable_cmpe((ra8_lvd_channel_t)k_ra8_lvd_test_bogus_ch));
  TEST_END("lvd enable_cmpe / disable_cmpe");
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
  test_init_happy_ch1,
  test_init_n_channel_no_cr1,
  test_init_n_channel_rejects_irq_response,
  test_init_null_cfg,
  test_init_bad_channel,
  test_init_bad_threshold,
  test_init_bad_edge,
  test_init_bad_filter_div,
  test_init_rn_rhsel_conflict,
  test_init_response_none_no_rie,
  test_init_filter_off_sets_dfdis,
  test_init_reset_response_sets_ri,
  test_deinit_clears_regs,
  test_set_threshold_preserves_pvde,
  test_set_irq_edge,
  test_set_irq_kind,
  test_enable_disable_irq,
  test_enable_disable_reset_m,
  test_enable_disable_reset_n,
  test_enable_disable_cmpe,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_lvd.c\n");
  return 0;
}
