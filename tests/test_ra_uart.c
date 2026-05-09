/**
 * @file test_ra_uart.c
 * @brief Unit tests for uart.c (SCI_B-based polling UART)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_sci_regs.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "ra_uart.h"
#include "unity_minimal.h"

/**
 * @enum ra_uart_test_ch_t
 * @brief Channel numbers used by UART tests.
 */
typedef enum : uint8_t {
  k_ra_uart_test_ch_first  = 0U,
  k_ra_uart_test_ch_middle = 5U,
  k_ra_uart_test_ch_last   = 9U,
  k_ra_uart_test_ch_oor    = 10U, /**< Out of range (SCI only goes 0..9). */
  k_ra_uart_test_ch_huge   = 250U,
} ra_uart_test_ch_t;

/**
 * @enum ra_uart_test_val_t
 * @brief Sample BRR / data values used in the tests.
 */
typedef enum : uint8_t {
  k_ra_uart_test_brr_val = 0x2AU,
  k_ra_uart_test_byte    = 0x55U,
} ra_uart_test_val_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy_first_channel(void)
{
  TEST_BEGIN("uart init first channel");
  ra_sim_mmap_reset();

  const ra_err_t err =
    ra_uart_init((uint8_t)k_ra_uart_test_ch_first, (uint8_t)k_ra_uart_test_brr_val);
  TEST_ASSERT_EQ(k_ra_ok, err);

  volatile r_sci_regs_t* reg = ra_sci((uint8_t)k_ra_uart_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* CCR0 = TE | RE bits, all other bits zero. */
  const uint32_t expected_ccr0 =
    (1U << (uint8_t)k_ra_sci_ccr0_bit_te) | (1U << (uint8_t)k_ra_sci_ccr0_bit_re);
  TEST_ASSERT_EQ(expected_ccr0, reg->CCR0);
  /* CCR1 = 0 means parity off, no inversion -- 8-N-1 path. */
  TEST_ASSERT_EQ(0, reg->CCR1);
  /* CCR3 carries the 8-bit CHR encoding shifted into the field. */
  const uint32_t expected_ccr3 =
    ((uint32_t)k_ra_sci_ccr3_chr_8bit << (uint8_t)k_ra_sci_ccr3_shift_chr);
  TEST_ASSERT_EQ(expected_ccr3, reg->CCR3);
  /* CCR2 carries BRR<<8 | MDDR<<24. */
  const uint32_t expected_ccr2 =
    ((uint32_t)k_ra_uart_test_brr_val << (uint8_t)k_ra_sci_ccr2_shift_brr) |
    ((uint32_t)0xFFU << (uint8_t)k_ra_sci_ccr2_shift_mddr);
  TEST_ASSERT_EQ(expected_ccr2, reg->CCR2);
  TEST_END("uart init first channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy_middle_channel(void)
{
  TEST_BEGIN("uart init middle channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_uart_init((uint8_t)k_ra_uart_test_ch_middle, (uint8_t)k_ra_uart_test_brr_val));
  TEST_END("uart init middle channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy_last_channel(void)
{
  TEST_BEGIN("uart init last channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_uart_init((uint8_t)k_ra_uart_test_ch_last, (uint8_t)k_ra_uart_test_brr_val));
  volatile r_sci_regs_t* reg = ra_sci((uint8_t)k_ra_uart_test_ch_last);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_END("uart init last channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_channel(void)
{
  TEST_BEGIN("uart init bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_uart_init((uint8_t)k_ra_uart_test_ch_oor, 0U));
  TEST_END("uart init bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_huge_channel(void)
{
  TEST_BEGIN("uart init huge channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_uart_init((uint8_t)k_ra_uart_test_ch_huge, 0U));
  TEST_END("uart init huge channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_putc_happy(void)
{
  TEST_BEGIN("uart putc happy");
  ra_sim_mmap_reset();

  /* Pre-arm CSR.TDRE so the first poll succeeds immediately. */
  volatile r_sci_regs_t* reg = ra_sci((uint8_t)k_ra_uart_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CSR = (1U << (uint8_t)k_ra_sci_csr_bit_tdre);

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_uart_putc((uint8_t)k_ra_uart_test_ch_first, (uint8_t)k_ra_uart_test_byte));
  TEST_ASSERT_EQ(k_ra_uart_test_byte, (reg->TDR & 0xFFU));
  TEST_END("uart putc happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_putc_timeout(void)
{
  TEST_BEGIN("uart putc timeout");
  ra_sim_mmap_reset();

  /* CSR is zero -> TDRE never sets -> expect hw_timeout. */
  TEST_ASSERT_EQ(k_ra_err_hw_timeout,
                 ra_uart_putc((uint8_t)k_ra_uart_test_ch_first, (uint8_t)k_ra_uart_test_byte));
  TEST_END("uart putc timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_putc_bad_channel(void)
{
  TEST_BEGIN("uart putc bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_uart_putc((uint8_t)k_ra_uart_test_ch_oor, (uint8_t)k_ra_uart_test_byte));
  TEST_END("uart putc bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_putc_last_channel(void)
{
  TEST_BEGIN("uart putc last channel");
  ra_sim_mmap_reset();

  volatile r_sci_regs_t* reg = ra_sci((uint8_t)k_ra_uart_test_ch_last);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CSR = (1U << (uint8_t)k_ra_sci_csr_bit_tdre);

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_uart_putc((uint8_t)k_ra_uart_test_ch_last, (uint8_t)k_ra_uart_test_byte));
  TEST_END("uart putc last channel");
}

int32_t main(void)
{
  test_init_happy_first_channel();
  test_init_happy_middle_channel();
  test_init_happy_last_channel();
  test_init_bad_channel();
  test_init_huge_channel();
  test_putc_happy();
  test_putc_timeout();
  test_putc_bad_channel();
  test_putc_last_channel();
  (void)fprintf(stderr, "[OK  ] test_ra_uart.c\n");
  return 0;
}
