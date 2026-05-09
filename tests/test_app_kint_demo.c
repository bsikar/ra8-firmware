/**
 * @file test_app_kint_demo.c
 * @brief Integration test: KINT (IRQ pin) input demo bring-up
 *
 * @details
 * Mirrors examples/ek_ra8d2/kint_demo/main.c bring-up flow:
 * ra_icu_init -> ra_board_sw_init(SW1) -> ra_icu_configure_irq_pin
 * (falling-edge, filter on, IRQ13). Switch read returns
 * released by default in the host shim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_err.h"
#include "ra_icu.h"
#include "ra_pin_validator.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_kint_app_irq_chan     = 13U, /**< SW1 -> IRQ13-DS. */
  k_test_kint_app_irq_chan_bad = 33U,
} test_kint_app_chan_t;

static void reset_world(void)
{
  ra_sim_mmap_reset();
  ra_pin_validator_reset();
}

/**
 * @brief Golden bring-up: icu_init + sw_init + configure_irq_pin.
 *
 * @par MC/DC:
 * Compound decision in app: ``icu_init != ok || sw_init != ok ||
 * configure_irq_pin != ok``. Three atomic conditions x N+1 = 4
 * vectors -- golden (this) + null cfg + bad chan + sw_read NULL.
 */
static void test_kint_app_arm_ok(void)
{
  reset_world();
  TEST_BEGIN("kint_demo: arm SW1 falling-edge IRQ");
  TEST_ASSERT_EQ(k_ra_ok, ra_icu_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_board_sw_init(k_ra_board_sw1));
  const ra_icu_irq_cfg_t cfg = {
    .sense      = k_ra_icu_irqmd_falling,
    .filter_div = k_ra_icu_fclksel_pclkb_64,
    .filter_en  = true,
  };
  TEST_ASSERT_EQ(k_ra_ok, ra_icu_configure_irq_pin((uint8_t)k_test_kint_app_irq_chan, &cfg));
  TEST_END("kint_demo: arm SW1 falling-edge IRQ");
}

/**
 * @brief NULL cfg rejected by ra_icu_configure_irq_pin.
 *
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors --
 * golden (non-NULL) + this NULL.
 */
static void test_kint_app_cfg_null(void)
{
  reset_world();
  TEST_BEGIN("kint_demo: NULL cfg rejected");
  TEST_ASSERT_EQ(k_ra_ok, ra_icu_init());
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_icu_configure_irq_pin((uint8_t)k_test_kint_app_irq_chan, nullptr));
  TEST_END("kint_demo: NULL cfg rejected");
}

/**
 * @brief Out-of-range IRQ channel rejected.
 *
 * @par MC/DC:
 * Decision: ``irq_num < 32``. One atomic condition x 2 vectors --
 * golden (in-range) + this out-of-range.
 */
static void test_kint_app_bad_chan(void)
{
  reset_world();
  TEST_BEGIN("kint_demo: bad IRQ chan rejected");
  TEST_ASSERT_EQ(k_ra_ok, ra_icu_init());
  const ra_icu_irq_cfg_t cfg = {
    .sense      = k_ra_icu_irqmd_falling,
    .filter_div = k_ra_icu_fclksel_pclkb_64,
    .filter_en  = true,
  };
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_icu_configure_irq_pin((uint8_t)k_test_kint_app_irq_chan_bad, &cfg));
  TEST_END("kint_demo: bad IRQ chan rejected");
}

/**
 * @brief NULL out_pressed rejected by ra_board_sw_read.
 *
 * @par MC/DC:
 * Decision: ``out_pressed == nullptr``. One atomic condition x 2
 * vectors -- golden (non-NULL) + this NULL.
 */
static void test_kint_app_sw_read_null(void)
{
  reset_world();
  TEST_BEGIN("kint_demo: sw_read NULL rejected");
  TEST_ASSERT_EQ(k_ra_ok, ra_board_sw_init(k_ra_board_sw1));
  TEST_ASSERT(ra_board_sw_read(k_ra_board_sw1, nullptr) != k_ra_ok);
  TEST_END("kint_demo: sw_read NULL rejected");
}

int main(void)
{
  test_kint_app_arm_ok();
  test_kint_app_cfg_null();
  test_kint_app_bad_chan();
  test_kint_app_sw_read_null();
  return 0;
}
