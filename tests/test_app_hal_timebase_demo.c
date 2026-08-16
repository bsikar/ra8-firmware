/**
 * @file test_app_hal_timebase_demo.c
 * @brief Integration test: the hal_timebase_demo example's timebase logic.
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_pending/hal_timebase_demo/main.c bring-up
 * against the fake MMIO map: the demo computes a SysTick reload with
 * ra8_systick_reload_for (the value it prints as `reload=...`), arms SysTick
 * with ra8_systick_configure, enables the DWT cycle counter, and measures an
 * ra8_delay_ms via ra8_dwt_cyccnt_reset / ra8_dwt_cyccnt_read, converting the
 * elapsed cycles to microseconds. On the host build ra8_delay_ms is a no-op and
 * the fake DWT_CYCCNT does not free-run, so this test drives the same primitive
 * calls and register effects the demo depends on and asserts them deterministically:
 * the reload value, the SysTick program, the DWT enable + reset + read path, and
 * the demo's cycles-to-microseconds conversion (its own copy of the helper,
 * exercised across the divisor-floor branch).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_systick.h"
#include "unity_minimal.h"

/**
 * @enum app_hal_timebase_reg_t
 * @brief SCS timebase register addresses the test reads back.
 */
typedef enum : uintptr_t {
  k_t_csr      = 0xE000E010UL, /**< SYST_CSR control and status. */
  k_t_rvr      = 0xE000E014UL, /**< SYST_RVR reload value.       */
  k_t_cvr      = 0xE000E018UL, /**< SYST_CVR current value.      */
  k_t_demcr    = 0xE000EDFCUL, /**< DEMCR (TRCENA at bit 24).    */
  k_t_dwt_ctrl = 0xE0001000UL, /**< DWT_CTRL (CYCCNTENA bit 0).  */
  k_t_cyccnt   = 0xE0001004UL, /**< DWT_CYCCNT free-running.     */
} app_hal_timebase_reg_t;

/**
 * @enum app_hal_timebase_const_t
 * @brief The demo's fixed clock / tick / expected values.
 */
typedef enum : uint32_t {
  k_t_cpu_1ghz       = 1000000000UL, /**< Demo CPUCLK0 after ra8_cgc_init.    */
  k_t_tick_1khz      = 1000UL,       /**< Demo 1 kHz tick (k_demo_tick_hz).   */
  k_t_reload_1ghz    = 999999UL,     /**< The `reload=999999` banner value.   */
  k_t_csr_all        = 0x00000007UL, /**< ENABLE | TICKINT | CLKSOURCE.       */
  k_t_demcr_trcena   = 0x01000000UL, /**< DEMCR.TRCENA (bit 24).              */
  k_t_dwt_cyccntena  = 0x00000001UL, /**< DWT_CTRL.CYCCNTENA (bit 0).         */
  k_t_cyccnt_seed    = 0x3B9AC9FFUL, /**< Seeded elapsed-cycle sample.        */
  k_t_cycles_one_sec = 1000000000UL, /**< ~1 s of cycles at 1 GHz.            */
  k_t_micros_one_sec = 1000000UL,    /**< Expected us for a 1 s delay.        */
  k_t_hz_per_mhz     = 1000000UL,    /**< Cycles-per-us divisor numerator.    */
  k_t_cpu_sub_mhz    = 500UL,        /**< Sub-MHz clock: divisor floors to 1. */
  k_t_cycles_sub_mhz = 1234UL,       /**< Cycles under the floored divisor.   */
  k_t_sentinel       = 0xDEADBEEFUL, /**< Poison to prove no write on error.  */
} app_hal_timebase_const_t;

/** @brief Typed access to a seeded/observed SCS register in the fake map. */
static volatile uint32_t* reg(app_hal_timebase_reg_t addr)
{
  return (volatile uint32_t*)addr;
}

/**
 * @brief The demo's cycles-to-microseconds conversion (copied verbatim).
 *
 * @details A local copy of the arithmetic in
 *          hal_timebase_demo/main.c@demo_emit_measurement, so the test exercises
 *          the exact conversion the demo prints, including the divisor floor
 *          that guards a sub-MHz clock against divide-by-zero.
 *
 * @param[in] cpuclk_hz Live CPUCLK0 in Hz.
 * @param[in] cycles    DWT cycles measured across the delay.
 *
 * @return Elapsed microseconds.
 * @retval 0..UINT32_MAX `cycles / max(1, cpuclk_hz / 1e6)`.
 *
 * @pre `cpuclk_hz` is the clock the delay was calibrated against.
 * @pre None beyond the above.
 * @post No state is modified (pure function).
 * @post The result is floored, never a divide-by-zero.
 *
 * @note Pure; trivially thread-safe.
 * @since 0.1.0
 */
static uint32_t demo_micros(uint32_t cpuclk_hz, uint32_t cycles)
{
  uint32_t cycles_per_us = cpuclk_hz / (uint32_t)k_t_hz_per_mhz;
  if (cycles_per_us == 0U) {
    cycles_per_us = 1U;
  }
  return cycles / cycles_per_us;
}

/**
 * @brief The demo prints the reload ra8_systick_reload_for derives.
 *
 * @par MC/DC:
 * (no compound decision on this path -- reload_for's three guards are each
 * single-condition and all evaluate false for a 1 GHz / 1 kHz input)
 */
static void test_demo_reload_banner(void)
{
  TEST_BEGIN("hal_timebase_demo: reload banner value");
  ra8_fake_mmap_reset();
  uint32_t reload = (uint32_t)k_t_sentinel;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_systick_reload_for(k_t_cpu_1ghz, k_t_tick_1khz, &reload));
  TEST_ASSERT_EQ(k_t_reload_1ghz, reload);
  TEST_END("hal_timebase_demo: reload banner value");
}

/**
 * @brief The demo arms SysTick (CPU clock + tick IRQ) with that reload.
 *
 * @par MC/DC:
 * (no compound decision -- configure's option ternaries are single-condition;
 * this drives the CPU-clock + tick-IRQ arm the demo uses)
 */
static void test_demo_arms_systick(void)
{
  TEST_BEGIN("hal_timebase_demo: configure arms SysTick as the demo does");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_systick_configure(k_t_reload_1ghz, k_ra8_systick_clk_cpu, true));
  TEST_ASSERT_EQ(k_t_csr_all, *reg(k_t_csr));
  TEST_ASSERT_EQ(k_t_reload_1ghz, *reg(k_t_rvr));
  TEST_ASSERT_EQ(0U, *reg(k_t_cvr));
  TEST_END("hal_timebase_demo: configure arms SysTick as the demo does");
}

/**
 * @brief The demo's DWT measure path: enable, reset, read.
 *
 * @details Mirrors demo_setup_or_halt (ra8_dwt_cyccnt_enable) and
 *          demo_emit_measurement (ra8_dwt_cyccnt_reset then ra8_dwt_cyccnt_read).
 *          A seeded DWT_CYCCNT stands in for the cycles a real delay would
 *          accumulate on silicon.
 *
 * @par MC/DC:
 * (no compound decision -- read-modify-write enable + a straight reset/read)
 */
static void test_demo_dwt_measure_path(void)
{
  TEST_BEGIN("hal_timebase_demo: DWT enable/reset/read path");
  ra8_fake_mmap_reset();

  ra8_dwt_cyccnt_enable();
  TEST_ASSERT_EQ(k_t_demcr_trcena, *reg(k_t_demcr));
  TEST_ASSERT_EQ(k_t_dwt_cyccntena, *reg(k_t_dwt_ctrl));

  /* Demo brackets the delay with a reset then a read. */
  *reg(k_t_cyccnt) = (uint32_t)k_t_sentinel;
  ra8_dwt_cyccnt_reset();
  TEST_ASSERT_EQ(0U, ra8_dwt_cyccnt_read());

  /* Stand in for the cycles a 1 s delay accumulates on silicon. */
  *reg(k_t_cyccnt) = (uint32_t)k_t_cyccnt_seed;
  TEST_ASSERT_EQ(k_t_cyccnt_seed, ra8_dwt_cyccnt_read());
  TEST_END("hal_timebase_demo: DWT enable/reset/read path");
}

/**
 * @brief The demo's cycles-to-microseconds conversion, both divisor branches.
 *
 * @par MC/DC:
 * Decision: `if (cycles_per_us == 0U)` (1 condition) in
 * hal_timebase_demo/main.c@demo_emit_measurement (copied into demo_micros).
 * - V1: cpuclk=1e9 -> cycles_per_us=1000 -> false (no floor): 1e9 cyc -> 1e6 us.
 * - V2: cpuclk=500 -> cycles_per_us=0    -> true  (floor to 1): 1234 cyc -> 1234 us.
 * V1 vs V2 vary the single condition across both branches.
 */
static void test_demo_micros_conversion(void)
{
  TEST_BEGIN("hal_timebase_demo: cycles-to-microseconds conversion");
  /* V1: normal 1 GHz clock -> 1 000 000 000 cycles is 1 000 000 us. */
  TEST_ASSERT_EQ(k_t_micros_one_sec, demo_micros(k_t_cpu_1ghz, k_t_cycles_one_sec));
  /* V2: sub-MHz clock floors the divisor to 1 -> cycles pass through as us. */
  TEST_ASSERT_EQ(k_t_cycles_sub_mhz, demo_micros(k_t_cpu_sub_mhz, k_t_cycles_sub_mhz));
  TEST_END("hal_timebase_demo: cycles-to-microseconds conversion");
}

int main(void)
{
  test_demo_reload_banner();
  test_demo_arms_systick();
  test_demo_dwt_measure_path();
  test_demo_micros_conversion();
  return 0;
}
