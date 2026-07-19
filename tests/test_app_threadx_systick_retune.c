/**
 * @file test_app_threadx_systick_retune.c
 * @brief Unit tests for the ThreadX SysTick retune (issue #287)
 *
 * @details
 * The production app at
 * examples/ek_ra8d2/hw_validated/hil/threadx_systick_retune/main.c raises
 * CPUCLK0 with ra8_cgc_init() and then calls ra8_threadx_systick_retune()
 * from tx_application_define so the ThreadX kernel tick is derived from the
 * *live* core clock rather than the compile-time assumption baked into
 * tx_initialize_low_level.S. ThreadX is not in the host build; this test
 * exercises the two functions that back that behaviour:
 *
 *   - ra8_threadx_systick_reload_for() -- the pure reload arithmetic, with
 *     full range/validity coverage incl. the MC/DC vectors for its compound
 *     zero-input guard.
 *   - ra8_threadx_systick_retune() -- the end-to-end path over the live CGC
 *     published-clock table (SYST_RVR writes compile out under
 *     RA8_SIMULATOR_MODE, so only the clock-query + arithmetic run here).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "ra8_system_regs.h"
#include "ra8_threadx.h"
#include "unity_minimal.h"

/**
 * @enum threadx_systick_retune_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_sys_oscsf_all_ready =
    0xFFU, /**< Every oscillator-stabilisation flag set, so clock bring-up sees all sources ready. */
} threadx_systick_retune_uint8_const_t;

/** @brief Per-test constants (no magic numbers). */
typedef enum : uint32_t {
  k_test_cpuclk_1ghz     = 1000000000UL, /**< Post-CGC CPUCLK0 target.         */
  k_test_reload_1ghz     = 999999UL,     /**< 1 GHz / 1 kHz - 1.               */
  k_test_cpuclk_moco     = 8000000UL,    /**< Boot-default MOCO ~8 MHz.        */
  k_test_reload_moco     = 7999UL,       /**< 8 MHz / 1 kHz - 1.               */
  k_test_tick_1khz       = 1000UL,       /**< ThreadX 1 kHz kernel tick.       */
  k_test_reload_max      = 0x00FFFFFFUL, /**< 24-bit SYST_RVR ceiling.         */
  k_test_cpuclk_fits_max = 16777216UL,   /**< reload == max at tick_hz = 1.    */
  k_test_cpuclk_over_max = 16777218UL,   /**< reload == max + 1 (rejected).    */
  k_test_tick_one_hz     = 1UL,          /**< Degenerate 1 Hz tick for range.  */
  k_test_cpuclk_below    = 500UL,        /**< Slower than one 1 kHz tick.      */
  k_test_sentinel_reload = 0xDEADBEEFUL, /**< Poison to prove no write on err. */
} test_retune_const_t;

/**
 * @brief Re-seed the sim world so ra8_cgc_init() spin loops settle.
 */
static void reset_world(void)
{
  ra8_sim_mmap_reset();
  /* Pre-seed OSCSF stabilisation bits so ra8_cgc_init() spin loops
   * complete on the first iteration in RA8_SIMULATOR_MODE. */
  *ra8_sys_oscsf() = (uint8_t)k_sys_oscsf_all_ready;
}

/**
 * @brief Reject a NULL out_reload pointer.
 *
 * @par MC/DC:
 * (no compound decision in the code path this case touches -- the
 * RA8_CHECK_NULL_PTR guard is a single `ptr == nullptr` condition)
 */
static void test_reload_for_rejects_null(void)
{
  TEST_BEGIN("reload_for rejects NULL out_reload");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_threadx_systick_reload_for(k_test_cpuclk_1ghz, k_test_tick_1khz, nullptr));
  TEST_END("reload_for rejects NULL out_reload");
}

/**
 * @brief Compute the 1 GHz / 1 kHz reload (the nominal RA8 case).
 *
 * @par MC/DC:
 * (no compound decision on the happy path -- the zero-input guard, the
 * ticks==0 guard, and the range guard are each single-condition and all
 * evaluate false here)
 */
static void test_reload_for_computes_1ghz(void)
{
  TEST_BEGIN("reload_for computes 1 GHz reload");
  uint32_t reload = (uint32_t)k_test_sentinel_reload;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_threadx_systick_reload_for(k_test_cpuclk_1ghz, k_test_tick_1khz, &reload));
  TEST_ASSERT_EQ(k_test_reload_1ghz, reload);
  TEST_END("reload_for computes 1 GHz reload");
}

/**
 * @brief Compute the MOCO reload -- shows the value is clock-driven, not fixed.
 *
 * @par MC/DC:
 * (no compound decision on this happy path -- all three single-condition
 * guards evaluate false for an 8 MHz clock)
 */
static void test_reload_for_computes_moco(void)
{
  TEST_BEGIN("reload_for computes MOCO reload");
  uint32_t reload = (uint32_t)k_test_sentinel_reload;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_threadx_systick_reload_for(k_test_cpuclk_moco, k_test_tick_1khz, &reload));
  TEST_ASSERT_EQ(k_test_reload_moco, reload);
  TEST_END("reload_for computes MOCO reload");
}

/**
 * @brief MC/DC for the compound zero-input guard.
 *
 * @details
 * Decision under test: `if ((cpuclk_hz == 0U) || (tick_hz == 0U))` (2
 * conditions) in
 * port/threadx/src/cortex_m85/tx_systick_retune.c@ra8_threadx_systick_reload_for.
 *
 * @par MC/DC:
 * Decision: `if ((cpuclk_hz == 0U) || (tick_hz == 0U))` (2 conditions) in
 * port/threadx/src/cortex_m85/tx_systick_retune.c@ra8_threadx_systick_reload_for.
 * - V1: cpuclk_hz=1e9, tick_hz=1000 -> false||false -> false (ok path).
 * - V2: cpuclk_hz=0,   tick_hz=1000 -> true         -> true  (varies cpuclk_hz).
 * - V3: cpuclk_hz=1e9, tick_hz=0    -> false||true  -> true  (varies tick_hz).
 * V1 vs V2 prove cpuclk_hz independently affects the outcome; V1 vs V3
 * prove the same for tick_hz. N+1 = 3 vectors for N=2: minimal MC/DC.
 * On both true vectors the out param is left unmodified (poison survives).
 */
static void test_mcdc_reload_for_zero_inputs(void)
{
  TEST_BEGIN("reload_for MC/DC: zero-input guard");
  uint32_t reload = (uint32_t)k_test_sentinel_reload;

  /* V1: both conditions false -> success, out written. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_threadx_systick_reload_for(k_test_cpuclk_1ghz, k_test_tick_1khz, &reload));
  TEST_ASSERT_EQ(k_test_reload_1ghz, reload);

  /* V2: first condition true (cpuclk_hz == 0) -> invalid_arg, out untouched. */
  reload = (uint32_t)k_test_sentinel_reload;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_threadx_systick_reload_for(0U, k_test_tick_1khz, &reload));
  TEST_ASSERT_EQ(k_test_sentinel_reload, reload);

  /* V3: second condition true (tick_hz == 0) -> invalid_arg, out untouched. */
  reload = (uint32_t)k_test_sentinel_reload;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_threadx_systick_reload_for(k_test_cpuclk_1ghz, 0U, &reload));
  TEST_ASSERT_EQ(k_test_sentinel_reload, reload);

  TEST_END("reload_for MC/DC: zero-input guard");
}

/**
 * @brief Reject a clock slower than one tick period (ticks == 0).
 *
 * @par MC/DC:
 * (single-condition guard `if (ticks == 0U)` -- 500 Hz / 1 kHz = 0 ticks
 * exercises the true branch; the false branch is covered by the happy-path
 * cases above)
 */
static void test_reload_for_rejects_clock_below_tick(void)
{
  TEST_BEGIN("reload_for rejects clock below one tick");
  uint32_t reload = (uint32_t)k_test_sentinel_reload;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_threadx_systick_reload_for(k_test_cpuclk_below, k_test_tick_1khz, &reload));
  /* Out must be untouched on error. */
  TEST_ASSERT_EQ(k_test_sentinel_reload, reload);
  TEST_END("reload_for rejects clock below one tick");
}

/**
 * @brief Reject a reload that would overflow the 24-bit SYST_RVR field.
 *
 * @details
 * At tick_hz = 1, a 16,777,218 Hz clock yields reload = 16,777,217 > the
 * 24-bit ceiling (16,777,215), which must be rejected rather than
 * truncated. The exact-fit boundary (reload == ceiling) must still pass.
 *
 * @par MC/DC:
 * (single-condition guard `if (reload > k_ra8_systick_reload_max)` -- the
 * over-max clock takes the true branch and the exact-fit clock the false
 * branch)
 */
static void test_reload_for_rejects_overflow(void)
{
  TEST_BEGIN("reload_for rejects 24-bit overflow");
  uint32_t reload = (uint32_t)k_test_sentinel_reload;

  /* Exact fit: reload == ceiling -> accepted. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_threadx_systick_reload_for(k_test_cpuclk_fits_max, k_test_tick_one_hz, &reload));
  TEST_ASSERT_EQ(k_test_reload_max, reload);

  /* One past the ceiling -> rejected, out untouched. */
  reload = (uint32_t)k_test_sentinel_reload;
  TEST_ASSERT_EQ(
    k_ra8_err_out_of_range,
    ra8_threadx_systick_reload_for(k_test_cpuclk_over_max, k_test_tick_one_hz, &reload));
  TEST_ASSERT_EQ(k_test_sentinel_reload, reload);

  TEST_END("reload_for rejects 24-bit overflow");
}

/**
 * @brief Retune succeeds and matches the live CPUCLK0 after ra8_cgc_init().
 *
 * @details
 * ra8_cgc_init() publishes CPUCLK0 = 1 GHz into the CGC clock table; the
 * retune must therefore succeed and the reload the app would read from
 * SYST_RVR (independently recomputed here via the pure function) is the
 * 1 GHz value. This mirrors the app's tx_application_define self-check.
 *
 * @par MC/DC:
 * (no compound decision under test -- ra8_threadx_systick_retune has only
 * the two single-condition RA8_RETURN_ON_ERROR guards, both false here on a
 * valid 1 GHz table)
 */
static void test_retune_matches_live_clock(void)
{
  TEST_BEGIN("retune matches live CPUCLK0");
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());

  /* The retune reads the live table + programs SYST_RVR (write compiles
   * out on host); it must succeed for a valid clock. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_threadx_systick_retune());

  /* Confirm the live clock the retune saw is CPUCLK0 = 1 GHz and that the
   * derived reload is the expected 1 GHz value. */
  uint32_t cpuclk_hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk_hz));
  TEST_ASSERT_EQ(k_test_cpuclk_1ghz, cpuclk_hz);

  uint32_t reload = (uint32_t)k_test_sentinel_reload;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_threadx_systick_reload_for(cpuclk_hz, k_test_tick_1khz, &reload));
  TEST_ASSERT_EQ(k_test_reload_1ghz, reload);
  TEST_END("retune matches live CPUCLK0");
}

/**
 * @brief Program entry point -- runs every test in sequence.
 *
 * @return 0 if all tests pass; any failure calls exit(1) first.
 *
 * @pre The host build defines RA8_SIMULATOR_MODE (SCS writes compile out).
 * @pre None beyond the above.
 * @post Every registered test has run.
 * @post Process exit status reflects pass/fail.
 */
int32_t main(void)
{
  test_reload_for_rejects_null();
  test_reload_for_computes_1ghz();
  test_reload_for_computes_moco();
  test_mcdc_reload_for_zero_inputs();
  test_reload_for_rejects_clock_below_tick();
  test_reload_for_rejects_overflow();
  test_retune_matches_live_clock();
  return 0;
}
