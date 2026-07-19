/**
 * @file test_ra8_gpio.c
 * @brief Unit tests for gpio.c (high-level PORT + PFS GPIO driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_icu.h"
#include "ra8_icu_regs.h"
#include "ra8_isr.h"
#include "ra8_pfs_regs.h"
#include "ra8_pin_interface.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_port_regs.h"
#include "ra8_port_utils.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum gpio_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_gpio_irqcr_ff = 0xFFU,
} gpio_uint8_const_t;

/**
 * @enum ra8_gpio_test_ids_t
 * @brief Identifiers used by the GPIO test cases.
 */
typedef enum : uint16_t {
  k_ra8_gpio_test_pin_valid_low  = 0x0000U, /**< Port 0, pin 0.          */
  k_ra8_gpio_test_pin_valid_high = 0x0E0FU, /**< Port 14, pin 15.        */
  k_ra8_gpio_test_pin_alt        = 0x0102U, /**< Port 1, pin 2.          */
  k_ra8_gpio_test_pin_bad_port   = 0x0F00U, /**< Port 15 (out of range). */
  k_ra8_gpio_test_pin_bad_pin    = 0x0010U, /**< Port 0, pin 16 (OOR).   */
} ra8_gpio_test_ids_t;

/* Forward declaration of the concrete DI vtable. */
extern const ra8_pin_interface_t g_ra8_gpio_pin_interface;

/**
 * @brief Reset both the mock MMIO backing and the pin-validator bitmap.
 */
static void reset_state(void)
{
  ra8_sim_mmap_reset();
  ra8_pin_validator_reset();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_output_init_happy_low(void)
{
  TEST_BEGIN("gpio output_init low");
  reset_state();

  const ra8_err_t err =
    ra8_gpio_output_init((ra8_port_pin_t)k_ra8_gpio_test_pin_valid_low, k_ra8_level_low);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  /* PFS must have PDR=1, PODR=0. */
  volatile uint32_t* pfs = ra8_pfs_pmn(k_ra8_port_0, k_ra8_pin_0);
  TEST_ASSERT_NOT_NULL((void*)pfs);
  TEST_ASSERT_EQ(k_ra8_pfs_mask_pdr, *pfs);
  TEST_ASSERT(ra8_pin_validator_is_claimed((ra8_port_pin_t)k_ra8_gpio_test_pin_valid_low));
  TEST_END("gpio output_init low");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_output_init_happy_high(void)
{
  TEST_BEGIN("gpio output_init high");
  reset_state();

  const ra8_err_t err =
    ra8_gpio_output_init((ra8_port_pin_t)k_ra8_gpio_test_pin_valid_high, k_ra8_level_high);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile uint32_t* pfs = ra8_pfs_pmn(k_ra8_port_14, k_ra8_pin_15);
  TEST_ASSERT_NOT_NULL((void*)pfs);
  const uint32_t expected = (uint32_t)k_ra8_pfs_mask_pdr | (uint32_t)k_ra8_pfs_mask_podr;
  TEST_ASSERT_EQ(expected, *pfs);
  TEST_END("gpio output_init high");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_output_init_invalid_port(void)
{
  TEST_BEGIN("gpio output_init invalid port");
  reset_state();

  const ra8_err_t err =
    ra8_gpio_output_init((ra8_port_pin_t)k_ra8_gpio_test_pin_bad_port, k_ra8_level_low);
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_port, err);
  TEST_END("gpio output_init invalid port");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_output_init_invalid_pin(void)
{
  TEST_BEGIN("gpio output_init invalid pin");
  reset_state();

  const ra8_err_t err =
    ra8_gpio_output_init((ra8_port_pin_t)k_ra8_gpio_test_pin_bad_pin, k_ra8_level_low);
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin, err);
  TEST_END("gpio output_init invalid pin");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_output_init_conflict(void)
{
  TEST_BEGIN("gpio output_init conflict");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpio_output_init((ra8_port_pin_t)k_ra8_gpio_test_pin_alt, k_ra8_level_low));
  /* Second claim of the same pin must fail with gpio_conflict. */
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict,
                 ra8_gpio_output_init((ra8_port_pin_t)k_ra8_gpio_test_pin_alt, k_ra8_level_high));
  TEST_END("gpio output_init conflict");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_input_init_no_pull(void)
{
  TEST_BEGIN("gpio input_init no pull");
  reset_state();

  const ra8_err_t err =
    ra8_gpio_input_init((ra8_port_pin_t)k_ra8_gpio_test_pin_valid_low, k_ra8_pull_none);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile uint32_t* pfs = ra8_pfs_pmn(k_ra8_port_0, k_ra8_pin_0);
  TEST_ASSERT_EQ(0, *pfs);
  TEST_END("gpio input_init no pull");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_input_init_pull_up(void)
{
  TEST_BEGIN("gpio input_init pull up");
  reset_state();

  const ra8_err_t err = ra8_gpio_input_init((ra8_port_pin_t)k_ra8_gpio_test_pin_alt, k_ra8_pull_up);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile uint32_t* pfs = ra8_pfs_pmn(k_ra8_port_1, k_ra8_pin_2);
  TEST_ASSERT_EQ(k_ra8_pfs_mask_pcr, *pfs);
  TEST_END("gpio input_init pull up");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_input_init_invalid_port(void)
{
  TEST_BEGIN("gpio input_init invalid port");
  reset_state();

  TEST_ASSERT_EQ(
    k_ra8_err_gpio_invalid_port,
    ra8_gpio_input_init((ra8_port_pin_t)k_ra8_gpio_test_pin_bad_port, k_ra8_pull_none));
  TEST_END("gpio input_init invalid port");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_input_init_invalid_pin(void)
{
  TEST_BEGIN("gpio input_init invalid pin");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin,
                 ra8_gpio_input_init((ra8_port_pin_t)k_ra8_gpio_test_pin_bad_pin, k_ra8_pull_none));
  TEST_END("gpio input_init invalid pin");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_high_sets_posr(void)
{
  TEST_BEGIN("gpio write high sets POSR");
  reset_state();

  const ra8_port_pin_t pin = (ra8_port_pin_t)k_ra8_gpio_test_pin_alt;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpio_write(pin, k_ra8_level_high));

  volatile r_port_regs_t* port = ra8_port(k_ra8_port_1);
  TEST_ASSERT_NOT_NULL((void*)port);
  /* POSR lives in the LOW half of PCNTR3. */
  const uint32_t expected = (uint32_t)(1UL << (uint32_t)k_ra8_pin_2);
  TEST_ASSERT_EQ(expected, port->PCNTR3);
  TEST_END("gpio write high sets POSR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_low_sets_porr(void)
{
  TEST_BEGIN("gpio write low sets PORR");
  reset_state();

  const ra8_port_pin_t pin = (ra8_port_pin_t)k_ra8_gpio_test_pin_alt;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpio_write(pin, k_ra8_level_low));

  volatile r_port_regs_t* port = ra8_port(k_ra8_port_1);
  /* PORR lives in the HIGH half of PCNTR3. */
  const uint32_t expected = (uint32_t)(1UL << (uint32_t)k_ra8_pin_2)
                            << (uint32_t)k_ra8_pcntr_high_half_shift;
  TEST_ASSERT_EQ(expected, port->PCNTR3);
  TEST_END("gpio write low sets PORR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_invalid_port(void)
{
  TEST_BEGIN("gpio write invalid port");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_port,
                 ra8_gpio_write((ra8_port_pin_t)k_ra8_gpio_test_pin_bad_port, k_ra8_level_low));
  TEST_END("gpio write invalid port");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_invalid_pin(void)
{
  TEST_BEGIN("gpio write invalid pin");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin,
                 ra8_gpio_write((ra8_port_pin_t)k_ra8_gpio_test_pin_bad_pin, k_ra8_level_low));
  TEST_END("gpio write invalid pin");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_toggle_when_low(void)
{
  TEST_BEGIN("gpio toggle when low -> high");
  reset_state();

  const ra8_port_pin_t    pin = (ra8_port_pin_t)k_ra8_gpio_test_pin_alt;
  volatile r_port_regs_t* reg = ra8_port(k_ra8_port_1);
  /* PODR = 0 -> toggle should set POSR (low half). */
  reg->PCNTR1 = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpio_toggle(pin));
  const uint32_t expected = (uint32_t)(1UL << (uint32_t)k_ra8_pin_2);
  TEST_ASSERT_EQ(expected, reg->PCNTR3);
  TEST_END("gpio toggle when low -> high");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_toggle_when_high(void)
{
  TEST_BEGIN("gpio toggle when high -> low");
  reset_state();

  const ra8_port_pin_t    pin = (ra8_port_pin_t)k_ra8_gpio_test_pin_alt;
  volatile r_port_regs_t* reg = ra8_port(k_ra8_port_1);
  /* Set PODR bit 2 in the HIGH half of PCNTR1. */
  reg->PCNTR1 = (uint32_t)(1UL << (uint32_t)k_ra8_pin_2) << (uint32_t)k_ra8_pcntr_high_half_shift;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpio_toggle(pin));
  const uint32_t expected = (uint32_t)(1UL << (uint32_t)k_ra8_pin_2)
                            << (uint32_t)k_ra8_pcntr_high_half_shift;
  TEST_ASSERT_EQ(expected, reg->PCNTR3);
  TEST_END("gpio toggle when high -> low");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_toggle_invalid_port(void)
{
  TEST_BEGIN("gpio toggle invalid port");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_port,
                 ra8_gpio_toggle((ra8_port_pin_t)k_ra8_gpio_test_pin_bad_port));
  TEST_END("gpio toggle invalid port");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_toggle_invalid_pin(void)
{
  TEST_BEGIN("gpio toggle invalid pin");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin,
                 ra8_gpio_toggle((ra8_port_pin_t)k_ra8_gpio_test_pin_bad_pin));
  TEST_END("gpio toggle invalid pin");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_high_and_low(void)
{
  TEST_BEGIN("gpio read both levels");
  reset_state();

  const ra8_port_pin_t    pin = (ra8_port_pin_t)k_ra8_gpio_test_pin_alt;
  volatile r_port_regs_t* reg = ra8_port(k_ra8_port_1);

  /* PIDR (low half of PCNTR2) bit 2 set. */
  reg->PCNTR2          = (uint32_t)(1UL << (uint32_t)k_ra8_pin_2);
  ra8_level_t got_high = k_ra8_level_low;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpio_read(pin, &got_high));
  TEST_ASSERT_EQ(k_ra8_level_high, got_high);

  /* And cleared. */
  reg->PCNTR2         = 0U;
  ra8_level_t got_low = k_ra8_level_high;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpio_read(pin, &got_low));
  TEST_ASSERT_EQ(k_ra8_level_low, got_low);
  TEST_END("gpio read both levels");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_null_out(void)
{
  TEST_BEGIN("gpio read null out");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_gpio_read((ra8_port_pin_t)k_ra8_gpio_test_pin_valid_low, nullptr));
  TEST_END("gpio read null out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_invalid_port(void)
{
  TEST_BEGIN("gpio read invalid port");
  reset_state();

  ra8_level_t got = k_ra8_level_low;
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_port,
                 ra8_gpio_read((ra8_port_pin_t)k_ra8_gpio_test_pin_bad_port, &got));
  TEST_END("gpio read invalid port");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_invalid_pin(void)
{
  TEST_BEGIN("gpio read invalid pin");
  reset_state();

  ra8_level_t got = k_ra8_level_low;
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin,
                 ra8_gpio_read((ra8_port_pin_t)k_ra8_gpio_test_pin_bad_pin, &got));
  TEST_END("gpio read invalid pin");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_release_pin(void)
{
  TEST_BEGIN("gpio release pin");
  reset_state();

  const ra8_port_pin_t pin = (ra8_port_pin_t)k_ra8_gpio_test_pin_alt;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpio_output_init(pin, k_ra8_level_low));
  TEST_ASSERT(ra8_pin_validator_is_claimed(pin));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpio_release(pin));
  TEST_ASSERT(!ra8_pin_validator_is_claimed(pin));
  TEST_END("gpio release pin");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_route_peripheral_happy(void)
{
  TEST_BEGIN("pfs route peripheral happy");
  reset_state();

  const ra8_port_pin_t pin = (ra8_port_pin_t)k_ra8_gpio_test_pin_alt;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pfs_route_peripheral(pin, k_ra8_psel_sci_async, "TEST"));

  volatile uint32_t* pfs      = ra8_pfs_pmn(k_ra8_port_1, k_ra8_pin_2);
  const uint32_t     expected = (uint32_t)k_ra8_pfs_mask_pmr |
                                (((uint32_t)k_ra8_psel_sci_async) << (uint32_t)k_ra8_pfs_bit_psel0);
  TEST_ASSERT_EQ(expected, *pfs);
  TEST_END("pfs route peripheral happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_route_peripheral_null_owner(void)
{
  TEST_BEGIN("pfs route peripheral null owner");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_gpio_test_pin_alt,
                                          k_ra8_psel_sci_async,
                                          nullptr));
  TEST_END("pfs route peripheral null owner");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_route_peripheral_invalid_port(void)
{
  TEST_BEGIN("pfs route peripheral invalid port");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_port,
                 ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_gpio_test_pin_bad_port,
                                          k_ra8_psel_sci_async,
                                          "TEST"));
  TEST_END("pfs route peripheral invalid port");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_route_peripheral_invalid_pin(void)
{
  TEST_BEGIN("pfs route peripheral invalid pin");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin,
                 ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_gpio_test_pin_bad_pin,
                                          k_ra8_psel_sci_async,
                                          "TEST"));
  TEST_END("pfs route peripheral invalid pin");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_route_peripheral_conflict(void)
{
  TEST_BEGIN("pfs route peripheral conflict");
  reset_state();

  const ra8_port_pin_t pin = (ra8_port_pin_t)k_ra8_gpio_test_pin_alt;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pfs_route_peripheral(pin, k_ra8_psel_sci_async, "FIRST"));
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict,
                 ra8_pfs_route_peripheral(pin, k_ra8_psel_sci_async, "SECOND"));
  TEST_END("pfs route peripheral conflict");
}

/* ---------------------------------------------------------------------------
 * DI vtable thunks
 * 
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_vtable_output_init_and_write(void)
{
  TEST_BEGIN("vtable output_init + write");
  reset_state();

  const ra8_port_pin_t pin = (ra8_port_pin_t)k_ra8_gpio_test_pin_alt;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    g_ra8_gpio_pin_interface.output_init(g_ra8_gpio_pin_interface.ctx, pin, k_ra8_level_low));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    g_ra8_gpio_pin_interface.write(g_ra8_gpio_pin_interface.ctx, pin, k_ra8_level_high));
  TEST_END("vtable output_init + write");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_vtable_read_and_toggle(void)
{
  TEST_BEGIN("vtable read + toggle");
  reset_state();

  const ra8_port_pin_t pin = (ra8_port_pin_t)k_ra8_gpio_test_pin_alt;
  ra8_level_t          got = k_ra8_level_low;
  TEST_ASSERT_EQ(k_ra8_ok, g_ra8_gpio_pin_interface.read(g_ra8_gpio_pin_interface.ctx, pin, &got));
  TEST_ASSERT_EQ(k_ra8_level_low, got);
  TEST_ASSERT_EQ(k_ra8_ok, g_ra8_gpio_pin_interface.toggle(g_ra8_gpio_pin_interface.ctx, pin));
  TEST_END("vtable read + toggle");
}

/* ---------------------------------------------------------------------------
 * external IRQ attach / detach
 * 
 */

typedef enum : uint8_t {
  k_ra8_gpio_test_irq_num        = 3U,  /**< IRQ3 used by attach tests. */
  k_ra8_gpio_test_irq_bad_num    = 16U, /**< Out-of-range IRQ number.   */
  k_ra8_gpio_test_irq_prio       = 5U,  /**< NVIC priority for tests.   */
  k_ra8_gpio_test_expected_event = 4U,  /**< ICU IRQ3 ELC event.        */
} ra8_gpio_test_irq_ids_t;

static uint32_t s_irq_fire_count;
static void*    s_last_irq_ctx;

static void stub_irq_handler(void* ctx)
{
  ++s_irq_fire_count;
  s_last_irq_ctx = ctx;
}

static void reset_irq_state(void)
{
  reset_state();
  s_irq_fire_count = 0U;
  s_last_irq_ctx   = nullptr;
  (void)ra8_icu_init();
  (void)ra8_isr_init();
}

static ra8_gpio_irq_cfg_t make_irq_cfg(void)
{
  const ra8_gpio_irq_cfg_t cfg = {
    .pull       = k_ra8_pull_up,
    .sense      = k_ra8_icu_irqmd_falling,
    .filter_div = k_ra8_icu_fclksel_pclkb,
    .filter_en  = true,
    .priority   = (uint8_t)k_ra8_gpio_test_irq_prio,
  };
  return cfg;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpio_attach_irq_happy(void)
{
  TEST_BEGIN("gpio attach_irq happy");
  reset_irq_state();

  const ra8_port_pin_t     pin = (ra8_port_pin_t)k_ra8_gpio_test_pin_alt;
  const ra8_gpio_irq_cfg_t cfg = make_irq_cfg();

  const ra8_err_t err = ra8_gpio_attach_irq(pin,
                                            (uint8_t)k_ra8_gpio_test_irq_num,
                                            &cfg,
                                            stub_irq_handler,
                                            (void*)(uintptr_t)0xC0DEU);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  uint8_t irqcr = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_read_irqcr((uint8_t)k_ra8_gpio_test_irq_num, &irqcr));
  TEST_ASSERT(irqcr != 0U);
  TEST_ASSERT(ra8_pin_validator_is_claimed(pin));
  TEST_END("gpio attach_irq happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpio_attach_irq_null_cfg(void)
{
  TEST_BEGIN("gpio attach_irq null cfg");
  reset_irq_state();

  const ra8_err_t err = ra8_gpio_attach_irq((ra8_port_pin_t)k_ra8_gpio_test_pin_alt,
                                            (uint8_t)k_ra8_gpio_test_irq_num,
                                            nullptr,
                                            stub_irq_handler,
                                            nullptr);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, err);
  TEST_END("gpio attach_irq null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpio_attach_irq_null_handler(void)
{
  TEST_BEGIN("gpio attach_irq null handler");
  reset_irq_state();

  const ra8_gpio_irq_cfg_t cfg = make_irq_cfg();
  const ra8_err_t          err = ra8_gpio_attach_irq((ra8_port_pin_t)k_ra8_gpio_test_pin_alt,
                                                     (uint8_t)k_ra8_gpio_test_irq_num,
                                                     &cfg,
                                                     nullptr,
                                                     nullptr);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, err);
  TEST_END("gpio attach_irq null handler");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpio_attach_irq_bad_num(void)
{
  TEST_BEGIN("gpio attach_irq bad irq num");
  reset_irq_state();

  const ra8_gpio_irq_cfg_t cfg = make_irq_cfg();
  const ra8_err_t          err = ra8_gpio_attach_irq((ra8_port_pin_t)k_ra8_gpio_test_pin_alt,
                                                     (uint8_t)k_ra8_gpio_test_irq_bad_num,
                                                     &cfg,
                                                     stub_irq_handler,
                                                     nullptr);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, err);
  TEST_END("gpio attach_irq bad irq num");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpio_attach_irq_bad_pin(void)
{
  TEST_BEGIN("gpio attach_irq bad pin");
  reset_irq_state();

  const ra8_gpio_irq_cfg_t cfg = make_irq_cfg();
  const ra8_err_t          err = ra8_gpio_attach_irq((ra8_port_pin_t)k_ra8_gpio_test_pin_bad_port,
                                                     (uint8_t)k_ra8_gpio_test_irq_num,
                                                     &cfg,
                                                     stub_irq_handler,
                                                     nullptr);
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_port, err);
  TEST_END("gpio attach_irq bad pin");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpio_detach_irq_happy(void)
{
  TEST_BEGIN("gpio detach_irq happy");
  reset_irq_state();

  const ra8_port_pin_t     pin = (ra8_port_pin_t)k_ra8_gpio_test_pin_alt;
  const ra8_gpio_irq_cfg_t cfg = make_irq_cfg();
  TEST_ASSERT_EQ(
    k_ra8_ok,

    ra8_gpio_attach_irq(pin, (uint8_t)k_ra8_gpio_test_irq_num, &cfg, stub_irq_handler, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpio_detach_irq(pin, (uint8_t)k_ra8_gpio_test_irq_num));
  TEST_ASSERT(!ra8_pin_validator_is_claimed(pin));

  uint8_t irqcr = k_gpio_irqcr_ff;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_read_irqcr((uint8_t)k_ra8_gpio_test_irq_num, &irqcr));
  TEST_ASSERT_EQ(0, irqcr);
  TEST_END("gpio detach_irq happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpio_detach_irq_bad_num(void)
{
  TEST_BEGIN("gpio detach_irq bad irq num");
  reset_irq_state();

  const ra8_err_t err = ra8_gpio_detach_irq((ra8_port_pin_t)k_ra8_gpio_test_pin_alt,
                                            (uint8_t)k_ra8_gpio_test_irq_bad_num);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, err);
  TEST_END("gpio detach_irq bad irq num");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpio_detach_irq_not_attached(void)
{
  TEST_BEGIN("gpio detach_irq not attached");
  reset_irq_state();

  const ra8_err_t err =
    ra8_gpio_detach_irq((ra8_port_pin_t)k_ra8_gpio_test_pin_alt, (uint8_t)k_ra8_gpio_test_irq_num);
  TEST_ASSERT_EQ(k_ra8_err_not_found, err);
  TEST_END("gpio detach_irq not attached");
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
  test_output_init_happy_low,
  test_output_init_happy_high,
  test_output_init_invalid_port,
  test_output_init_invalid_pin,
  test_output_init_conflict,
  test_input_init_no_pull,
  test_input_init_pull_up,
  test_input_init_invalid_port,
  test_input_init_invalid_pin,
  test_write_high_sets_posr,
  test_write_low_sets_porr,
  test_write_invalid_port,
  test_write_invalid_pin,
  test_toggle_when_low,
  test_toggle_when_high,
  test_toggle_invalid_port,
  test_toggle_invalid_pin,
  test_read_high_and_low,
  test_read_null_out,
  test_read_invalid_port,
  test_read_invalid_pin,
  test_release_pin,
  test_route_peripheral_happy,
  test_route_peripheral_null_owner,
  test_route_peripheral_invalid_port,
  test_route_peripheral_invalid_pin,
  test_route_peripheral_conflict,
  test_vtable_output_init_and_write,
  test_vtable_read_and_toggle,
  test_gpio_attach_irq_happy,
  test_gpio_attach_irq_null_cfg,
  test_gpio_attach_irq_null_handler,
  test_gpio_attach_irq_bad_num,
  test_gpio_attach_irq_bad_pin,
  test_gpio_detach_irq_happy,
  test_gpio_detach_irq_bad_num,
  test_gpio_detach_irq_not_attached,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_gpio.c\n");
  return 0;
}
