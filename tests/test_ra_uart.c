/**
 * @file test_ra_uart.c
 * @brief Unit tests for uart.c (SCI-based polling UART)
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
  k_ra_uart_test_ch_oor    = 10U, /**< Out of range (SCI only goes 0..9).  */
  k_ra_uart_test_ch_huge   = 250U,
} ra_uart_test_ch_t;

/**
 * @enum ra_uart_test_val_t
 * @brief Sample BRR / data values used in the tests.
 */
typedef enum : uint8_t {
  k_ra_uart_test_brr_val     = 0x2AU,
  k_ra_uart_test_byte        = 0x55U,
  k_ra_uart_test_tdre_mask   = 0x80U, /**< SSR.TDRE = bit 7.                 */
  k_ra_uart_test_scr_enabled = (uint8_t)((1U << 4) | (1U << 5)), /**< TE|RE. */
  k_ra_uart_test_scmr_expect = 0xF2U,
} ra_uart_test_val_t;

static void test_init_happy_first_channel(void)
{
  TEST_BEGIN("uart init first channel");
  ra_sim_mmap_reset();

  const ra_err_t err =
    ra_uart_init((uint8_t)k_ra_uart_test_ch_first, (uint8_t)k_ra_uart_test_brr_val);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);

  volatile r_sci_regs_t* reg = ra_sci((uint8_t)k_ra_uart_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ((int)k_ra_uart_test_scr_enabled, (int)reg->SCR);
  TEST_ASSERT_EQ(0, (int)reg->SMR);
  TEST_ASSERT_EQ((int)k_ra_uart_test_scmr_expect, (int)reg->SCMR);
  TEST_ASSERT_EQ((int)k_ra_uart_test_brr_val, (int)reg->BRR);
  TEST_ASSERT_EQ(0, (int)reg->SEMR);
  TEST_END("uart init first channel");
}

static void test_init_happy_middle_channel(void)
{
  TEST_BEGIN("uart init middle channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_uart_init((uint8_t)k_ra_uart_test_ch_middle, (uint8_t)k_ra_uart_test_brr_val));
  TEST_END("uart init middle channel");
}

static void test_init_happy_last_channel(void)
{
  TEST_BEGIN("uart init last channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_uart_init((uint8_t)k_ra_uart_test_ch_last, (uint8_t)k_ra_uart_test_brr_val));
  volatile r_sci_regs_t* reg = ra_sci((uint8_t)k_ra_uart_test_ch_last);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_END("uart init last channel");
}

static void test_init_bad_channel(void)
{
  TEST_BEGIN("uart init bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_uart_init((uint8_t)k_ra_uart_test_ch_oor, 0U));
  TEST_END("uart init bad channel");
}

static void test_init_huge_channel(void)
{
  TEST_BEGIN("uart init huge channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_uart_init((uint8_t)k_ra_uart_test_ch_huge, 0U));
  TEST_END("uart init huge channel");
}

static void test_putc_happy(void)
{
  TEST_BEGIN("uart putc happy");
  ra_sim_mmap_reset();

  /* Pre-arm TDRE so the first poll succeeds immediately. */
  volatile r_sci_regs_t* reg = ra_sci((uint8_t)k_ra_uart_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->SSR = (uint8_t)k_ra_uart_test_tdre_mask;

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_uart_putc((uint8_t)k_ra_uart_test_ch_first, (uint8_t)k_ra_uart_test_byte));
  TEST_ASSERT_EQ((int)k_ra_uart_test_byte, (int)reg->TDR);
  TEST_END("uart putc happy");
}

static void test_putc_timeout(void)
{
  TEST_BEGIN("uart putc timeout");
  ra_sim_mmap_reset();

  /* SSR is zero -> TDRE never sets -> expect hw_timeout. */
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout,
                 (int)ra_uart_putc((uint8_t)k_ra_uart_test_ch_first, (uint8_t)k_ra_uart_test_byte));
  TEST_END("uart putc timeout");
}

static void test_putc_bad_channel(void)
{
  TEST_BEGIN("uart putc bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_uart_putc((uint8_t)k_ra_uart_test_ch_oor, (uint8_t)k_ra_uart_test_byte));
  TEST_END("uart putc bad channel");
}

static void test_putc_last_channel(void)
{
  TEST_BEGIN("uart putc last channel");
  ra_sim_mmap_reset();

  volatile r_sci_regs_t* reg = ra_sci((uint8_t)k_ra_uart_test_ch_last);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->SSR = (uint8_t)k_ra_uart_test_tdre_mask;

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_uart_putc((uint8_t)k_ra_uart_test_ch_last, (uint8_t)k_ra_uart_test_byte));
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
