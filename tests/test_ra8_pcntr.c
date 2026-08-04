/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file test_ra8_pcntr.c
 * @brief Unit tests for the CPU1-safe PCNTR PORT primitive (ra8_pcntr.h)
 *
 * @details
 * Exercises the header-only `ra8_pcntr_set_output()` / `ra8_pcntr_read()`
 * primitives on the `ra8_fake_mmap` peripheral-RAM backing, the same way
 * `test_ra8_gpio.c` covers the high-level PORT + PFS driver. The primitive is
 * the one the freestanding Cortex-M33 (CPU1) images use, so these host tests
 * are the off-target proof of its register behaviour (issue #580).
 *
 * The primitive contains no compound boolean decisions (every guard is a single
 * condition; the `if (level == high)` and the `? :` in the reader are single
 * conditions too), so there are no MC/DC vectors to enumerate; each case notes
 * this in its `@par MC/DC:` block, and the low/high pairs still drive both
 * branches of the level select for full branch coverage.
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_pcntr.h"
#include "ra8_port_constants.h"
#include "ra8_port_regs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_pcntr_test_ids_t
 * @brief Ports / pins and poison values used by the PCNTR test cases.
 */
typedef enum : uint8_t {
  k_pcntr_test_port     = 1U,    /**< PORT1: the port under test.                  */
  k_pcntr_test_pin      = 2U,    /**< Pin 2: the pin under test.                   */
  k_pcntr_test_sibling  = 5U,    /**< Pin 5: a sibling pin the RMW must preserve.  */
  k_pcntr_test_bad_port = 15U,   /**< Port 15: one past k_ra8_port_max.            */
  k_pcntr_test_bad_pin  = 16U,   /**< Pin 16: one past k_ra8_pin_max.              */
  k_pcntr_poison_level  = 0x5AU, /**< Poison level: proves an out-param untouched. */
} ra8_pcntr_test_ids_t;

/**
 * @brief Reset the mock MMIO backing before each case.
 */
static void reset_state(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @brief Build the low-half (PDR) mask for a pin.
 * @param[in] pin Pin index 0..15.
 * @return `1U << pin`.
 */
static uint32_t pdr_mask(uint32_t pin)
{
  return (uint32_t)(1UL << pin);
}

/**
 * @brief Build the high-half (PODR) mask for a pin.
 * @param[in] pin Pin index 0..15.
 * @return `1U << (16 + pin)`.
 */
static uint32_t podr_mask(uint32_t pin)
{
  return (uint32_t)(1UL << pin) << (uint32_t)k_ra8_pcntr_high_half_shift;
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- this case drives the
 * `level == high` branch of the single level-select condition)
 */
static void test_set_output_high(void)
{
  TEST_BEGIN("pcntr set_output high");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pcntr_set_output((ra8_port_t)k_pcntr_test_port,
                                      (ra8_pin_t)k_pcntr_test_pin,
                                      k_ra8_level_high));

  volatile r_port_regs_t* reg = ra8_port((ra8_port_t)k_pcntr_test_port);
  TEST_ASSERT_NOT_NULL((void*)reg);
  const uint32_t expected =
    pdr_mask((uint32_t)k_pcntr_test_pin) | podr_mask((uint32_t)k_pcntr_test_pin);
  /* HUM Ch 20.2.1 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 */
  TEST_ASSERT_EQ(expected, reg->PCNTR1);
  TEST_END("pcntr set_output high");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- this case drives the
 * `level != high` branch of the single level-select condition)
 */
static void test_set_output_low(void)
{
  TEST_BEGIN("pcntr set_output low");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pcntr_set_output((ra8_port_t)k_pcntr_test_port,
                                      (ra8_pin_t)k_pcntr_test_pin,
                                      k_ra8_level_low));

  volatile r_port_regs_t* reg = ra8_port((ra8_port_t)k_pcntr_test_port);
  /* Output direction set (PDR bit), output latch cleared (no PODR bit). */
  /* HUM Ch 20.2.1 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 */
  TEST_ASSERT_EQ(pdr_mask((uint32_t)k_pcntr_test_pin), reg->PCNTR1);
  TEST_END("pcntr set_output low");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- proves the read-modify-write
 * preserves an unrelated sibling pin; drives the `level == high` branch)
 */
static void test_set_output_preserves_sibling_high(void)
{
  TEST_BEGIN("pcntr set_output preserves sibling (high)");
  reset_state();

  volatile r_port_regs_t* reg = ra8_port((ra8_port_t)k_pcntr_test_port);
  /* Sibling pin pre-configured output + high. */
  /* HUM Ch 20.2.1 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 */
  reg->PCNTR1 =
    pdr_mask((uint32_t)k_pcntr_test_sibling) | podr_mask((uint32_t)k_pcntr_test_sibling);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pcntr_set_output((ra8_port_t)k_pcntr_test_port,
                                      (ra8_pin_t)k_pcntr_test_pin,
                                      k_ra8_level_high));

  const uint32_t expected =
    pdr_mask((uint32_t)k_pcntr_test_sibling) | podr_mask((uint32_t)k_pcntr_test_sibling) |
    pdr_mask((uint32_t)k_pcntr_test_pin) | podr_mask((uint32_t)k_pcntr_test_pin);
  /* HUM Ch 20.2.1 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 */
  TEST_ASSERT_EQ(expected, reg->PCNTR1);
  TEST_END("pcntr set_output preserves sibling (high)");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- proves driving a pin LOW
 * clears only that pin's PODR bit and preserves the sibling; drives the
 * `level != high` branch)
 */
static void test_set_output_low_clears_only_target(void)
{
  TEST_BEGIN("pcntr set_output low clears only target");
  reset_state();

  volatile r_port_regs_t* reg = ra8_port((ra8_port_t)k_pcntr_test_port);
  /* Sibling output+high, and our pin's PODR already high before we drive it low. */
  /* HUM Ch 20.2.1 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 */
  reg->PCNTR1 = pdr_mask((uint32_t)k_pcntr_test_sibling) |
                podr_mask((uint32_t)k_pcntr_test_sibling) | podr_mask((uint32_t)k_pcntr_test_pin);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pcntr_set_output((ra8_port_t)k_pcntr_test_port,
                                      (ra8_pin_t)k_pcntr_test_pin,
                                      k_ra8_level_low));

  /* Our pin: PDR set, PODR cleared. Sibling: untouched. */
  const uint32_t expected = pdr_mask((uint32_t)k_pcntr_test_sibling) |
                            podr_mask((uint32_t)k_pcntr_test_sibling) |
                            pdr_mask((uint32_t)k_pcntr_test_pin);
  /* HUM Ch 20.2.1 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 */
  TEST_ASSERT_EQ(expected, reg->PCNTR1);
  TEST_END("pcntr set_output low clears only target");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- single-condition port guard)
 */
static void test_set_output_invalid_port(void)
{
  TEST_BEGIN("pcntr set_output invalid port");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_port,
                 ra8_pcntr_set_output((ra8_port_t)k_pcntr_test_bad_port,
                                      (ra8_pin_t)k_pcntr_test_pin,
                                      k_ra8_level_high));
  TEST_END("pcntr set_output invalid port");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- single-condition pin guard)
 */
static void test_set_output_invalid_pin(void)
{
  TEST_BEGIN("pcntr set_output invalid pin");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin,
                 ra8_pcntr_set_output((ra8_port_t)k_pcntr_test_port,
                                      (ra8_pin_t)k_pcntr_test_bad_pin,
                                      k_ra8_level_high));
  TEST_END("pcntr set_output invalid pin");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- drives the `PIDR bit set`
 * branch of the single level-select ternary)
 */
static void test_read_high(void)
{
  TEST_BEGIN("pcntr read high");
  reset_state();

  volatile r_port_regs_t* reg = ra8_port((ra8_port_t)k_pcntr_test_port);
  /* PIDR (low half of PCNTR2) bit set. */
  /* HUM Ch 20.2.2 "PCNTR2/EIDR/PIDR : Port Control Register 2" p 841 */
  reg->PCNTR2 = pdr_mask((uint32_t)k_pcntr_test_pin);

  ra8_level_t got = k_ra8_level_low;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pcntr_read((ra8_port_t)k_pcntr_test_port, (ra8_pin_t)k_pcntr_test_pin, &got));
  TEST_ASSERT_EQ(k_ra8_level_high, got);
  TEST_END("pcntr read high");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- drives the `PIDR bit clear`
 * branch of the single level-select ternary)
 */
static void test_read_low(void)
{
  TEST_BEGIN("pcntr read low");
  reset_state();

  volatile r_port_regs_t* reg = ra8_port((ra8_port_t)k_pcntr_test_port);
  /* HUM Ch 20.2.2 "PCNTR2/EIDR/PIDR : Port Control Register 2" p 841 */
  reg->PCNTR2 = 0U;

  ra8_level_t got = k_ra8_level_high;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pcntr_read((ra8_port_t)k_pcntr_test_port, (ra8_pin_t)k_pcntr_test_pin, &got));
  TEST_ASSERT_EQ(k_ra8_level_low, got);
  TEST_END("pcntr read low");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- single-condition null guard)
 */
static void test_read_null_out(void)
{
  TEST_BEGIN("pcntr read null out");
  reset_state();

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_pcntr_read((ra8_port_t)k_pcntr_test_port, (ra8_pin_t)k_pcntr_test_pin, nullptr));
  TEST_END("pcntr read null out");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- single-condition port guard;
 * also asserts the out-param is left untouched on the failure path)
 */
static void test_read_invalid_port(void)
{
  TEST_BEGIN("pcntr read invalid port");
  reset_state();

  ra8_level_t got = (ra8_level_t)k_pcntr_poison_level;
  TEST_ASSERT_EQ(
    k_ra8_err_gpio_invalid_port,
    ra8_pcntr_read((ra8_port_t)k_pcntr_test_bad_port, (ra8_pin_t)k_pcntr_test_pin, &got));
  /* Failure path must not write the out-param. */
  TEST_ASSERT_EQ((ra8_level_t)k_pcntr_poison_level, got);
  TEST_END("pcntr read invalid port");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- single-condition pin guard)
 */
static void test_read_invalid_pin(void)
{
  TEST_BEGIN("pcntr read invalid pin");
  reset_state();

  ra8_level_t got = (ra8_level_t)k_pcntr_poison_level;
  TEST_ASSERT_EQ(
    k_ra8_err_gpio_invalid_pin,
    ra8_pcntr_read((ra8_port_t)k_pcntr_test_port, (ra8_pin_t)k_pcntr_test_bad_pin, &got));
  TEST_ASSERT_EQ((ra8_level_t)k_pcntr_poison_level, got);
  TEST_END("pcntr read invalid pin");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 * @details main() walks this table, so adding a case is a one-line edit.
 * @note Order is significant: cases run top to bottom.
 */
static void (*const s_test_roster[])(void) = {
  test_set_output_high,
  test_set_output_low,
  test_set_output_preserves_sibling_high,
  test_set_output_low_clears_only_target,
  test_set_output_invalid_port,
  test_set_output_invalid_pin,
  test_read_high,
  test_read_low,
  test_read_null_out,
  test_read_invalid_port,
  test_read_invalid_pin,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_pcntr.c\n");
  return 0;
}
