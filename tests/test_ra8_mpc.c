/**
 * @file test_ra8_mpc.c
 * @brief Unit tests for the pin-mux facade (libs/ra8_hal/src/ra8_mpc.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mpc.h"
#include "ra8_pfs_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum t_pfs_probe_t
 * @brief PFS register patterns proving which bits the MPC driver preserves.
 */
typedef enum : uint32_t {
  k_t_pfs_all_ones = 0xFFFFFFFFU, /**< Every bit set, so a clear is visible.   */
  k_t_pfs_pattern  = 0xDEADBEEFU, /**< A mixed pattern, so a set is visible too. */
} t_pfs_probe_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_route_peripheral_sets_pmr_and_psel(void)
{
  TEST_BEGIN("ra8_mpc_route_peripheral: PMR+PSEL set");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpc_route_peripheral(k_ra8_port_1, 5U, k_ra8_mpc_psel_sci0));

  volatile const uint32_t* pfs = ra8_pfs_pmn(k_ra8_port_1, k_ra8_pin_5);
  TEST_ASSERT_NOT_NULL((void*)pfs);
  TEST_ASSERT((*pfs & (uint32_t)k_ra8_pfs_mask_pmr) != 0U);
  const uint32_t expected_psel =
    ((uint32_t)k_ra8_mpc_psel_sci0 << (uint8_t)k_ra8_pfs_bit_psel0) & (uint32_t)k_ra8_pfs_mask_psel;
  TEST_ASSERT_EQ(expected_psel, (*pfs & (uint32_t)k_ra8_pfs_mask_psel));

  TEST_END("ra8_mpc_route_peripheral: PMR+PSEL set");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_gpio_output_clears_pmr(void)
{
  TEST_BEGIN("ra8_mpc_set_gpio output: clears PMR, sets PDR");
  ra8_sim_mmap_reset();

  /* Pollute the register first. */
  volatile uint32_t* pfs = ra8_pfs_pmn(k_ra8_port_3, k_ra8_pin_7);
  TEST_ASSERT_NOT_NULL((void*)pfs);
  *pfs = k_t_pfs_all_ones;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpc_set_gpio(k_ra8_port_3, 7U, k_ra8_mpc_dir_output));

  TEST_ASSERT_EQ(k_ra8_pfs_mask_pdr, *ra8_pfs_pmn(k_ra8_port_3, 7U));
  TEST_END("ra8_mpc_set_gpio output: clears PMR, sets PDR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_gpio_input(void)
{
  TEST_BEGIN("ra8_mpc_set_gpio input: all bits clear");
  ra8_sim_mmap_reset();
  volatile uint32_t* pfs = ra8_pfs_pmn(k_ra8_port_2, 0U);
  TEST_ASSERT_NOT_NULL((void*)pfs);
  *pfs = k_t_pfs_pattern;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpc_set_gpio(k_ra8_port_2, 0U, k_ra8_mpc_dir_input));
  TEST_ASSERT_EQ(0, *ra8_pfs_pmn(k_ra8_port_2, 0U));
  TEST_END("ra8_mpc_set_gpio input: all bits clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_analog(void)
{
  TEST_BEGIN("ra8_mpc_set_analog: ASEL set");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpc_set_analog(k_ra8_port_0, 1U));
  TEST_ASSERT_EQ(k_ra8_pfs_mask_asel, *ra8_pfs_pmn(k_ra8_port_0, 1U));
  TEST_END("ra8_mpc_set_analog: ASEL set");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_irq(void)
{
  TEST_BEGIN("ra8_mpc_set_irq: ISEL set");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpc_set_irq(k_ra8_port_4, 2U));
  TEST_ASSERT_EQ(k_ra8_pfs_mask_isel, *ra8_pfs_pmn(k_ra8_port_4, 2U));
  TEST_END("ra8_mpc_set_irq: ISEL set");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_pull_up_then_off(void)
{
  TEST_BEGIN("ra8_mpc_set_pull: up -> off");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpc_set_pull(k_ra8_port_5, 3U, k_ra8_mpc_pull_up));
  volatile const uint32_t* pfs = ra8_pfs_pmn(k_ra8_port_5, 3U);
  TEST_ASSERT((*pfs & (uint32_t)k_ra8_pfs_mask_pcr) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpc_set_pull(k_ra8_port_5, 3U, k_ra8_mpc_pull_off));
  TEST_ASSERT_EQ(0, (*pfs & (uint32_t)k_ra8_pfs_mask_pcr));
  TEST_END("ra8_mpc_set_pull: up -> off");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_open_drain(void)
{
  TEST_BEGIN("ra8_mpc_set_open_drain: NCODR toggled");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpc_set_open_drain(k_ra8_port_6, 4U, true));
  volatile const uint32_t* pfs = ra8_pfs_pmn(k_ra8_port_6, 4U);
  TEST_ASSERT((*pfs & (uint32_t)k_ra8_pfs_mask_ncodr) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpc_set_open_drain(k_ra8_port_6, 4U, false));
  TEST_ASSERT_EQ(0, (*pfs & (uint32_t)k_ra8_pfs_mask_ncodr));
  TEST_END("ra8_mpc_set_open_drain: NCODR toggled");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_pfs_null_out(void)
{
  TEST_BEGIN("ra8_mpc_read_pfs: NULL out rejected");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mpc_read_pfs(k_ra8_port_0, 0U, nullptr));
  TEST_END("ra8_mpc_read_pfs: NULL out rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_pfs_roundtrip(void)
{
  TEST_BEGIN("ra8_mpc_read_pfs: round-trip");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpc_set_analog(k_ra8_port_0, 2U));
  uint32_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpc_read_pfs(k_ra8_port_0, 2U, &v));
  TEST_ASSERT_EQ(k_ra8_pfs_mask_asel, v);
  TEST_END("ra8_mpc_read_pfs: round-trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_bounds_checks(void)
{
  TEST_BEGIN("ra8_mpc: bounds checks");
  ra8_sim_mmap_reset();

  const ra8_port_t bad_port = (ra8_port_t)(k_ra8_port_max + 1U);
  const ra8_pin_t  bad_pin  = (ra8_pin_t)(k_ra8_pin_max + 1U);
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_port, ra8_mpc_set_gpio(bad_port, 0U, k_ra8_mpc_dir_input));
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin,
                 ra8_mpc_set_gpio(k_ra8_port_0, bad_pin, k_ra8_mpc_dir_input));

  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_port, ra8_mpc_set_analog(bad_port, 0U));
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin, ra8_mpc_set_irq(k_ra8_port_0, bad_pin));
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_port, ra8_mpc_set_pull(bad_port, 0U, k_ra8_mpc_pull_off));
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin, ra8_mpc_set_open_drain(k_ra8_port_0, bad_pin, false));
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_port,
                 ra8_mpc_route_peripheral(bad_port, 0U, k_ra8_mpc_psel_gpio));
  uint32_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin, ra8_mpc_read_pfs(k_ra8_port_0, bad_pin, &v));

  TEST_END("ra8_mpc: bounds checks");
}

int32_t main(void)
{
  test_route_peripheral_sets_pmr_and_psel();
  test_set_gpio_output_clears_pmr();
  test_set_gpio_input();
  test_set_analog();
  test_set_irq();
  test_set_pull_up_then_off();
  test_set_open_drain();
  test_read_pfs_null_out();
  test_read_pfs_roundtrip();
  test_bounds_checks();
  (void)fprintf(stderr, "[OK  ] test_ra8_mpc.c\n");
  return 0;
}
