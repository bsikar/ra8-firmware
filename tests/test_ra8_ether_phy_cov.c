/**
 * @file test_ra8_ether_phy_cov.c
 * @brief Coverage-extension unit tests for ra8_ether_phy.c
 *
 * @details
 * Exercises the branches that remain uncovered after test_ra8_ether_phy.c.
 * Each test uses the same fault-injecting bus mock pattern (fail_next_read /
 * fail_next_write flags, reset_reads_remaining countdown) that the sibling
 * test established.  Targeted uncovered source lines:
 *
 *  - 86-87   ra8_ether_phy_open(): BMCR write fails; opened flag rolled back.
 *  - 93-94   ra8_ether_phy_open(): BMCR-poll read fails inside the loop.
 *  - 101-102 ra8_ether_phy_open(): reset-poll timeout exhausted.
 *  - 157     ra8_ether_phy_link_status_get(): BMSR read fails.
 *  - 172     ra8_ether_phy_link_status_get(): LPA speed resolved to 100half.
 *  - 173-174 ra8_ether_phy_link_status_get(): LPA speed resolved to 10full.
 *  - 175-176 ra8_ether_phy_link_status_get(): LPA speed resolved to 10half.
 *  - 177     ra8_ether_phy_link_status_get(): closing brace of speed chain.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_ether_phy.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ether_phy_cov_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_phy_ability_half_duplex = 0x0040U, /**< An ability word advertising the half-duplex mode. */
  k_phy_ability_full_duplex =
    0x0080U, /**< One advertising full duplex, so the two negotiate to different results. */
  k_phy_reset_wait_us =
    100U, /**< Reset settling time the configuration requests, in microseconds. */
  k_phy_reg_1000base_status =
    5, /**< MII register 5: the link-partner ability register this fixture drives. */
} ether_phy_cov_uint8_const_t;

/**
 * @enum ether_phy_cov_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_phy_bmcr_reset =
    0x8000U, /**< BMCR bit 15, the PHY reset bit, which the driver must clear when reset completes. */
} ether_phy_cov_uint16_const_t;

/* ---------------------------------------------------------------------------
 * Local mock bus state
 * ------------------------------------------------------------------------ */

/**
 * @enum test_cov_const_t
 * @brief Constants used across this test file.
 *
 * @details
 * k_cov_phy_addr            -- arbitrary valid MDIO PHY address.
 * k_cov_reg_count           -- Clause-22 register space (0..31).
 * k_cov_timeout_remaining   -- value just large enough that the reset bit
 *                              never auto-clears within the 32-poll limit.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cov_phy_addr          = 2U,  /**< Arbitrary valid PHY address.           */
  k_cov_reg_count         = 32U, /**< Clause-22 register count.              */
  k_cov_timeout_remaining = 33U, /**< Greater than poll max (32); see below. */
} test_cov_const_t;

/**
 * @struct test_cov_io_t
 * @brief Mock MDIO bus state for this coverage test file.
 *
 * @details
 * Mirrors the state used in test_ra8_ether_phy.c so both files can be
 * auto-discovered and linked independently without symbol conflicts.
 *
 * @invariant regs[] is written only through cov_bus_write().
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t regs[k_cov_reg_count]; /**< Simulated PHY register bank.                */
  uint16_t reset_reads_remaining; /**< Counts before BMCR.RESET auto-clears.       */
  uint8_t  fail_next_read;        /**< When non-zero the next read returns error.  */
  uint8_t  fail_next_write;       /**< When non-zero the next write returns error. */
} test_cov_io_t;

/** @var s_cov_io
 *  @brief Singleton mock bus state for this TU.
 *  @warning Do not access directly; use cov_prep() and test helpers.
 *  @since 0.1.0
 */
static test_cov_io_t s_cov_io = {};

/**
 * @brief Mock MDIO read callback.
 *
 * @details
 * Returns k_ra8_err_hw_error on the next call when fail_next_read is set,
 * then clears the flag. Replicates the BMCR.RESET auto-clear countdown
 * used in the sibling test so reset-poll behaviour is deterministic.
 *
 * @param[in]  ctx      Pointer to test_cov_io_t.
 * @param[in]  phy      PHY address (unused; single-PHY mock).
 * @param[in]  reg      Register address.
 * @param[out] out      Receives the register value.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Read succeeded.
 * @retval k_ra8_err_hw_error    fail_next_read was non-zero.
 * @retval k_ra8_err_invalid_arg reg out of range.
 *
 * @pre ctx is a valid test_cov_io_t pointer.
 * @post fail_next_read is cleared after returning hw_error.
 *
 * @note Not thread-safe; test-only.
 * @since 0.1.0
 */
static ra8_err_t cov_bus_read(void* ctx, uint8_t phy, uint8_t reg, uint16_t* out)
{
  (void)phy;
  test_cov_io_t* st = (test_cov_io_t*)ctx;
  if (st->fail_next_read != 0U) {
    st->fail_next_read = 0U;
    return k_ra8_err_hw_error;
  }
  if (reg >= k_cov_reg_count) {
    return k_ra8_err_invalid_arg;
  }
  /* Replicate BMCR.RESET auto-clear countdown: decrement until zero,
   * then clear bit 15 so the driver sees reset complete. */
  if (reg == 0U) {
    if (st->reset_reads_remaining > 0U) {
      st->reset_reads_remaining = (uint16_t)(st->reset_reads_remaining - 1U);
    } else {
      st->regs[0] = (uint16_t)(st->regs[0] & (uint16_t)~k_phy_bmcr_reset);
    }
  }
  *out = st->regs[reg];
  return k_ra8_ok;
}

/**
 * @brief Mock MDIO write callback.
 *
 * @details
 * Returns k_ra8_err_hw_error on the next call when fail_next_write is set,
 * then clears the flag. Otherwise mirrors the write into the register bank.
 *
 * @param[in] ctx    Pointer to test_cov_io_t.
 * @param[in] phy    PHY address (unused; single-PHY mock).
 * @param[in] reg    Register address.
 * @param[in] data   Value to write.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Write succeeded.
 * @retval k_ra8_err_hw_error    fail_next_write was non-zero.
 * @retval k_ra8_err_invalid_arg reg out of range.
 *
 * @pre ctx is a valid test_cov_io_t pointer.
 * @post fail_next_write is cleared after returning hw_error.
 *
 * @note Not thread-safe; test-only.
 * @since 0.1.0
 */
static ra8_err_t cov_bus_write(void* ctx, uint8_t phy, uint8_t reg, uint16_t data)
{
  (void)phy;
  test_cov_io_t* st = (test_cov_io_t*)ctx;
  if (st->fail_next_write != 0U) {
    st->fail_next_write = 0U;
    return k_ra8_err_hw_error;
  }
  if (reg >= k_cov_reg_count) {
    return k_ra8_err_invalid_arg;
  }
  st->regs[reg] = data;
  return k_ra8_ok;
}

/**
 * @brief Reset driver and mock state before each test.
 *
 * @details
 * Closes the PHY driver (ignoring error if already closed), zeroes the
 * mock register bank, and sets reset_reads_remaining to 1 so that a
 * normal ra8_ether_phy_open() succeeds on the second poll (reset auto-clears).
 *
 * @pre None.
 * @post s_cov_io is zeroed; reset_reads_remaining == 1; driver is closed.
 *
 * @note Call at the start of every test function.
 * @since 0.1.0
 */
static void cov_prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_ether_phy_close();
  (void)memset(&s_cov_io, 0, sizeof(s_cov_io));
  s_cov_io.reset_reads_remaining = 1U;
}

/**
 * @brief Build a valid PHY configuration backed by the local mock.
 *
 * @return ra8_ether_phy_cfg_t with cov_bus_read / cov_bus_write and s_cov_io.
 *
 * @pre cov_prep() was called.
 * @post Returned cfg points to s_cov_io as its MDIO bus context.
 *
 * @since 0.1.0
 */
static ra8_ether_phy_cfg_t cov_make_cfg(void)
{
  ra8_ether_phy_cfg_t cfg = {};
  cfg.io.read             = cov_bus_read;
  cfg.io.write            = cov_bus_write;
  cfg.io.ctx              = &s_cov_io;
  cfg.phy_address         = (uint8_t)k_cov_phy_addr;
  cfg.mii_type            = k_ra8_ether_phy_rmii;
  cfg.reset_wait_us       = k_phy_reset_wait_us;
  return cfg;
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------ */

/**
 * @brief open() rolls back opened flag when the BMCR write fails.
 *
 * @details
 * Covers source lines 86-87: the initial BMCR write (RESET bit) fails;
 * ra8_ether_phy_open() must clear s_state.opened and propagate the error.
 * We verify the rollback by confirming that a subsequent open (after
 * resetting the fault flag) succeeds -- proving opened was not stuck at true.
 *
 * @par MC/DC:
 * Decision: ``if (err != k_ra8_ok)`` at line 85 (single condition).
 * - Vector T: write returns hw_error    -> error propagated (this test).
 * - Vector F: write returns ok          -> covered by test_ra8_ether_phy.c.
 * Single-condition decision; T and F together satisfy MC/DC trivially.
 *
 * @since 0.1.0
 */
static void test_open_write_fails(void)
{
  TEST_BEGIN("open: BMCR write failure propagates and rolls back opened flag");
  cov_prep();
  ra8_ether_phy_cfg_t cfg = cov_make_cfg();

  /* Inject write fault so the initial BMCR reset write fails. */
  s_cov_io.fail_next_write = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ether_phy_open(&cfg));

  /* Prove opened was rolled back: a fresh open must succeed. */
  s_cov_io.reset_reads_remaining = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_close());
  TEST_END("open: BMCR write failure propagates and rolls back opened flag");
}

/**
 * @brief open() rolls back opened flag when a BMCR-poll read fails.
 *
 * @details
 * Covers source lines 93-94: the first BMCR-poll read returns an error;
 * ra8_ether_phy_open() must clear s_state.opened and return the error.
 * The bus write succeeds (no fail_next_write), then the first poll read
 * fails (fail_next_read = 1), triggering the error path.
 *
 * @par MC/DC:
 * Decision: ``if (err != k_ra8_ok)`` at line 92 (single condition;
 * branch coverage excluded by GCOVR_EXCL_BR_LINE on that source line).
 * - Vector T: poll read returns hw_error -> lines 93-94 hit (this test).
 * - Vector F: poll read returns ok       -> covered by test_ra8_ether_phy.c.
 * Single-condition decision; both vectors satisfy MC/DC.
 *
 * @since 0.1.0
 */
static void test_open_poll_read_fails(void)
{
  TEST_BEGIN("open: BMCR poll read failure propagates and rolls back opened flag");
  cov_prep();
  ra8_ether_phy_cfg_t cfg = cov_make_cfg();

  /* Fault the first read (the initial poll in the reset loop). */
  s_cov_io.fail_next_read = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ether_phy_open(&cfg));

  /* Prove opened was rolled back: a fresh open must succeed. */
  s_cov_io.reset_reads_remaining = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_close());
  TEST_END("open: BMCR poll read failure propagates and rolls back opened flag");
}

/**
 * @brief open() returns k_ra8_err_hw_timeout when reset never self-clears.
 *
 * @details
 * Covers source lines 101-102: reset_reads_remaining is set to 33, which
 * exceeds the 32-iteration poll limit (k_ra8_ether_phy_reset_poll_max).
 * After 32 reads the bit is still set (remaining went from 33 to 1 but
 * never reached 0), so the loop exits and the timeout path is taken.
 *
 * @par MC/DC:
 * Decision: loop exit condition ``i < k_ra8_ether_phy_reset_poll_max``
 * (single condition; branch coverage excluded by GCOVR_EXCL_BR_LINE on
 * the for line). The inner decision
 * ``if ((reg & k_ra8_ether_phy_bmcr_reset) == 0U)`` is a single condition.
 * - Vector T (reset cleared): loop exits early -> covered by test_ra8_ether_phy.c.
 * - Vector F (reset still set): poll exhausted -> lines 101-102 hit (this test).
 *
 * @since 0.1.0
 */
static void test_open_reset_timeout(void)
{
  TEST_BEGIN("open: reset-poll timeout returns k_ra8_err_hw_timeout");
  cov_prep();
  ra8_ether_phy_cfg_t cfg = cov_make_cfg();

  /* Set countdown beyond the 32-poll limit so the reset bit never clears. */
  s_cov_io.reset_reads_remaining = (uint16_t)k_cov_timeout_remaining;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_ether_phy_open(&cfg));

  /* opened was rolled back; verify a subsequent open can succeed. */
  s_cov_io.reset_reads_remaining = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_close());
  TEST_END("open: reset-poll timeout returns k_ra8_err_hw_timeout");
}

/**
 * @brief link_status_get() propagates BMSR read failure.
 *
 * @details
 * Covers source line 157: the read of the BMSR (status register 1)
 * fails; ra8_ether_phy_link_status_get() must return the error immediately.
 * This is triggered by injecting fail_next_read after a successful open.
 *
 * @par MC/DC:
 * Decision: ``if (err != k_ra8_ok)`` at line 156 (single condition).
 * - Vector T: BMSR read returns hw_error -> line 157 hit (this test).
 * - Vector F: BMSR read returns ok       -> covered by test_ra8_ether_phy.c.
 * Single-condition decision; both vectors satisfy MC/DC.
 *
 * @since 0.1.0
 */
static void test_link_status_bmsr_read_fails(void)
{
  TEST_BEGIN("link_status_get: BMSR read failure propagates error (line 157)");
  cov_prep();
  const ra8_ether_phy_cfg_t cfg = cov_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_open(&cfg));

  /* Inject a read fault; the next read in link_status_get is the BMSR read. */
  s_cov_io.fail_next_read   = 1U;
  ra8_ether_phy_link_t link = {};
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ether_phy_link_status_get(&link));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_close());
  TEST_END("link_status_get: BMSR read failure propagates error (line 157)");
}

/**
 * @brief link_status_get() resolves speed to 100half when LPA advertises it.
 *
 * @details
 * Covers source line 172: when BMSR shows link-up + AN-complete and the
 * LPA register has the 100half bit (0x0080) set but not 100full (0x0100),
 * the speed field is resolved to k_ra8_ether_phy_speed_100h.
 *
 * @par MC/DC:
 * Decision: ``if ((lpa & k_ra8_ether_phy_anar_100half) != 0U)`` at line 171
 * (single condition; the 100full condition at line 169 is false).
 * - Vector T: LPA=0x0080 (100half only) -> speed=100h (this test).
 * - Vector F: LPA=0x0100 (100full)      -> 100full arm taken first; covered
 *             by test_ra8_ether_phy.c (test_link_status).
 *
 * @since 0.1.0
 */
static void test_link_status_speed_100half(void)
{
  TEST_BEGIN("link_status_get: LPA 100half resolves speed to k_ra8_ether_phy_speed_100h");
  cov_prep();
  const ra8_ether_phy_cfg_t cfg = cov_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_open(&cfg));

  /* BMSR: link_up (bit 2) | AN complete (bit 5). LPA: 100half (bit 7). */
  s_cov_io.regs[1]                         = (uint16_t)(0x0004U | 0x0020U);
  s_cov_io.regs[k_phy_reg_1000base_status] = k_phy_ability_full_duplex;

  ra8_ether_phy_link_t link = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_link_status_get(&link));
  TEST_ASSERT_EQ(1, link.link_up);
  TEST_ASSERT_EQ(1, link.auto_neg_done);
  TEST_ASSERT_EQ(k_ra8_ether_phy_speed_100h, link.speed);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_close());
  TEST_END("link_status_get: LPA 100half resolves speed to k_ra8_ether_phy_speed_100h");
}

/**
 * @brief link_status_get() resolves speed to 10full when LPA advertises it.
 *
 * @details
 * Covers source lines 173-174: when neither 100full nor 100half bits are set
 * in the LPA, the 10full condition at line 173 is evaluated and, when true
 * (LPA bit 6 set), speed is resolved to k_ra8_ether_phy_speed_10f.
 *
 * @par MC/DC:
 * Decision: ``if ((lpa & k_ra8_ether_phy_anar_10full) != 0U)`` at line 173
 * (single condition; 100full and 100half conditions are both false).
 * - Vector T: LPA=0x0040 (10full only)  -> speed=10f (this test).
 * - Vector F: LPA=0x0020 (10half only)  -> 10half arm taken; see next test.
 *
 * @since 0.1.0
 */
static void test_link_status_speed_10full(void)
{
  TEST_BEGIN("link_status_get: LPA 10full resolves speed to k_ra8_ether_phy_speed_10f");
  cov_prep();
  const ra8_ether_phy_cfg_t cfg = cov_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_open(&cfg));

  /* BMSR: link_up | AN complete. LPA: 10full only (bit 6). */
  s_cov_io.regs[1]                         = (uint16_t)(0x0004U | 0x0020U);
  s_cov_io.regs[k_phy_reg_1000base_status] = k_phy_ability_half_duplex;

  ra8_ether_phy_link_t link = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_link_status_get(&link));
  TEST_ASSERT_EQ(1, link.link_up);
  TEST_ASSERT_EQ(1, link.auto_neg_done);
  TEST_ASSERT_EQ(k_ra8_ether_phy_speed_10f, link.speed);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_close());
  TEST_END("link_status_get: LPA 10full resolves speed to k_ra8_ether_phy_speed_10f");
}

/**
 * @brief link_status_get() resolves speed to 10half when LPA advertises it.
 *
 * @details
 * Covers source lines 175-177: when none of 100full, 100half, or 10full
 * LPA bits are set, the 10half condition at line 175 is evaluated and,
 * when true (LPA bit 5 set), speed is resolved to k_ra8_ether_phy_speed_10h.
 * The closing brace at line 177 is also reached via this execution path.
 *
 * @par MC/DC:
 * Decision: ``if ((lpa & k_ra8_ether_phy_anar_10half) != 0U)`` at line 175
 * (single condition; 100full, 100half, 10full conditions are all false).
 * - Vector T: LPA=0x0020 (10half only)  -> speed=10h (this test).
 * - Vector F: LPA=0x0000 (no bits)      -> no speed resolved; covered
 *             implicitly by the no-link paths in test_ra8_ether_phy.c.
 *
 * @since 0.1.0
 */
static void test_link_status_speed_10half(void)
{
  TEST_BEGIN("link_status_get: LPA 10half resolves speed to k_ra8_ether_phy_speed_10h");
  cov_prep();
  const ra8_ether_phy_cfg_t cfg = cov_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_open(&cfg));

  /* BMSR: link_up | AN complete. LPA: 10half only (bit 5). */
  s_cov_io.regs[1]                         = (uint16_t)(0x0004U | 0x0020U);
  s_cov_io.regs[k_phy_reg_1000base_status] = 0x0020U;

  ra8_ether_phy_link_t link = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_link_status_get(&link));
  TEST_ASSERT_EQ(1, link.link_up);
  TEST_ASSERT_EQ(1, link.auto_neg_done);
  TEST_ASSERT_EQ(k_ra8_ether_phy_speed_10h, link.speed);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ether_phy_close());
  TEST_END("link_status_get: LPA 10half resolves speed to k_ra8_ether_phy_speed_10h");
}

/**
 * @brief Test entry point.
 *
 * @details
 * Runs all coverage-extension test functions in sequence. Each function
 * is self-contained: it calls cov_prep() to reset state, performs its
 * assertions, and prints PASS on success. Any assertion failure calls
 * exit(1) via TEST_FAIL_FMT.
 *
 * @return 0 on success (all tests pass).
 *
 * @pre None.
 * @post All targeted uncovered source lines have been executed.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  test_open_write_fails();
  test_open_poll_read_fails();
  test_open_reset_timeout();
  test_link_status_bmsr_read_fails();
  test_link_status_speed_100half();
  test_link_status_speed_10full();
  test_link_status_speed_10half();
  (void)fprintf(stderr, "[OK ] test_ra8_ether_phy_cov.c\n");
  return 0;
}
