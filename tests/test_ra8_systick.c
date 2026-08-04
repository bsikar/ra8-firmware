/**
 * @file test_ra8_systick.c
 * @brief Unit tests for ra8_systick.c (Cortex-M85 SysTick + DWT timebase).
 *
 * @details
 * The SCS timebase window (0xE000Exxx) is backed by the fake MMIO map, so the
 * tests drive the primitive and read the SysTick (SYST_CSR / SYST_RVR /
 * SYST_CVR) and DWT (DEMCR / DWT_CTRL / DWT_CYCCNT) registers back to verify
 * the reload arithmetic, the range guards (with the MC/DC vectors for the
 * compound zero-input guard), the configure / set-reload register sequences,
 * and the DWT cycle-counter enable / reset / read.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_systick.h"
#include "unity_minimal.h"

/**
 * @enum systick_test_reg_t
 * @brief SCS timebase register addresses the tests seed and read back.
 */
typedef enum : uintptr_t {
  k_t_csr      = 0xE000E010UL, /**< SYST_CSR control and status. */
  k_t_rvr      = 0xE000E014UL, /**< SYST_RVR reload value.       */
  k_t_cvr      = 0xE000E018UL, /**< SYST_CVR current value.      */
  k_t_demcr    = 0xE000EDFCUL, /**< DEMCR (TRCENA at bit 24).    */
  k_t_dwt_ctrl = 0xE0001000UL, /**< DWT_CTRL (CYCCNTENA bit 0).  */
  k_t_cyccnt   = 0xE0001004UL, /**< DWT_CYCCNT free-running.     */
} systick_test_reg_t;

/**
 * @enum systick_test_const_t
 * @brief Clocks, reloads, register values, and poison used by the tests.
 */
typedef enum : uint32_t {
  k_t_cpu_1ghz      = 1000000000UL, /**< Post-CGC CPUCLK0 target.            */
  k_t_reload_1ghz   = 999999UL,     /**< 1 GHz / 1 kHz - 1.                  */
  k_t_cpu_moco      = 8000000UL,    /**< Boot-default MOCO ~8 MHz.           */
  k_t_reload_moco   = 7999UL,       /**< 8 MHz / 1 kHz - 1.                  */
  k_t_tick_1khz     = 1000UL,       /**< 1 kHz -> 1 ms tick.                 */
  k_t_tick_1hz      = 1UL,          /**< Degenerate 1 Hz tick for range.     */
  k_t_reload_max    = 0x00FFFFFFUL, /**< 24-bit SYST_RVR ceiling.            */
  k_t_cpu_fits_max  = 16777216UL,   /**< reload == max at tick_hz = 1.       */
  k_t_cpu_over_max  = 16777218UL,   /**< reload == max + 1 (rejected).       */
  k_t_cpu_below     = 500UL,        /**< Slower than one 1 kHz tick.         */
  k_t_reload_over   = 0x01000000UL, /**< Reload one past the 24-bit field.   */
  k_t_reload_small  = 1234UL,       /**< Arbitrary in-range reload.          */
  k_t_sentinel      = 0xDEADBEEFUL, /**< Poison to prove no write on error.  */
  k_t_csr_all       = 0x00000007UL, /**< ENABLE | TICKINT | CLKSOURCE.       */
  k_t_csr_enable    = 0x00000001UL, /**< ENABLE only (external, no IRQ).     */
  k_t_demcr_trcena  = 0x01000000UL, /**< DEMCR.TRCENA (bit 24).              */
  k_t_dwt_cyccntena = 0x00000001UL, /**< DWT_CTRL.CYCCNTENA (bit 0).         */
  k_t_other_bit     = 0x00000002UL, /**< Unrelated bit; proves RMW preserve. */
  k_t_seed_cvr      = 0x0000ABCDUL, /**< Seeded SYST_CVR sample.             */
  k_t_seed_cyccnt   = 0x12345678UL, /**< Seeded DWT_CYCCNT sample.           */
} systick_test_const_t;

/** @brief Typed access to a seeded/observed SCS register in the fake map. */
static volatile uint32_t* reg(systick_test_reg_t addr)
{
  return (volatile uint32_t*)addr;
}

/**
 * @par MC/DC:
 * (no compound decision in the path this case touches -- the RA8_CHECK_NULL_PTR
 * guard is a single `ptr == nullptr` condition)
 */
static void test_reload_for_rejects_null(void)
{
  TEST_BEGIN("reload_for rejects NULL out_reload");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_systick_reload_for(k_t_cpu_1ghz, k_t_tick_1khz, nullptr));
  TEST_END("reload_for rejects NULL out_reload");
}

/**
 * @par MC/DC:
 * (no compound decision on the happy path -- the zero-input guard, the
 * ticks==0 guard, and the range guard are each single-condition and all
 * evaluate false here)
 */
static void test_reload_for_computes_values(void)
{
  TEST_BEGIN("reload_for computes clock-driven reloads");
  ra8_fake_mmap_reset();
  uint32_t reload = (uint32_t)k_t_sentinel;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_systick_reload_for(k_t_cpu_1ghz, k_t_tick_1khz, &reload));
  TEST_ASSERT_EQ(k_t_reload_1ghz, reload);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_systick_reload_for(k_t_cpu_moco, k_t_tick_1khz, &reload));
  TEST_ASSERT_EQ(k_t_reload_moco, reload);
  TEST_END("reload_for computes clock-driven reloads");
}

/**
 * @brief MC/DC for the compound zero-input guard.
 *
 * @par MC/DC:
 * Decision: `if ((cpu_hz == 0U) || (tick_hz == 0U))` (2 conditions) in
 * libs/ra8_core/src/ra8_systick.c@ra8_systick_reload_for.
 * - V1: cpu_hz=1e9, tick_hz=1000 -> false||false -> false (ok path).
 * - V2: cpu_hz=0,   tick_hz=1000 -> true         -> true  (varies cpu_hz).
 * - V3: cpu_hz=1e9, tick_hz=0    -> false||true  -> true  (varies tick_hz).
 * V1 vs V2 prove cpu_hz independently affects the outcome; V1 vs V3 prove the
 * same for tick_hz. N+1 = 3 vectors for N=2: minimal MC/DC. On both true
 * vectors the out param is left unmodified (the poison value survives).
 */
static void test_reload_for_mcdc_zero_inputs(void)
{
  TEST_BEGIN("reload_for MC/DC: zero-input guard");
  ra8_fake_mmap_reset();
  uint32_t reload = (uint32_t)k_t_sentinel;

  /* V1: both conditions false -> success, out written. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_systick_reload_for(k_t_cpu_1ghz, k_t_tick_1khz, &reload));
  TEST_ASSERT_EQ(k_t_reload_1ghz, reload);

  /* V2: first condition true (cpu_hz == 0) -> invalid_arg, out untouched. */
  reload = (uint32_t)k_t_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_systick_reload_for(0U, k_t_tick_1khz, &reload));
  TEST_ASSERT_EQ(k_t_sentinel, reload);

  /* V3: second condition true (tick_hz == 0) -> invalid_arg, out untouched. */
  reload = (uint32_t)k_t_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_systick_reload_for(k_t_cpu_1ghz, 0U, &reload));
  TEST_ASSERT_EQ(k_t_sentinel, reload);
  TEST_END("reload_for MC/DC: zero-input guard");
}

/**
 * @par MC/DC:
 * (single-condition guard `if (ticks == 0U)` -- 500 Hz / 1 kHz = 0 ticks
 * exercises the true branch; the false branch is covered by the happy-path
 * cases)
 */
static void test_reload_for_rejects_clock_below_tick(void)
{
  TEST_BEGIN("reload_for rejects clock below one tick");
  ra8_fake_mmap_reset();
  uint32_t reload = (uint32_t)k_t_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_systick_reload_for(k_t_cpu_below, k_t_tick_1khz, &reload));
  TEST_ASSERT_EQ(k_t_sentinel, reload);
  TEST_END("reload_for rejects clock below one tick");
}

/**
 * @par MC/DC:
 * (single-condition guard `if (reload > k_ra8_systick_rvr_max)` -- the over-max
 * clock takes the true branch and the exact-fit clock the false branch)
 */
static void test_reload_for_rejects_overflow(void)
{
  TEST_BEGIN("reload_for rejects 24-bit overflow");
  ra8_fake_mmap_reset();
  uint32_t reload = (uint32_t)k_t_sentinel;

  /* Exact fit: reload == ceiling -> accepted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_systick_reload_for(k_t_cpu_fits_max, k_t_tick_1hz, &reload));
  TEST_ASSERT_EQ(k_t_reload_max, reload);

  /* One past the ceiling -> rejected, out untouched. */
  reload = (uint32_t)k_t_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_systick_reload_for(k_t_cpu_over_max, k_t_tick_1hz, &reload));
  TEST_ASSERT_EQ(k_t_sentinel, reload);
  TEST_END("reload_for rejects 24-bit overflow");
}

/**
 * @par MC/DC:
 * (no compound decision -- the two option ternaries are single-condition; this
 * case drives the CPU-clock + tick-IRQ arm of both)
 */
static void test_configure_cpu_clock_with_irq(void)
{
  TEST_BEGIN("configure arms SysTick: CPU clock + tick IRQ");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_systick_configure(k_t_reload_1ghz, k_ra8_systick_clk_cpu, true));
  TEST_ASSERT_EQ(k_t_csr_all, *reg(k_t_csr));
  TEST_ASSERT_EQ(k_t_reload_1ghz, *reg(k_t_rvr));
  TEST_ASSERT_EQ(0U, *reg(k_t_cvr));
  TEST_END("configure arms SysTick: CPU clock + tick IRQ");
}

/**
 * @par MC/DC:
 * (no compound decision -- drives the external-clock + no-IRQ arm of the two
 * option ternaries, so ENABLE is the only bit set)
 */
static void test_configure_external_no_irq(void)
{
  TEST_BEGIN("configure arms SysTick: external clock, no IRQ");
  ra8_fake_mmap_reset();
  /* Seed CVR so the clear is observable. */
  *reg(k_t_cvr) = (uint32_t)k_t_sentinel;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_systick_configure(k_t_reload_moco, k_ra8_systick_clk_external, false));
  TEST_ASSERT_EQ(k_t_csr_enable, *reg(k_t_csr));
  TEST_ASSERT_EQ(k_t_reload_moco, *reg(k_t_rvr));
  TEST_ASSERT_EQ(0U, *reg(k_t_cvr));
  TEST_END("configure arms SysTick: external clock, no IRQ");
}

/**
 * @par MC/DC:
 * (single-condition guard `if (reload > k_ra8_systick_rvr_max)` -- true branch;
 * proves no register is written when the reload is rejected)
 */
static void test_configure_rejects_overflow(void)
{
  TEST_BEGIN("configure rejects an out-of-range reload without writing");
  ra8_fake_mmap_reset();
  *reg(k_t_csr) = (uint32_t)k_t_sentinel;
  *reg(k_t_rvr) = (uint32_t)k_t_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_systick_configure(k_t_reload_over, k_ra8_systick_clk_cpu, true));
  /* Untouched on error. */
  TEST_ASSERT_EQ(k_t_sentinel, *reg(k_t_csr));
  TEST_ASSERT_EQ(k_t_sentinel, *reg(k_t_rvr));
  TEST_END("configure rejects an out-of-range reload without writing");
}

/**
 * @par MC/DC:
 * (single-condition guard `if (reload > k_ra8_systick_rvr_max)` -- false branch
 * here; set_reload writes RVR and clears CVR but leaves CSR alone)
 */
static void test_set_reload_rearms_and_clears(void)
{
  TEST_BEGIN("set_reload re-arms RVR, clears CVR, preserves CSR");
  ra8_fake_mmap_reset();
  /* Pre-arm CSR + CVR to prove set_reload touches only RVR/CVR. */
  *reg(k_t_csr) = (uint32_t)k_t_csr_all;
  *reg(k_t_cvr) = (uint32_t)k_t_sentinel;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_systick_set_reload(k_t_reload_small));
  TEST_ASSERT_EQ(k_t_reload_small, *reg(k_t_rvr));
  TEST_ASSERT_EQ(0U, *reg(k_t_cvr));
  TEST_ASSERT_EQ(k_t_csr_all, *reg(k_t_csr));
  TEST_END("set_reload re-arms RVR, clears CVR, preserves CSR");
}

/**
 * @par MC/DC:
 * (single-condition guard `if (reload > k_ra8_systick_rvr_max)` -- true branch;
 * RVR must be untouched when the reload is rejected)
 */
static void test_set_reload_rejects_overflow(void)
{
  TEST_BEGIN("set_reload rejects an out-of-range reload without writing");
  ra8_fake_mmap_reset();
  *reg(k_t_rvr) = (uint32_t)k_t_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_systick_set_reload(k_t_reload_over));
  TEST_ASSERT_EQ(k_t_sentinel, *reg(k_t_rvr));
  TEST_END("set_reload rejects an out-of-range reload without writing");
}

/**
 * @par MC/DC:
 * (no decision -- a straight read of SYST_CVR)
 */
static void test_current_value_reads_cvr(void)
{
  TEST_BEGIN("current_value reads SYST_CVR");
  ra8_fake_mmap_reset();
  *reg(k_t_cvr) = (uint32_t)k_t_seed_cvr;
  TEST_ASSERT_EQ(k_t_seed_cvr, ra8_systick_current_value());
  TEST_END("current_value reads SYST_CVR");
}

/**
 * @par MC/DC:
 * (no decision -- read-modify-write that must SET the enable bits while
 * preserving any pre-existing bits)
 */
static void test_dwt_enable_sets_bits_preserving_others(void)
{
  TEST_BEGIN("dwt enable sets TRCENA + CYCCNTENA, preserves other bits");
  ra8_fake_mmap_reset();
  *reg(k_t_demcr)    = (uint32_t)k_t_other_bit;
  *reg(k_t_dwt_ctrl) = (uint32_t)k_t_other_bit;
  ra8_dwt_cyccnt_enable();
  TEST_ASSERT_EQ(k_t_demcr_trcena | (uint32_t)k_t_other_bit, *reg(k_t_demcr));
  TEST_ASSERT_EQ(k_t_dwt_cyccntena | (uint32_t)k_t_other_bit, *reg(k_t_dwt_ctrl));
  TEST_END("dwt enable sets TRCENA + CYCCNTENA, preserves other bits");
}

/**
 * @par MC/DC:
 * (no decision -- reset writes zero, read samples the seeded value)
 */
static void test_dwt_reset_and_read(void)
{
  TEST_BEGIN("dwt reset zeroes CYCCNT; read samples it");
  ra8_fake_mmap_reset();
  *reg(k_t_cyccnt) = (uint32_t)k_t_seed_cyccnt;
  TEST_ASSERT_EQ(k_t_seed_cyccnt, ra8_dwt_cyccnt_read());
  ra8_dwt_cyccnt_reset();
  TEST_ASSERT_EQ(0U, ra8_dwt_cyccnt_read());
  TEST_END("dwt reset zeroes CYCCNT; read samples it");
}

int32_t main(void)
{
  test_reload_for_rejects_null();
  test_reload_for_computes_values();
  test_reload_for_mcdc_zero_inputs();
  test_reload_for_rejects_clock_below_tick();
  test_reload_for_rejects_overflow();
  test_configure_cpu_clock_with_irq();
  test_configure_external_no_irq();
  test_configure_rejects_overflow();
  test_set_reload_rearms_and_clears();
  test_set_reload_rejects_overflow();
  test_current_value_reads_cvr();
  test_dwt_enable_sets_bits_preserving_others();
  test_dwt_reset_and_read();
  (void)fprintf(stderr, "[OK  ] test_ra8_systick.c\n");
  return 0;
}
