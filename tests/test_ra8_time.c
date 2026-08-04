/**
 * @file test_ra8_time.c
 * @brief Unit tests for ra8_time.h SysTick tick counter
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_time.h"
#include "unity_minimal.h"

/**
 * @enum t_time_t
 * @brief Timer source frequency the clock is initialised with.
 */
typedef enum : uint32_t {
  k_t_source_hz = 1000000UL, /**< 1 MHz, so one tick is exactly one microsecond
                                  and the conversions under test are exact.      */
} t_time_t;

/* On the host there is no real SysTick; we use the exposed
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * ra8_time_on_tick() to manually advance the counter. */

static void test_init_rejects_zero_hz(void)
{
  TEST_BEGIN("ra8_time_init rejects 0 Hz");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_time_init(0U));
  TEST_END("ra8_time_init rejects 0 Hz");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_accepts_reasonable_hz(void)
{
  TEST_BEGIN("ra8_time_init accepts sane cpu_hz");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_time_init(1000000UL));
  TEST_END("ra8_time_init accepts sane cpu_hz");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_tick_counter_advances(void)
{
  TEST_BEGIN("tick counter advances on on_tick");
  (void)ra8_time_init(k_t_source_hz);
  const uint32_t start = ra8_time_ms();
  ra8_time_on_tick();
  ra8_time_on_tick();
  ra8_time_on_tick();
  TEST_ASSERT_EQ((start + 3U), ra8_time_ms());
  TEST_END("tick counter advances on on_tick");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_now_and_sleep_aliases(void)
{
  TEST_BEGIN("short aliases forward to systick");
  (void)ra8_time_init(k_t_source_hz);
  const uint32_t via_long  = ra8_time_ms();
  const uint32_t via_short = ra8_now_ms();
  TEST_ASSERT_EQ(via_long, via_short);
  TEST_END("short aliases forward to systick");
}

/**
 * @brief Test that cpu_hz too low for a 1 kHz tick is rejected.
 *
 * @details
 * When cpu_hz is between 1 and 1999 the integer division
 * ``cpu_hz / 1000`` rounds to 1, which gives reload == 0 after
 * subtracting 1. The implementation must detect this and return
 * an error rather than programming a zero reload into SysTick.
 *
 * @pre None.
 * @post ra8_time_init returns k_ra8_err_invalid_arg for hz in [1, 1999].
 *
 * @par MC/DC:
 * Decision: ``if (reload == 0U)`` -- single condition.
 * - V1: cpu_hz=1000000 -> reload=999  (non-zero) -> ok path.
 * - V2: cpu_hz=1001    -> reload=0    (zero)     -> error path.
 * V1 vs V2 independently vary the condition.
 *
 * @since 0.1.0
 */
static void test_init_rejects_cpu_hz_too_low(void)
{
  TEST_BEGIN("ra8_time_init rejects cpu_hz that yields reload=0");
  /* 1001 / 1000 = 1; 1 - 1 = 0 -> k_ra8_err_invalid_arg */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_time_init(1001U));
  /* 1999 / 1000 = 1; same result */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_time_init(1999U));
  TEST_END("ra8_time_init rejects cpu_hz that yields reload=0");
}

/**
 * @brief Test that SysTick_Handler increments the tick counter.
 *
 * @details
 * SysTick_Handler is the weak IRQ vector body; it must increment
 * s_tick_ms exactly once per call. We call it directly (the function
 * is externally visible via the weak attribute).
 *
 * @pre ra8_time_init has been called.
 * @post s_tick_ms is incremented each time SysTick_Handler is called.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the ISR body which
 * is a straight-line increment; no ``&&`` or ``||``)
 *
 * @since 0.1.0
 */
static void test_systick_handler_increments_counter(void)
{
  TEST_BEGIN("SysTick_Handler increments tick counter");
  extern void SysTick_Handler(void);

  (void)ra8_time_init(k_t_source_hz);
  const uint32_t before = ra8_time_ms();
  SysTick_Handler();
  SysTick_Handler();
  TEST_ASSERT_EQ((before + 2U), ra8_time_ms());
  TEST_END("SysTick_Handler increments tick counter");
}

int32_t main(void)
{
  test_init_rejects_zero_hz();
  test_init_accepts_reasonable_hz();
  test_tick_counter_advances();
  test_now_and_sleep_aliases();
  test_init_rejects_cpu_hz_too_low();
  test_systick_handler_increments_counter();
  (void)fprintf(stderr, "[OK  ] test_ra8_time.c\n");
  return 0;
}
