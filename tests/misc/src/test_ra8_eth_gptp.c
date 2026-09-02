/**
 * @file test_ra8_eth_gptp.c
 * @brief Unit tests for ra8_eth_gptp.c (HUM Ch 35 GPTP timer driver)
 *
 * @details
 * The register window under test is the real HUM Table 35.3 map, so each
 * case either plants a value in a documented register and reads it back
 * through the driver, or drives the driver and inspects the register the
 * manual says it must have written.
 *
 * @par What these tests deliberately cannot prove
 * ``ra8_fake_mmap`` backs the GPTP aperture with ordinary host RAM, so three
 * hardware contracts are outside this file's reach and are HIL-only
 * verification objectives:
 *   - the mandated U -> M -> L offset write order (HUM 35.4.1.3.1 p 1944-1945)
 *     and the L-first read latch (HUM 35.4.1.3.4 p 1946) -- the assertions
 *     here check final values, and would pass under any ordering;
 *   - `PTPTMEC` / `PTPTMDC` write-1-to-set / write-1-to-clear coupling, which
 *     is why ``internal_test_timer_enable_disable`` inspects `PTPTMDC` directly instead
 *     of asserting that a disable clears `PTPTMEC`;
 *   - that `PTPIPV` is read-only, which the presence probe relies on --
 *     ``internal_test_ip_version`` has to write it to plant a value.
 * The per-timer `PTPTIVC` / `PTPTOVC` assertions and the 0x40-stride check in
 * ``internal_test_increment`` are the ones carrying real weight here: they prove the
 * driver reaches the HUM offsets rather than an invented window, which is the
 * defect this file exists to prevent recurring (#498).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_eth_gptp.h"
#include "ra8_ether_regs.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "unity_minimal.h"

/**
 * @enum gptp_fixture_t
 * @brief Clock frequencies and their HUM-documented TIV values.
 *
 * @details
 * HUM Ch 35.3.2.3 p 1928 gives the expected `PTPTIVCt.TIV` for four `clk`
 * frequencies; those four pairs are the driver's oracle. 31250000 Hz is the
 * exact frequency at which TIV would need bit 32, so it must be rejected.
 */
typedef enum : uint32_t {
  k_fix_hz_50m      = 50000000UL,   /**< 50 MHz  -> 0xA0000000.      */
  k_fix_hz_100m     = 100000000UL,  /**< 100 MHz -> 0x50000000.      */
  k_fix_hz_125m     = 125000000UL,  /**< 125 MHz -> 0x40000000.      */
  k_fix_hz_200m     = 200000000UL,  /**< 200 MHz -> 0x28000000.      */
  k_fix_hz_250m     = 250000000UL,  /**< 250 MHz -> 0x20000000.      */
  k_fix_hz_too_slow = 31250000UL,   /**< TIV would overflow 32 bits. */
  k_fix_tiv_50m     = 0xA0000000UL, /**< HUM worked example.         */
  k_fix_tiv_100m    = 0x50000000UL, /**< HUM worked example.         */
  k_fix_tiv_125m    = 0x40000000UL, /**< 8 ns x 2^27.                */
  k_fix_tiv_200m    = 0x28000000UL, /**< HUM worked example.         */
  k_fix_tiv_250m    = 0x20000000UL, /**< HUM worked example.         */
} gptp_fixture_t;

/**
 * @enum gptp_probe_t
 * @brief Values planted in registers to prove a read reaches them.
 *
 * @details
 * Each value is planted directly in a register through ::ra8_gptp, so the
 * matching driver read has to travel the real HUM Table 35.3 offset to find
 * it. ::k_probe_all_ones additionally proves the driver masks the reserved
 * bits off `PTPGPTPTMtL` and `PTPGPTPTMtU`.
 */
typedef enum : uint32_t {
  k_probe_ipv      = 0x00000003UL, /**< PTPIPV reset value per HUM p 1927.     */
  k_probe_all_ones = 0xFFFFFFFFUL, /**< Proves the driver masks reserved bits. */
  k_probe_nsec     = 0x0AAAAAAAUL, /**< A 30-bit nanosecond pattern.           */
  k_probe_sec_lo   = 0x12345678UL, /**< Seconds [31:0] pattern.                */
  k_probe_sec_hi   = 0x0000BEEFUL, /**< Seconds [47:32] pattern.               */
  k_probe_avtp_lo  = 0xCAFEBABEUL, /**< AVTP [31:0] pattern.                   */
  k_probe_avtp_hi  = 0x1EEE1588UL, /**< AVTP [63:32] pattern.                  */
  k_probe_tiv      = 0x33333333UL, /**< An arbitrary increment.                */
} gptp_probe_t;

/**
 * @enum gptp_offset_probe_t
 * @brief Offset values written through ::ra8_eth_gptp_set_offset.
 *
 * @details
 * ::k_probe_off_sec splits as `0x0000BEEF` / `0x12345678` across `PTPTOVCtU`
 * and `PTPTOVCtM`, which is what proves the 48-bit seconds split is wired the
 * way HUM 35.3.2.5 / 35.3.2.6 p 1929-1930 describes.
 */
typedef enum : uint64_t {
  k_probe_off_sec     = 0x0000BEEF12345678ULL, /**< 48-bit seconds value. */
  k_probe_off_sec_bad = 0x0001000000000000ULL, /**< One past 2^48 - 1.    */
} gptp_offset_probe_t;

/**
 * @enum gptp_nsec_probe_t
 * @brief Nanosecond arguments for the offset setter.
 *
 * @details
 * `PTPTOVCtL.TOVL` accepts 0..999999999 (HUM 35.3.2.4 p 1929). The pair below
 * straddles that boundary so the nanosecond guard in
 * ::ra8_eth_gptp_set_offset is exercised in both directions from one fixture.
 */
typedef enum : uint32_t {
  k_probe_off_nsec     = 999999999UL,  /**< Largest legal PTPTOVCtL value. */
  k_probe_off_nsec_bad = 1000000000UL, /**< One past the legal maximum.    */
} gptp_nsec_probe_t;

/**
 * @enum gptp_field_probe_t
 * @brief Field widths the driver must honour when it assembles a time.
 *
 * @details
 * `PTPGPTPTMtL.GPTPL` is 30 bits and `PTPGPTPTMtU.GPTPU` is 16 bits
 * (HUM Ch 35.3.3.3 p 1931 / 35.3.3.5 p 1932); the seconds upper part sits
 * at 2^32 in the assembled value.
 */
typedef enum : uint32_t {
  k_probe_mask_nsec  = 0x3FFFFFFFUL, /**< GPTPL[29:0].               */
  k_probe_mask_sec_u = 0x0000FFFFUL, /**< GPTPU[15:0].               */
  k_probe_shift_hi   = 32UL,         /**< Upper word starts at 2^32. */
} gptp_field_probe_t;

/**
 * @brief Reset the simulated MMIO window and the MSTP driver.
 *
 * @details
 * Clears every mapped register region so one case's writes cannot leak into
 * the next, then re-runs ``ra8_mstp_init`` so the ESWM refcount starts at 0
 * and ``ra8_eth_gptp_init`` sees a fresh gate.
 *
 * @pre Host test binary with the MMIO windows mapped.
 * @pre No other thread touches the GPTP window (single-threaded test).
 * @post The GPTP register window reads all zeroes.
 * @post The MSTP refcount table is empty.
 *
 * @note Thread-unsafe -- single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Bring the driver up at 125 MHz, failing the test if it refuses.
 *
 * @details
 * 125 MHz is not one of the HUM's four worked examples, so it also proves the
 * derivation is a formula rather than a lookup table: 8 ns x 2^27 =
 * 0x4000_0000.
 *
 * @pre ::internal_prep has reset the MMIO window and the MSTP driver.
 * @pre The caller is inside a ``TEST_BEGIN`` / ``TEST_END`` pair.
 * @post Both timer units hold ::k_fix_tiv_125m in `PTPTIVCt`.
 * @post Both timer units are stopped with a zero offset.
 *
 * @note Thread-unsafe -- single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_arm_125m(void)
{
  const ra8_eth_gptp_cfg_t cfg = {.clk_hz = (uint32_t)k_fix_hz_125m};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_init(&cfg));
}

/**
 * @test tiv_from_hz_matches_hum_examples
 *
 * @par MC/DC:
 * (no compound decisions in this test -- every guard in
 * ``ra8_eth_gptp_tiv_from_hz`` is a single condition, exercised here in
 * both directions; no `&&` or `||` in the code under test) @brief Verify tiv from hz behavior. @details Executes the tiv from hz scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_tiv_from_hz(void)
{
  TEST_BEGIN("gptp tiv from hz");
  internal_prep();
  uint32_t tiv = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_tiv_from_hz((uint32_t)k_fix_hz_50m, &tiv));
  TEST_ASSERT_EQ(k_fix_tiv_50m, tiv);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_tiv_from_hz((uint32_t)k_fix_hz_100m, &tiv));
  TEST_ASSERT_EQ(k_fix_tiv_100m, tiv);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_tiv_from_hz((uint32_t)k_fix_hz_125m, &tiv));
  TEST_ASSERT_EQ(k_fix_tiv_125m, tiv);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_tiv_from_hz((uint32_t)k_fix_hz_200m, &tiv));
  TEST_ASSERT_EQ(k_fix_tiv_200m, tiv);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_tiv_from_hz((uint32_t)k_fix_hz_250m, &tiv));
  TEST_ASSERT_EQ(k_fix_tiv_250m, tiv);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_eth_gptp_tiv_from_hz((uint32_t)k_fix_hz_125m, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gptp_tiv_from_hz(0U, &tiv));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_eth_gptp_tiv_from_hz((uint32_t)k_fix_hz_too_slow, &tiv));
  TEST_END("gptp tiv from hz");
}

/**
 * @test init_programs_both_timers
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the init guards are single
 * conditions and the loop bound is a constant; no `&&` or `||` in the code
 * under test) @brief Verify init behavior. @details Executes the init scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init(void)
{
  TEST_BEGIN("gptp init");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_eth_gptp_init(nullptr));

  const ra8_eth_gptp_cfg_t bad = {.clk_hz = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gptp_init(&bad));

  const ra8_eth_gptp_cfg_t slow = {.clk_hz = (uint32_t)k_fix_hz_too_slow};
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_eth_gptp_init(&slow));

  internal_arm_125m();
  volatile r_gptp_regs_t* reg = ra8_gptp();
  TEST_ASSERT_EQ(k_fix_tiv_125m, reg->TIMER[k_ra8_gptp_timer_0].PTPTIVC);
  TEST_ASSERT_EQ(k_fix_tiv_125m, reg->TIMER[k_ra8_gptp_timer_1].PTPTIVC);
  TEST_ASSERT_EQ(0U, reg->TIMER[k_ra8_gptp_timer_0].PTPTOVCL);
  TEST_ASSERT_EQ(0U, reg->TIMER[k_ra8_gptp_timer_1].PTPTOVCU);
  /* Both units were stopped: PTPTMDC last carried timer 1's disable bit. */
  TEST_ASSERT_EQ(1UL << (uint32_t)k_ra8_gptp_timer_1, reg->PTPTMDC);
  TEST_END("gptp init");
}

/**
 * @test deinit_restores_reset_values
 *
 * @par MC/DC:
 * (no compound decisions in this test -- ``ra8_eth_gptp_deinit`` has no
 * conditional at all; no `&&` or `||` in the code under test) @brief Verify deinit behavior. @details Executes the deinit scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_deinit(void)
{
  TEST_BEGIN("gptp deinit");
  internal_prep();
  internal_arm_125m();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_deinit());
  volatile r_gptp_regs_t* reg = ra8_gptp();
  TEST_ASSERT_EQ(0U, reg->TIMER[k_ra8_gptp_timer_0].PTPTIVC);
  TEST_ASSERT_EQ(0U, reg->TIMER[k_ra8_gptp_timer_1].PTPTIVC);
  TEST_END("gptp deinit");
}

/**
 * @test ip_version_reads_ptpipv
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the only guard is a null check;
 * no `&&` or `||` in the code under test) @brief Verify ip version behavior. @details Executes the ip version scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_ip_version(void)
{
  TEST_BEGIN("gptp ip version");
  internal_prep();
  internal_arm_125m();
  ra8_gptp()->PTPIPV = (uint32_t)k_probe_ipv;
  uint32_t version   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_ip_version(&version));
  TEST_ASSERT_EQ(k_probe_ipv, version);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_eth_gptp_ip_version(nullptr));
  TEST_END("gptp ip version");
}

/**
 * @test timer_enable_disable_hit_ptptmec_ptptmdc
 *
 * @par MC/DC:
 * (no compound decisions in this test -- each range guard is a single
 * condition, exercised true and false; no `&&` or `||` in the code under
 * test) @brief Verify timer enable disable behavior. @details Executes the timer enable disable scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_timer_enable_disable(void)
{
  TEST_BEGIN("gptp timer enable/disable");
  internal_prep();
  internal_arm_125m();
  volatile r_gptp_regs_t* reg = ra8_gptp();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_timer_enable(k_ra8_gptp_timer_1));
  TEST_ASSERT_EQ(1UL << (uint32_t)k_ra8_gptp_timer_1, reg->PTPTMEC);

  bool enabled = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_timer_is_enabled(k_ra8_gptp_timer_1, &enabled));
  TEST_ASSERT(enabled);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_timer_is_enabled(k_ra8_gptp_timer_0, &enabled));
  TEST_ASSERT(!enabled);

  reg->PTPTMDC = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_timer_disable(k_ra8_gptp_timer_0));
  TEST_ASSERT_EQ(1UL << (uint32_t)k_ra8_gptp_timer_0, reg->PTPTMDC);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gptp_timer_enable(k_ra8_gptp_timer_count));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gptp_timer_disable(k_ra8_gptp_timer_count));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_eth_gptp_timer_is_enabled(k_ra8_gptp_timer_count, &enabled));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_eth_gptp_timer_is_enabled(k_ra8_gptp_timer_0, nullptr));
  TEST_END("gptp timer enable/disable");
}

/**
 * @test increment_round_trips_per_timer
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the setter's two guards are
 * separate single-condition `if`s, each exercised both ways; no `&&` or
 * `||` in the code under test) @brief Verify increment behavior. @details Executes the increment scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_increment(void)
{
  TEST_BEGIN("gptp increment");
  internal_prep();
  internal_arm_125m();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_set_increment(k_ra8_gptp_timer_1, (uint32_t)k_probe_tiv));
  uint32_t tiv = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_get_increment(k_ra8_gptp_timer_1, &tiv));
  TEST_ASSERT_EQ(k_probe_tiv, tiv);
  /* The 0x40 stride must keep timer 0 untouched (HUM Table 35.3 p 1926). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_get_increment(k_ra8_gptp_timer_0, &tiv));
  TEST_ASSERT_EQ(k_fix_tiv_125m, tiv);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_eth_gptp_set_increment(k_ra8_gptp_timer_count, (uint32_t)k_probe_tiv));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gptp_set_increment(k_ra8_gptp_timer_0, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gptp_get_increment(k_ra8_gptp_timer_count, &tiv));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_eth_gptp_get_increment(k_ra8_gptp_timer_0, nullptr));
  TEST_END("gptp increment");
}

/**
 * @test set_offset_writes_u_m_l
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the three range guards are
 * separate single-condition `if`s, each exercised both ways; no `&&` or
 * `||` in the code under test) @brief Verify set offset behavior. @details Executes the set offset scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_set_offset(void)
{
  TEST_BEGIN("gptp set offset");
  internal_prep();
  internal_arm_125m();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_eth_gptp_set_offset(k_ra8_gptp_timer_0,
                                         (uint64_t)k_probe_off_sec,
                                         (uint32_t)k_probe_off_nsec));
  volatile r_gptp_timer_regs_t* t0 = &ra8_gptp()->TIMER[k_ra8_gptp_timer_0];
  TEST_ASSERT_EQ(k_probe_sec_hi, t0->PTPTOVCU);
  TEST_ASSERT_EQ(k_probe_sec_lo, t0->PTPTOVCM);
  TEST_ASSERT_EQ(k_probe_off_nsec, t0->PTPTOVCL);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gptp_set_offset(k_ra8_gptp_timer_count, 0ULL, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_eth_gptp_set_offset(k_ra8_gptp_timer_0, (uint64_t)k_probe_off_sec_bad, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_eth_gptp_set_offset(k_ra8_gptp_timer_0, 0ULL, (uint32_t)k_probe_off_nsec_bad));
  TEST_END("gptp set offset");
}

/**
 * @test get_time_assembles_78_bits
 *
 * @par MC/DC:
 * (no compound decisions in this test -- two null guards and one range
 * guard, each a single condition exercised both ways; no `&&` or `||` in
 * the code under test) @brief Verify get time behavior. @details Executes the get time scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_get_time(void)
{
  TEST_BEGIN("gptp get time");
  internal_prep();
  internal_arm_125m();
  volatile r_gptp_timer_regs_t* t1 = &ra8_gptp()->TIMER[k_ra8_gptp_timer_1];
  t1->PTPGPTPTML                   = (uint32_t)k_probe_nsec;
  t1->PTPGPTPTMM                   = (uint32_t)k_probe_sec_lo;
  t1->PTPGPTPTMU                   = (uint32_t)k_probe_sec_hi;

  uint64_t sec  = 0U;
  uint32_t nsec = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_get_time(k_ra8_gptp_timer_1, &sec, &nsec));
  TEST_ASSERT_EQ(k_probe_nsec, nsec);
  TEST_ASSERT(
    sec == (((uint64_t)k_probe_sec_hi << (uint64_t)k_probe_shift_hi) | (uint64_t)k_probe_sec_lo));

  /* Reserved bits must not leak into either field (HUM p 1931-1932). */
  t1->PTPGPTPTML = (uint32_t)k_probe_all_ones;
  t1->PTPGPTPTMU = (uint32_t)k_probe_all_ones;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_get_time(k_ra8_gptp_timer_1, &sec, &nsec));
  TEST_ASSERT_EQ(k_probe_mask_nsec, nsec);
  TEST_ASSERT(sec == (((uint64_t)k_probe_mask_sec_u << (uint64_t)k_probe_shift_hi) |
                      (uint64_t)k_probe_sec_lo));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gptp_get_time(k_ra8_gptp_timer_count, &sec, &nsec));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_eth_gptp_get_time(k_ra8_gptp_timer_0, nullptr, &nsec));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_eth_gptp_get_time(k_ra8_gptp_timer_0, &sec, nullptr));
  TEST_END("gptp get time");
}

/**
 * @test get_avtp_ns_assembles_64_bits
 *
 * @par MC/DC:
 * (no compound decisions in this test -- one null guard and one range
 * guard, each a single condition exercised both ways; no `&&` or `||` in
 * the code under test) @brief Verify get avtp ns behavior. @details Executes the get avtp ns scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_get_avtp_ns(void)
{
  TEST_BEGIN("gptp get avtp ns");
  internal_prep();
  internal_arm_125m();
  volatile r_gptp_timer_regs_t* t0 = &ra8_gptp()->TIMER[k_ra8_gptp_timer_0];
  t0->PTPAVTPTML                   = (uint32_t)k_probe_avtp_lo;
  t0->PTPAVTPTMU                   = (uint32_t)k_probe_avtp_hi;

  uint64_t ns = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_get_avtp_ns(k_ra8_gptp_timer_0, &ns));
  TEST_ASSERT(
    ns == (((uint64_t)k_probe_avtp_hi << (uint64_t)k_probe_shift_hi) | (uint64_t)k_probe_avtp_lo));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_gptp_get_avtp_ns(k_ra8_gptp_timer_count, &ns));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_eth_gptp_get_avtp_ns(k_ra8_gptp_timer_0, nullptr));
  TEST_END("gptp get avtp ns");
}

/**
 * @test power_transition_keeps_configuration
 *
 * @par MC/DC:
 * (no compound decisions in this test -- neither stop-path function has a
 * conditional; no `&&` or `||` in the code under test) @brief Verify power transition behavior. @details Executes the power transition scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_power_transition(void)
{
  TEST_BEGIN("gptp power transition");
  internal_prep();
  internal_arm_125m();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_enter_stop());
  /* A module stop is a clock gate, not a reset: TIV must survive it. */
  TEST_ASSERT_EQ(k_fix_tiv_125m, ra8_gptp()->TIMER[k_ra8_gptp_timer_0].PTPTIVC);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_exit_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_timer_enable(k_ra8_gptp_timer_0));
  TEST_END("gptp power transition");
}

/**
 * @test every_entry_point_rejects_use_before_init
 *
 * @details
 * The GPTP window sits inside the ESWM module-stop domain, so touching it
 * before ``ra8_eth_gptp_init`` has released MSTPC30 takes a bus fault on
 * silicon. Every entry point must therefore refuse rather than reach the
 * register.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the guard is a single condition per
 * function, exercised false here and true by every other case in this file;
 * no `&&` or `||` in the code under test) @brief Verify use before init behavior. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_use_before_init(void)
{
  TEST_BEGIN("gptp use before init");
  internal_prep();
  /* Leave the driver un-initialised: a previous case may have opened it. */
  internal_arm_125m();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_deinit());

  uint32_t word    = 0U;
  uint64_t sec     = 0U;
  uint32_t nsec    = 0U;
  uint64_t ns      = 0U;
  bool     enabled = false;

  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_eth_gptp_deinit());
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_eth_gptp_ip_version(&word));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_eth_gptp_timer_enable(k_ra8_gptp_timer_0));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_eth_gptp_timer_disable(k_ra8_gptp_timer_0));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_eth_gptp_timer_is_enabled(k_ra8_gptp_timer_0, &enabled));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_eth_gptp_set_increment(k_ra8_gptp_timer_0, (uint32_t)k_probe_tiv));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_eth_gptp_get_increment(k_ra8_gptp_timer_0, &word));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_eth_gptp_set_offset(k_ra8_gptp_timer_0, 0ULL, 0U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_eth_gptp_get_time(k_ra8_gptp_timer_0, &sec, &nsec));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_eth_gptp_get_avtp_ns(k_ra8_gptp_timer_0, &ns));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_eth_gptp_enter_stop());
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_eth_gptp_exit_stop());

  /* The pure helper needs no hardware and must still work. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_tiv_from_hz((uint32_t)k_fix_hz_125m, &word));
  TEST_ASSERT_EQ(k_fix_tiv_125m, word);
  TEST_END("gptp use before init");
}

/**
 * @test module_stop_timeout_is_reported
 *
 * @details
 * The ESWM module-stop handshake is the one failure ``ra8_eth_gptp_init``
 * cannot recover from, and every stop-path function documents
 * ``k_ra8_err_hw_timeout``. The ``ra8_fake_mmio`` seam in
 * ``priv_ra8_mstp_wait_reg_settle_internal`` makes that leg reachable on the host,
 * so it is asserted rather than excluded from coverage.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the propagation is a single
 * ``RA8_RETURN_ON_ERROR``; no `&&` or `||` in the code under test) @brief Verify mstp timeout behavior. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mstp_timeout(void)
{
  TEST_BEGIN("gptp mstp timeout");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&ra8_mstp()->MSTPCRC));
  const ra8_eth_gptp_cfg_t cfg = {.clk_hz = (uint32_t)k_fix_hz_125m};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_eth_gptp_init(&cfg));

  /* A failed init must not unlock the rest of the driver. */
  uint32_t word = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_eth_gptp_ip_version(&word));

  /* With the seam cleared the same call succeeds, proving the fault was the
   * injected one and not a latent driver defect. */
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_deinit());
  TEST_END("gptp mstp timeout");
}

int main(void)
{
  internal_test_tiv_from_hz();
  internal_test_init();
  internal_test_deinit();
  internal_test_ip_version();
  internal_test_timer_enable_disable();
  internal_test_increment();
  internal_test_set_offset();
  internal_test_get_time();
  internal_test_get_avtp_ns();
  internal_test_power_transition();
  internal_test_use_before_init();
  internal_test_mstp_timeout();
  return 0;
}
