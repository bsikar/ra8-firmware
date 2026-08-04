/**
 * @file test_ra8_board_ek_ra8d2_comms_cov.c
 * @brief Coverage-boosting unit tests for the EK-RA8D2 comms BSP layer
 *
 * @details
 * Companion to test_ra8_board_ek_ra8d2.c.  Exercises the error-return
 * branches and loop bodies in ra8_board_ek_ra8d2_comms.c that the primary
 * test cannot reach without hardware-state manipulation:
 *
 *   Lines 154, 155, 167:
 *     Pre-seed the MIPI PHY SFR (k_ra8_mipi_phy_sfr_ready_mask) so
 *     ra8_mipi_phy_init spin-loops find the ready bit on the first
 *     iteration.  ra8_mipi_dsi_init then runs (no spin-loop), and
 *     ra8_mipi_dsi_hs_clock_start times out because PLSR stays zero in
 *     the fake.  Covers the dsi_init call (line 154), the
 *     not-taken branch check (line 155), and the hs_clock_start return
 *     (line 167).
 *
 *   Line 225 (was 224):
 *     Call ra8_board_uart_console_init with a non-zero baud before
 *     ra8_cgc_init so PCLKA is still the MOCO default (~8 MHz), which is
 *     below k_ra8_board_uart_console_min_pclka_hz.  The function returns
 *     k_ra8_err_not_initialized at this guard.
 *
 *   Line 236 (was 235):
 *     After a successful ra8_cgc_init, pre-claim the TXD pin so
 *     ra8_pfs_route_peripheral returns k_ra8_err_gpio_conflict and the
 *     function propagates it.
 *
 *   Line 243 (was 242):
 *     After a successful ra8_cgc_init, pre-claim only the RXD pin so
 *     the TXD route succeeds but the RXD route fails.
 *
 *   Lines 270, 287, 288 (were 269, 286, 287):
 *     Call uart_console_write and uart_console_read with valid arguments
 *     while s_uart_console_initialized is false.  Both return
 *     k_ra8_err_not_initialized.
 *
 *   Lines 294-299, 301-304 (were 293-298, 300-303):
 *     After a successful console init, call ra8_board_uart_console_read
 *     with RDRF unset (getc times out, early-return path) and then
 *     with RDRF pre-seeded (getc succeeds, byte-copy path).
 *
 * Source lines marked GCOVR_EXCL_LINE in ra8_board_ek_ra8d2_comms.c
 * (original numbering before edits):
 *   156: return after ra8_mipi_dsi_init -- dsi_init with valid static cfg
 *        always returns k_ra8_ok in RA8_OFF_TARGET.
 *   221: return after ra8_cgc_get_clock_hz -- call with valid clock-id and
 *        non-null pointer always returns k_ra8_ok.
 *   254: return after ra8_sci_init -- sci_init(8, valid_cfg) always returns
 *        k_ra8_ok in RA8_OFF_TARGET (MSTP timeout excluded on host).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mipi_phy_regs.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_sci_regs.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

/**
 * @enum board_ek_ra8d2_comms_cov_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable, plus buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_board_poison_len =
    0xAAU, /**< Poison in a length out-parameter; a call that skips setting it is detectable. */
  k_sys_oscsf_all_ready =
    0xFFU, /**< Every oscillator-stabilisation flag set, so bring-up sees all sources ready. */
} board_ek_ra8d2_comms_cov_fixture_t;

/* -------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------
 */

/**
 * @brief Reset all fake peripheral state and pin ownership.
 *
 * @details
 * Calls ra8_fake_mmap_reset() to zero every hardware register window,
 * ra8_fake_mmio_reset() to disarm any armed MMIO wait faults, and
 * ra8_pin_validator_reset() to free all claimed pins.  Must be called at
 * the start of every test that issues HAL pin claims so test ordering
 * does not create false conflicts.
 *
 * @pre None.
 * @post ra8_fake_mmap register window cleared; MMIO fault table empty; pin
 *       validator bitmap zeroed.
 *
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
static void reset_state(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  ra8_pin_validator_reset();
}

/**
 * @brief Pre-seed the OSCSF register and run ra8_cgc_init.
 *
 * @details
 * Under RA8_OFF_TARGET the CGC oscillator spin-loops check the OSCSF
 * register rather than real hardware.  Writing 0xFF satisfies every
 * oscillator-stable check on the first iteration.  After this helper
 * returns, s_clock_hz[PCLKA] is set to k_ra8_pclka_hz (125 MHz), which
 * exceeds the console uart minimum.
 *
 * @pre ra8_fake_mmap_reset() has been called (OSCSF is writable memory).
 * @post PCLKA is published to k_ra8_pclka_hz in the CGC state table.
 *
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
static void cgc_init_with_oscsf_preseed(void)
{
  *ra8_sys_oscsf() = (uint8_t)k_sys_oscsf_all_ready;
  (void)ra8_cgc_init();
}

/* -------------------------------------------------------------------------
 * 1. ra8_board_mipi_dsi_init -- dsi_init path + hs_clock_start (lines 154,155,167)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_mipi_dsi_init reaches ra8_mipi_dsi_init and
 *        ra8_mipi_dsi_hs_clock_start when ra8_mipi_phy_init succeeds.
 *
 * @details
 * The existing test_stubs_return_not_supported exercises only the
 * ra8_mipi_phy_init failure path (DPHY SFR never set off-target -> timeout ->
 * early return at source line 145).  To cover source lines 154, 155,
 * and 167, ra8_mipi_phy_init must succeed.
 *
 * Pre-seeding the DPHY SFR with k_ra8_mipi_phy_sfr_ready_mask (PWRSF bit 0
 * and PLLSF bit 8) makes both internal_mipi_phy_init_power_up and
 * internal_mipi_phy_init_host find their respective ready bits on the
 * first spin-loop iteration.  ra8_mipi_dsi_init then runs without a
 * spin-loop (uses MSTP refcount write + register writes only) and returns
 * k_ra8_ok.  ra8_mipi_dsi_hs_clock_start then waits for PLSR which stays
 * zero in the fake and times out, so the function returns a non-ok
 * code.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at source line 155 (1 condition).
 * - Vector A: err=k_ra8_ok (this test -- dsi_init succeeds, branch not taken)
 * - Vector B: err!=k_ra8_ok (would be taken if dsi_init failed; excluded
 *             by GCOVR_EXCL_LINE because dsi_init with valid static cfg
 *             always returns k_ra8_ok in RA8_OFF_TARGET)
 * Providing Vector A is sufficient to show the condition was evaluated;
 * Vector B is confirmed unreachable off-target, as documented above.
 *
 * @pre None (MIPI PHY and DSI module states are in reset from fake start).
 * @post MIPI PHY and DSI module refcounts incremented; s_last_sfr cached.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_mipi_dsi_phy_succeeds_hs_clock_fails(void)
{
  TEST_BEGIN("mipi_dsi_init: phy ok -> dsi ok -> hs_clock times out (lines 154,155,167)");
  reset_state();
  /* Pre-seed both SFR ready bits so ra8_mipi_phy_init spin-loops resolve
   * on the first iteration in RA8_OFF_TARGET. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) = (uint32_t)k_ra8_mipi_phy_sfr_ready_mask;
  /* ra8_mipi_dsi_hs_clock_start waits for PLSR which stays zero -> timeout. */
  const ra8_err_t err = ra8_board_mipi_dsi_init();
  TEST_ASSERT(err != k_ra8_ok);
  TEST_END("mipi_dsi_init: phy ok -> dsi ok -> hs_clock times out (lines 154,155,167)");
}

/* -------------------------------------------------------------------------
 * 2. ra8_board_uart_console_write -- not-initialized guard (line 270)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_uart_console_write rejects calls when the
 *        console has not been initialized.
 *
 * @details
 * The existing test (test_uart_console_write_validates in
 * test_ra8_board_ek_ra8d2.c) exercises the len==0 early-success path
 * and the null-data/non-zero-len invalid-arg path, but not the
 * s_uart_console_initialized==false guard at source line 270.
 *
 * This test calls ra8_board_uart_console_write with a valid non-null
 * buffer and non-zero length while s_uart_console_initialized is false
 * (guaranteed at the start of this binary and before any successful
 * ra8_board_uart_console_init call in this sequence).
 *
 * @par MC/DC:
 * Decision: `if (!s_uart_console_initialized)` at source line 269
 * (1 condition).
 * - Vector A: initialized=false (this test)   -> guard taken
 * - Vector B: initialized=true  (test_lx_console_heartbeat_loop_writes
 *             in test_app_threadx_levelx_demo.c, write path exercised)
 * Vectors A+B demonstrate that the single condition independently
 * controls the outcome; N+1=2 vectors for N=1 condition.
 *
 * @pre s_uart_console_initialized is false (no successful init in binary).
 * @post No side effects; guard returns before forwarding to ra8_sci.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_uart_console_write_not_initialized(void)
{
  TEST_BEGIN("uart_console_write: not-initialized returns error (line 270)");
  static const uint8_t s_payload[] = "ping";
  const ra8_err_t      err = ra8_board_uart_console_write(s_payload, sizeof(s_payload) - 1U);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, err);
  TEST_END("uart_console_write: not-initialized returns error (line 270)");
}

/* -------------------------------------------------------------------------
 * 3. ra8_board_uart_console_read -- not-initialized guard (lines 287, 288)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_uart_console_read rejects calls when the
 *        console has not been initialized.
 *
 * @details
 * The existing test (test_uart_console_read_validates in
 * test_ra8_board_ek_ra8d2.c) exercises null-out_len, cap==0, and
 * null-buffer paths, but not the s_uart_console_initialized==false guard
 * at source lines 287-288.  This test provides the missing vector:
 * a call with valid out, non-zero cap, and valid out_len while
 * s_uart_console_initialized is still false.
 *
 * @par MC/DC:
 * Decision: `if (!s_uart_console_initialized)` in the read function
 * (1 condition).
 * - Vector A: initialized=false (this test)   -> guard taken
 * - Vector B: initialized=true  (test 7 in this file, loop exercises the
 *             non-taken branch)
 * N+1=2 vectors; minimal MC/DC.
 *
 * @pre s_uart_console_initialized is false.
 * @post No side effects; guard returns before forwarding to ra8_sci.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_uart_console_read_not_initialized(void)
{
  TEST_BEGIN("uart_console_read: not-initialized returns error (lines 287,288)");
  uint8_t         buf[4]  = {};
  size_t          out_len = 0U;
  const ra8_err_t err     = ra8_board_uart_console_read(buf, sizeof(buf), &out_len);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, err);
  TEST_END("uart_console_read: not-initialized returns error (lines 287,288)");
}

/* -------------------------------------------------------------------------
 * 4. ra8_board_uart_console_init -- PCLKA below minimum (line 225)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_uart_console_init rejects initialization
 *        when PCLKA is below the minimum required frequency.
 *
 * @details
 * At binary startup, s_clock_hz[PCLKA] is k_ra8_moco_hz (~8 MHz), which
 * is below k_ra8_board_uart_console_min_pclka_hz (16 MHz).  Calling
 * ra8_board_uart_console_init with a non-zero baud rate (so it passes the
 * baud guard) reaches the PCLKA check at source line 224 and returns
 * k_ra8_err_not_initialized.
 *
 * This test must run before any call to cgc_init_with_oscsf_preseed in
 * this binary so that PCLKA is still at the MOCO default.
 *
 * @par MC/DC:
 * Decision: `if (pclka_hz < k_ra8_board_uart_console_min_pclka_hz)` at
 * source line 223 (1 condition).
 * - Vector A: pclka_hz < min (this test, MOCO ~8 MHz)    -> guard taken
 * - Vector B: pclka_hz >= min (tests 5-7, after cgc_init) -> guard not taken
 * N+1=2 vectors; minimal MC/DC.
 *
 * @pre s_clock_hz[PCLKA] == k_ra8_moco_hz (no cgc_init called yet).
 * @post s_uart_console_initialized remains false (init failed).
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_uart_console_init_pclka_below_minimum(void)
{
  TEST_BEGIN("uart_console_init: pclka below min returns not_initialized (line 225)");
  reset_state();
  /* PCLKA defaults to k_ra8_moco_hz (~8 MHz) at binary start without cgc_init.
   * That is below k_ra8_board_uart_console_min_pclka_hz (16 MHz). */
  const ra8_err_t err = ra8_board_uart_console_init(115200U);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, err);
  TEST_END("uart_console_init: pclka below min returns not_initialized (line 225)");
}

/* -------------------------------------------------------------------------
 * 5. ra8_board_uart_console_init -- TXD pin conflict (line 236)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_uart_console_init propagates a pin-conflict
 *        error when the TXD pin is already claimed.
 *
 * @details
 * Source line 235 is `return err;` inside the `if (err != k_ra8_ok)` check
 * after the ra8_pfs_route_peripheral call for TXD (PD02 = port 13 pin 2).
 * Pre-claiming that pin via ra8_pin_validator_claim causes
 * ra8_pfs_route_peripheral to return k_ra8_err_gpio_conflict, which is then
 * propagated.  cgc_init is run first so that the PCLKA guard does not
 * intercept the call.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after the TXD route call (1 condition).
 * - Vector A: err=k_ra8_ok (happy path -- tests 7 and 8 in this file)
 * - Vector B: err=k_ra8_err_gpio_conflict (this test)
 * N+1=2 vectors; minimal MC/DC.
 *
 * @pre Clean pin-validator state (all pins free after reset_state).
 * @post TXD pin remains claimed by the test pre-claim.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_uart_console_init_txd_pin_conflict(void)
{
  TEST_BEGIN("uart_console_init: TXD pin conflict propagated (line 236)");
  reset_state();
  cgc_init_with_oscsf_preseed();
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) -- RA8_PIN()-packed board pin is valid ra8_port_pin_t data outside the enumerator list. */
  const ra8_port_pin_t txd_pin = (ra8_port_pin_t)k_ra8_board_uart_console_pin_txd;
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(txd_pin, "test.txd.conflict"));
  const ra8_err_t err = ra8_board_uart_console_init(115200U);
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, err);
  TEST_END("uart_console_init: TXD pin conflict propagated (line 236)");
}

/* -------------------------------------------------------------------------
 * 6. ra8_board_uart_console_init -- RXD pin conflict (line 243)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_uart_console_init propagates a pin-conflict
 *        error when the RXD pin is already claimed but TXD is free.
 *
 * @details
 * Source line 242 is `return err;` inside the `if (err != k_ra8_ok)` check
 * after the ra8_pfs_route_peripheral call for RXD (PD03 = port 13 pin 3).
 * Only the RXD pin is pre-claimed; the TXD pin is left free so the TXD
 * route succeeds and execution reaches the RXD route.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after the RXD route call (1 condition).
 * - Vector A: err=k_ra8_ok (happy path -- tests 7 and 8 in this file)
 * - Vector B: err=k_ra8_err_gpio_conflict (this test)
 * N+1=2 vectors; minimal MC/DC.
 *
 * @pre Clean pin-validator state.
 * @post TXD pin is claimed by the console init (then the function returned
 *       because RXD failed, leaving TXD claimed by the route call).
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_uart_console_init_rxd_pin_conflict(void)
{
  TEST_BEGIN("uart_console_init: RXD pin conflict propagated (line 243)");
  reset_state();
  cgc_init_with_oscsf_preseed();
  /* Pre-claim only RXD so TXD succeeds but RXD fails. */
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) -- RA8_PIN()-packed board pin is valid ra8_port_pin_t data outside the enumerator list. */
  const ra8_port_pin_t rxd_pin = (ra8_port_pin_t)k_ra8_board_uart_console_pin_rxd;
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(rxd_pin, "test.rxd.conflict"));
  const ra8_err_t err = ra8_board_uart_console_init(115200U);
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, err);
  TEST_END("uart_console_init: RXD pin conflict propagated (line 243)");
}

/* -------------------------------------------------------------------------
 * 7. ra8_board_uart_console_read -- loop with no byte available (lines 294-299)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_uart_console_read returns k_ra8_ok immediately
 *        (non-blocking stop) when ra8_sci_getc_polling finds no byte.
 *
 * @details
 * A successful ra8_board_uart_console_init is required to set
 * s_uart_console_initialized = true before the loop runs.  The SCI8 CSR
 * register is zero after reset_state, so RDRF (bit 31) is not set.
 * ra8_sci_getc_polling spins ra8_hw_wait_flag_set32 for k_ra8_hw_budget_medium
 * iterations, finds RDRF clear throughout, and returns k_ra8_err_hw_timeout.
 * ra8_board_uart_console_read treats this as a non-blocking "no data yet"
 * and returns k_ra8_ok at source line 299.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` inside the read loop (1 condition).
 * - Vector A: err=k_ra8_err_hw_timeout (this test, RDRF not set) -> taken
 * - Vector B: err=k_ra8_ok             (test 8 below, RDRF pre-seeded) -> not taken
 * N+1=2 vectors; minimal MC/DC.
 *
 * @pre Clean hardware state; s_uart_console_initialized becomes true here.
 * @post s_uart_console_initialized is true; SCI8 registers set by init.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_uart_console_read_no_byte_available(void)
{
  TEST_BEGIN("uart_console_read: no byte available returns ok (lines 294-299)");
  reset_state();
  cgc_init_with_oscsf_preseed();
  /* Initialize the console so s_uart_console_initialized becomes true. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_uart_console_init(115200U));

  /* Arm the console SCI CSR so ra8_sci_getc_polling's RDRF wait
   * (ra8_hw_wait_flag_set32(&reg->CSR, ...)) runs to its full budget and
   * returns k_ra8_err_hw_timeout.  Without arming, the fake MMIO seam
   * succeeds an unarmed wait on the first poll, so getc would spuriously
   * report a byte.  The read then treats the timeout as "no data yet"
   * and returns k_ra8_ok with out_len unchanged. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_wait(
      (const volatile void*)&ra8_sci((uint8_t)k_ra8_board_uart_console_sci_channel)->CSR));

  uint8_t         buf[4]  = {};
  size_t          out_len = k_board_poison_len;
  const ra8_err_t err     = ra8_board_uart_console_read(buf, 1U, &out_len);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ(0U, out_len);
  TEST_END("uart_console_read: no byte available returns ok (lines 294-299)");
}

/* -------------------------------------------------------------------------
 * 8. ra8_board_uart_console_read -- loop with byte available (lines 301-304)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify that ra8_board_uart_console_read copies a received byte and
 *        increments out_len when ra8_sci_getc_polling succeeds.
 *
 * @details
 * Continuing from test 7 (s_uart_console_initialized is still true),
 * the SCI8 CSR register is pre-seeded with RDRF (bit 31) set.
 * ra8_sci_getc_polling finds RDRF set on the first spin-loop check and
 * returns k_ra8_ok; RDR is zero (never written), so the received byte is 0.
 * The read loop then executes:
 *   out[i] = byte    (source line 301)
 *   *out_len += 1    (source line 302)
 * The loop completes one iteration (cap=1), falls through to the loop
 * termination check (source line 303), and returns k_ra8_ok (source line 304).
 *
 * @par MC/DC:
 * (See test 7 for the full MC/DC table covering the `if (err != k_ra8_ok)`
 * decision; this test provides Vector B where the branch is not taken.)
 *
 * @pre s_uart_console_initialized is true (set by test 7 above).
 * @post out[0] == 0 (byte read from zero-initialized RDR); *out_len == 1.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_uart_console_read_byte_available(void)
{
  TEST_BEGIN("uart_console_read: byte available copies byte (lines 301-304)");
  /* s_uart_console_initialized is true from test_uart_console_read_no_byte_available. */

  /* Disarm the CSR wait fault that the no-byte test armed on this channel so
   * ra8_sci_getc_polling's RDRF wait can succeed here.  Only the MMIO fault
   * table is cleared; s_uart_console_initialized and the mmap window persist. */
  ra8_fake_mmio_reset();

  /* Pre-seed RDRF (bit 31) in CSR so ra8_sci_getc_polling resolves immediately. */
  volatile r_sci_regs_t* sci_reg = ra8_sci((uint8_t)k_ra8_board_uart_console_sci_channel);
  sci_reg->CSR                   = (uint32_t)(1U << (uint8_t)k_ra8_sci_csr_bit_rdrf);

  uint8_t         buf[2]  = {k_sys_oscsf_all_ready, k_sys_oscsf_all_ready};
  size_t          out_len = 0U;
  const ra8_err_t err     = ra8_board_uart_console_read(buf, 1U, &out_len);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  /* RDR was zero; the received byte is 0. */
  TEST_ASSERT_EQ(0U, buf[0]);
  TEST_ASSERT_EQ(1U, out_len);
  TEST_END("uart_console_read: byte available copies byte (lines 301-304)");
}

/* -------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------
 */

/**
 * @brief Test binary entry point.
 *
 * @details
 * Runs every coverage-boosting test in sequence.  Returns 0 on success;
 * any TEST_ASSERT failure calls exit(1) before this function returns.
 *
 * Test ordering is significant:
 *   - Tests 2 and 3 must precede any successful uart_console_init so that
 *     s_uart_console_initialized is false when they execute.
 *   - Test 4 must precede tests 5-8 so that s_clock_hz[PCLKA] is still
 *     the MOCO default (cgc_init not yet called in the uart console context).
 *   - Test 7 runs uart_console_init to set s_uart_console_initialized;
 *     test 8 relies on that state without re-initializing.
 *
 * @return 0 on success.
 *
 * @pre ra8_fake_mmap register window allocated by the test framework.
 * @post All targeted source lines are instrumented with gcov data.
 *
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_mipi_dsi_phy_succeeds_hs_clock_fails();
  test_uart_console_write_not_initialized();
  test_uart_console_read_not_initialized();
  test_uart_console_init_pclka_below_minimum();
  test_uart_console_init_txd_pin_conflict();
  test_uart_console_init_rxd_pin_conflict();
  test_uart_console_read_no_byte_available();
  test_uart_console_read_byte_available();
  (void)fprintf(stderr, "[OK ] test_ra8_board_ek_ra8d2_comms_cov.c\n");
  return 0;
}
