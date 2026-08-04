/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_uart_hello.c
 * @brief Integration test: SCI8 init + write + LED heartbeat flow
 *
 * @details
 * Mirrors STAR's test_communication_task.c structure for the UART
 * "hello" demo at examples/ek_ra8d2/uart_hello/main.c. Exercises the full
 * bring-up sequence (PFS routing -> ra8_sci_init -> LED init) and
 * the steady-state loop (write_polling + led_toggle), with mocked
 * MMIO via the existing tests/mocks/ra8_fake_mmap.c.
 *
 * Exercised modules:
 *   - ra8_pfs (peripheral pin routing)
 *   - ra8_sci (SCI_B async UART driver)
 *   - ra8_board_ek_ra8d2 (LED heartbeat)
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpio_constants.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "ra8_sci_regs.h"
#include "unity_minimal.h"

/** @brief Per-test enums. */
typedef enum : uint32_t {
  k_test_uart_baud        = 115200U,    /**< Test UART baud.          */
  k_test_uart_pclk_hz     = 125000000U, /**< Same PCLKA the app sees. */
  k_test_uart_sci_channel = 8U,         /**< Test UART SCI channel.   */
  k_test_uart_loop_iters  = 3U,         /**< Test UART loop iters.    */
  k_test_uart_bad_channel = 99U,        /**< Test UART bad channel.   */
} test_uart_const_t;

/** @brief Greeting buffer same shape as the app. */
static const uint8_t k_test_uart_greeting[] = "hello, ra8d2!\r\n";

/** @brief Pin map mirrors examples/ek_ra8d2/uart_hello/main.c PD_02 / PD_03. */
static const ra8_port_pin_t k_test_uart_pin_txd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << 8) | (uint16_t)k_ra8_pin_2);
static const ra8_port_pin_t k_test_uart_pin_rxd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << 8) | (uint16_t)k_ra8_pin_3);

/**
 * @brief Per-test fixture reset.
 */
static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
  /* Pre-seed CSR.TDRE for the channel under test so putc spins
   * complete on the first iteration in RA8_OFF_TARGET. */
  ra8_sci((uint8_t)k_test_uart_sci_channel)->CSR =
    (uint32_t)(1U << (uint8_t)k_ra8_sci_csr_bit_tdre);
}

/* -------------------------------------------------------------------------
 * Golden path: full bring-up sequence
 * ------------------------------------------------------------------------- */

/**
 * @brief Replays the PFS-routing portion of uart_hello_setup_or_halt.
 *
 * @par MC/DC:
 * Decision under test (in app): ``ra8_pfs_route_peripheral(TXD) != ok``
 * short-circuited with the RXD route. Two atomic conditions x N+1 = 3
 * vectors. This case covers both-ok.
 */
static void test_uart_pfs_routing_ok(void)
{
  reset_world();
  TEST_BEGIN("uart_hello: PFS routes TXD8 + RXD8");
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pfs_route_peripheral(k_test_uart_pin_txd, k_ra8_psel_sci_async, "test.txd8"));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pfs_route_peripheral(k_test_uart_pin_rxd, k_ra8_psel_sci_async, "test.rxd8"));
  TEST_END("uart_hello: PFS routes TXD8 + RXD8");
}

/**
 * @brief Drives ra8_sci_init with the same cfg the app uses.
 *
 * @par MC/DC:
 * Decision under test: ``ra8_sci_init() != k_ra8_ok``. One atomic
 * condition x 2 vectors -- ok (this test) + invalid-channel
 * (test_uart_init_bad_channel below).
 */
static void test_uart_sci_init_115200_8n1(void)
{
  reset_world();
  TEST_BEGIN("uart_hello: ra8_sci_init 115200 8N1");
  const ra8_sci_cfg_t cfg = {
    .baud      = (uint32_t)k_test_uart_baud,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_none,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = (uint32_t)k_test_uart_pclk_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_test_uart_sci_channel, &cfg));
  TEST_END("uart_hello: ra8_sci_init 115200 8N1");
}

/**
 * @brief Replays the steady-state main loop body N times.
 *
 * @details Each iteration: write_polling(greeting) + led_toggle. This
 * is the exact pair of calls the app's `while(1)` issues per period.
 *
 * @par MC/DC:
 * Compound decision under test (app): ``write_polling != ok ||
 * led_toggle != ok``. Two atomic conditions x N+1 = 3 vectors -- both
 * ok (this test) + write fails (covered by bad-channel test) + toggle
 * fails (covered by invalid LED test in test_app_threadx_blink.c).
 */
static void test_uart_steady_state_write_and_toggle(void)
{
  reset_world();
  TEST_BEGIN("uart_hello: write+toggle loop");
  /* Bring everything up first like the app does. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pfs_route_peripheral(k_test_uart_pin_txd, k_ra8_psel_sci_async, "test.txd8"));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pfs_route_peripheral(k_test_uart_pin_rxd, k_ra8_psel_sci_async, "test.rxd8"));
  const ra8_sci_cfg_t cfg = {
    .baud      = (uint32_t)k_test_uart_baud,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_none,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = (uint32_t)k_test_uart_pclk_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_test_uart_sci_channel, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led1));

  /* ra8_sci_init clears CSR via CFCLR; re-seed TDRE so each putc spin
   * completes immediately under RA8_OFF_TARGET. */
  volatile r_sci_regs_t* sci_reg = ra8_sci((uint8_t)k_test_uart_sci_channel);
  for (uint8_t i = 0; i < (uint8_t)k_test_uart_loop_iters; i++) {
    sci_reg->CSR = (uint32_t)(1U << (uint8_t)k_ra8_sci_csr_bit_tdre);
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_sci_write_polling((uint8_t)k_test_uart_sci_channel,
                                         k_test_uart_greeting,
                                         (uint32_t)(sizeof(k_test_uart_greeting) - 1U)));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle(k_ra8_board_led1));
  }
  TEST_END("uart_hello: write+toggle loop");
}

/* -------------------------------------------------------------------------
 * Edge / failure paths
 * ------------------------------------------------------------------------- */

/**
 * @brief Init failure on invalid SCI channel surfaces to caller.
 *
 * @par MC/DC:
 * Decision under test: ``ra8_sci_init() != k_ra8_ok`` -- failure vector.
 * Pairs with test_uart_sci_init_115200_8n1 ok-vector for N+1 coverage.
 */
static void test_uart_init_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("uart_hello: ra8_sci_init rejects channel 99");
  const ra8_sci_cfg_t cfg = {
    .baud      = (uint32_t)k_test_uart_baud,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_none,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = (uint32_t)k_test_uart_pclk_hz,
  };
  TEST_ASSERT(ra8_sci_init((uint8_t)k_test_uart_bad_channel, &cfg) != k_ra8_ok);
  TEST_END("uart_hello: ra8_sci_init rejects channel 99");
}

/**
 * @brief NULL cfg pointer is rejected by ra8_sci_init.
 *
 * @par MC/DC:
 * Decision under test: ``cfg == nullptr`` precondition guard. Two
 * vectors: cfg==NULL (this test) + cfg!=NULL (golden). Pairs with the
 * ok-init test for full coverage of the NULL check.
 */
static void test_uart_init_null_cfg_rejected(void)
{
  reset_world();
  TEST_BEGIN("uart_hello: ra8_sci_init rejects NULL cfg");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_init((uint8_t)k_test_uart_sci_channel, nullptr));
  TEST_END("uart_hello: ra8_sci_init rejects NULL cfg");
}

/**
 * @brief PFS route on an out-of-range pin is rejected.
 *
 * @par MC/DC:
 * Decision under test: ``ra8_pfs_route_peripheral`` failure vector --
 * combined with test_uart_pfs_routing_ok this exercises both branches
 * of the route_peripheral early-return guard.
 */
static void test_uart_pfs_route_invalid_pin_rejected(void)
{
  reset_world();
  TEST_BEGIN("uart_hello: PFS route rejects bad port");
  /* Port 15 is out-of-range on the RA8D2; the BSP must reject it. */
  const ra8_port_pin_t bad_pin = (ra8_port_pin_t)(((uint16_t)0x0FU << 8) | (uint16_t)k_ra8_pin_0);
  TEST_ASSERT(ra8_pfs_route_peripheral(bad_pin, k_ra8_psel_sci_async, "test.bad") != k_ra8_ok);
  TEST_END("uart_hello: PFS route rejects bad port");
}

/* -------------------------------------------------------------------------
 * Test runner
 * ------------------------------------------------------------------------- */

int main(void)
{
  test_uart_pfs_routing_ok();
  test_uart_sci_init_115200_8n1();
  test_uart_steady_state_write_and_toggle();
  test_uart_init_bad_channel();
  test_uart_init_null_cfg_rejected();
  test_uart_pfs_route_invalid_pin_rejected();
  return 0;
}
