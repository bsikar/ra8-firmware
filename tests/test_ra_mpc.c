/**
 * @file test_ra_mpc.c
 * @brief Unit tests for the pin-mux facade (libs/ra_hal/src/ra_mpc.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_pfs_regs.h"
#include "ra_err.h"
#include "ra_mpc.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static void test_route_peripheral_sets_pmr_and_psel(void)
{
  TEST_BEGIN("ra_mpc_route_peripheral: PMR+PSEL set");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mpc_route_peripheral(k_ra_port_1, 5U, k_ra_mpc_psel_sci0));

  volatile const uint32_t* pfs = ra_pfs_pmn(k_ra_port_1, 5U);
  TEST_ASSERT_NOT_NULL((void*)pfs);
  TEST_ASSERT((*pfs & (uint32_t)k_ra_pfs_mask_pmr) != 0U);
  const uint32_t expected_psel =
    ((uint32_t)k_ra_mpc_psel_sci0 << (uint8_t)k_ra_pfs_bit_psel0) & (uint32_t)k_ra_pfs_mask_psel;
  TEST_ASSERT_EQ((int)expected_psel, (int)(*pfs & (uint32_t)k_ra_pfs_mask_psel));

  TEST_END("ra_mpc_route_peripheral: PMR+PSEL set");
}

static void test_set_gpio_output_clears_pmr(void)
{
  TEST_BEGIN("ra_mpc_set_gpio output: clears PMR, sets PDR");
  ra_sim_mmap_reset();

  /* Pollute the register first. */
  volatile uint32_t* pfs = ra_pfs_pmn(k_ra_port_3, 7U);
  TEST_ASSERT_NOT_NULL((void*)pfs);
  *pfs = 0xFFFFFFFFU;

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mpc_set_gpio(k_ra_port_3, 7U, k_ra_mpc_dir_output));

  TEST_ASSERT_EQ((long long)(uint32_t)k_ra_pfs_mask_pdr, (long long)*ra_pfs_pmn(k_ra_port_3, 7U));
  TEST_END("ra_mpc_set_gpio output: clears PMR, sets PDR");
}

static void test_set_gpio_input(void)
{
  TEST_BEGIN("ra_mpc_set_gpio input: all bits clear");
  ra_sim_mmap_reset();
  volatile uint32_t* pfs = ra_pfs_pmn(k_ra_port_2, 0U);
  TEST_ASSERT_NOT_NULL((void*)pfs);
  *pfs = 0xDEADBEEFU;

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mpc_set_gpio(k_ra_port_2, 0U, k_ra_mpc_dir_input));
  TEST_ASSERT_EQ(0, (int)*ra_pfs_pmn(k_ra_port_2, 0U));
  TEST_END("ra_mpc_set_gpio input: all bits clear");
}

static void test_set_analog(void)
{
  TEST_BEGIN("ra_mpc_set_analog: ASEL set");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mpc_set_analog(k_ra_port_0, 1U));
  TEST_ASSERT_EQ((long long)(uint32_t)k_ra_pfs_mask_asel, (long long)*ra_pfs_pmn(k_ra_port_0, 1U));
  TEST_END("ra_mpc_set_analog: ASEL set");
}

static void test_set_irq(void)
{
  TEST_BEGIN("ra_mpc_set_irq: ISEL set");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mpc_set_irq(k_ra_port_4, 2U));
  TEST_ASSERT_EQ((long long)(uint32_t)k_ra_pfs_mask_isel, (long long)*ra_pfs_pmn(k_ra_port_4, 2U));
  TEST_END("ra_mpc_set_irq: ISEL set");
}

static void test_set_pull_up_then_off(void)
{
  TEST_BEGIN("ra_mpc_set_pull: up -> off");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mpc_set_pull(k_ra_port_5, 3U, k_ra_mpc_pull_up));
  volatile const uint32_t* pfs = ra_pfs_pmn(k_ra_port_5, 3U);
  TEST_ASSERT((*pfs & (uint32_t)k_ra_pfs_mask_pcr) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mpc_set_pull(k_ra_port_5, 3U, k_ra_mpc_pull_off));
  TEST_ASSERT_EQ(0, (int)(*pfs & (uint32_t)k_ra_pfs_mask_pcr));
  TEST_END("ra_mpc_set_pull: up -> off");
}

static void test_set_open_drain(void)
{
  TEST_BEGIN("ra_mpc_set_open_drain: NCODR toggled");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mpc_set_open_drain(k_ra_port_6, 4U, true));
  volatile const uint32_t* pfs = ra_pfs_pmn(k_ra_port_6, 4U);
  TEST_ASSERT((*pfs & (uint32_t)k_ra_pfs_mask_ncodr) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mpc_set_open_drain(k_ra_port_6, 4U, false));
  TEST_ASSERT_EQ(0, (int)(*pfs & (uint32_t)k_ra_pfs_mask_ncodr));
  TEST_END("ra_mpc_set_open_drain: NCODR toggled");
}

static void test_read_pfs_null_out(void)
{
  TEST_BEGIN("ra_mpc_read_pfs: NULL out rejected");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_mpc_read_pfs(k_ra_port_0, 0U, nullptr));
  TEST_END("ra_mpc_read_pfs: NULL out rejected");
}

static void test_read_pfs_roundtrip(void)
{
  TEST_BEGIN("ra_mpc_read_pfs: round-trip");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mpc_set_analog(k_ra_port_0, 2U));
  uint32_t v = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mpc_read_pfs(k_ra_port_0, 2U, &v));
  TEST_ASSERT_EQ((long long)(uint32_t)k_ra_pfs_mask_asel, (long long)v);
  TEST_END("ra_mpc_read_pfs: round-trip");
}

static void test_bounds_checks(void)
{
  TEST_BEGIN("ra_mpc: bounds checks");
  ra_sim_mmap_reset();

  const ra_port_t bad_port = (ra_port_t)(k_ra_port_max + 1U);
  const ra_pin_t  bad_pin  = (ra_pin_t)(k_ra_pin_max + 1U);
  TEST_ASSERT_EQ((int)k_ra_err_gpio_invalid_port,
                 (int)ra_mpc_set_gpio(bad_port, 0U, k_ra_mpc_dir_input));
  TEST_ASSERT_EQ((int)k_ra_err_gpio_invalid_pin,
                 (int)ra_mpc_set_gpio(k_ra_port_0, bad_pin, k_ra_mpc_dir_input));

  TEST_ASSERT_EQ((int)k_ra_err_gpio_invalid_port, (int)ra_mpc_set_analog(bad_port, 0U));
  TEST_ASSERT_EQ((int)k_ra_err_gpio_invalid_pin, (int)ra_mpc_set_irq(k_ra_port_0, bad_pin));
  TEST_ASSERT_EQ((int)k_ra_err_gpio_invalid_port,
                 (int)ra_mpc_set_pull(bad_port, 0U, k_ra_mpc_pull_off));
  TEST_ASSERT_EQ((int)k_ra_err_gpio_invalid_pin,
                 (int)ra_mpc_set_open_drain(k_ra_port_0, bad_pin, false));
  TEST_ASSERT_EQ((int)k_ra_err_gpio_invalid_port,
                 (int)ra_mpc_route_peripheral(bad_port, 0U, k_ra_mpc_psel_gpio));
  uint32_t v = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_gpio_invalid_pin, (int)ra_mpc_read_pfs(k_ra_port_0, bad_pin, &v));

  TEST_END("ra_mpc: bounds checks");
}

int main(void)
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
  (void)fprintf(stderr, "[OK  ] test_ra_mpc.c\n");
  return 0;
}
