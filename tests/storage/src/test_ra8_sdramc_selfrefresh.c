/**
 * @file test_ra8_sdramc_selfrefresh.c
 * @brief Unit tests for SDRAMC self-refresh entry and exit.
 *
 * @details
 * Exercises `ra8_sdramc_enter_self_refresh` and
 * `ra8_sdramc_exit_self_refresh` via the fake MMIO window
 * provided by `ra8_fake_mmap`. The mock faithfully reflects register
 * writes, so each test verifies the exact register image left by the
 * driver after a successful transition or the error code returned on
 * a violated precondition.
 *
 * Decision coverage: every single-condition `if` guard in both
 * functions (SDSR busy, already-in-SR, not-in-SR, SRFST busy) is
 * exercised by a dedicated test. No compound decisions exist in the
 * production code under test, so no multi-vector MC/DC tables are
 * required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpio_constants.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sdramc.h"
#include "ra8_sdramc_regs.h"
#include "unity_minimal.h"

/**
 * @enum test_sdramc_sr_bits_t
 * @brief Bit values injected into the mock SDSR / SDSELF registers
 *        to exercise precondition guards.
 */
typedef enum : uint8_t {
  k_test_sdsr_srfst  = 0x10U, /**< SDSR.SRFST (bit 4): SR transition in progress. */
  k_test_sdsr_inist  = 0x08U, /**< SDSR.INIST (bit 3): init sequence in progress. */
  k_test_sdsr_mrsst  = 0x01U, /**< SDSR.MRSST (bit 0): mode-reg-set in progress.  */
  k_test_sdself_sfen = 0x01U, /**< SDSELF.SFEN (bit 0): self-refresh enable.      */
  k_test_sdrfen_rfen = 0x01U, /**< SDRFEN.RFEN (bit 0): auto-refresh enable.      */
} test_sdramc_sr_bits_t;

/**
 * @brief Reset the fake MMIO backing store before each test.
 *
 * @details Zeroes the peripheral RAM image so all register reads
 * return 0 (idle state). No pin-validator reset is needed here
 * because the self-refresh tests do not call `ra8_sdramc_init`.
 *
 * @pre Host MMIO substrate is linked.
 * @pre `ra8_fake_mmap_reset` is safe to call repeatedly.
 * @post All SDRAMC registers read as 0.
 * @post SDSR, SDSELF, and SDRFEN are all 0.
 */
static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

/* =========================================================================
 * Happy-path tests
 * =========================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- exercises the happy
 * path of `ra8_sdramc_enter_self_refresh`; guard branches are each
 * covered by dedicated error-path tests below)
 */
static void test_enter_happy_path(void)
{
  TEST_BEGIN("sdramc enter_self_refresh: happy path sets SDSELF and clears SDRFEN");
  reset_world();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_enter_self_refresh());

  volatile r_sdramc_regs_t* reg = ra8_sdramc();
  TEST_ASSERT_EQ(k_test_sdself_sfen, reg->SDSELF);
  TEST_ASSERT_EQ(0U, reg->SDRFEN);

  TEST_END("sdramc enter_self_refresh: happy path sets SDSELF and clears SDRFEN");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- exercises the full
 * round-trip; entry preconditions covered above, exit preconditions
 * covered by dedicated error-path tests below)
 */
static void test_exit_happy_path(void)
{
  TEST_BEGIN("sdramc exit_self_refresh: happy path clears SDSELF and re-enables SDRFEN");
  reset_world();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_enter_self_refresh());

  /* SDSR.SRFST stays 0 in the mock (hardware does not simulate
   * the transition pulse), so exit precondition 2 passes. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_exit_self_refresh());

  volatile r_sdramc_regs_t* reg = ra8_sdramc();
  TEST_ASSERT_EQ(0U, reg->SDSELF);
  TEST_ASSERT_EQ(k_test_sdrfen_rfen, reg->SDRFEN);

  TEST_END("sdramc exit_self_refresh: happy path clears SDSELF and re-enables SDRFEN");
}

/* =========================================================================
 * Entry precondition error paths
 * =========================================================================
 */

/**
 * @par MC/DC:
 * Decision: `(SDSR & k_ra8_sdsr_all) != 0U` (single condition).
 * - Vector 1 (covered by test_enter_happy_path): SDSR==0 -> false -> k_ra8_ok.
 * - Vector 2 (this test): SDSR.SRFST set   -> true  -> k_ra8_err_invalid_state.
 * N+1 = 2 vectors for N=1 condition.
 */
static void test_enter_sdsr_busy_returns_invalid_state(void)
{
  TEST_BEGIN("sdramc enter_self_refresh: SDSR busy returns invalid_state");
  reset_world();

  volatile r_sdramc_regs_t* reg = ra8_sdramc();
  /* Inject a non-zero SDSR (SRFST set: transition already in progress). */
  reg->SDSR = (uint8_t)k_test_sdsr_srfst;

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdramc_enter_self_refresh());

  /* SDSELF and SDRFEN must not have been touched. */
  TEST_ASSERT_EQ(0U, reg->SDSELF);
  TEST_ASSERT_EQ(0U, reg->SDRFEN);

  TEST_END("sdramc enter_self_refresh: SDSR busy returns invalid_state");
}

/**
 * @par MC/DC:
 * Decision: `(SDSELF & k_ra8_sdself_sfen) != 0U` (single condition).
 * - Vector 1 (covered by test_enter_happy_path): SFEN==0 -> false -> k_ra8_ok.
 * - Vector 2 (this test): SFEN==1 -> true -> k_ra8_err_invalid_state.
 * N+1 = 2 vectors for N=1 condition.
 */
static void test_enter_already_in_self_refresh(void)
{
  TEST_BEGIN("sdramc enter_self_refresh: SFEN already 1 returns invalid_state");
  reset_world();

  volatile r_sdramc_regs_t* reg = ra8_sdramc();
  /* Simulate controller already in self-refresh. */
  reg->SDSELF = (uint8_t)k_test_sdself_sfen;

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdramc_enter_self_refresh());

  /* SDSELF must remain unchanged (no double-assertion). */
  TEST_ASSERT_EQ(k_test_sdself_sfen, reg->SDSELF);

  TEST_END("sdramc enter_self_refresh: SFEN already 1 returns invalid_state");
}

/**
 * @par MC/DC:
 * (no compound decisions -- verifies SDSR.INIST triggers the same guard
 * as SDSR.SRFST; the guard is `SDSR & k_ra8_sdsr_all` so any set bit
 * causes invalid_state; this exercises the INIST variant)
 */
static void test_enter_sdsr_inist_busy(void)
{
  TEST_BEGIN("sdramc enter_self_refresh: SDSR.INIST set returns invalid_state");
  reset_world();

  volatile r_sdramc_regs_t* reg = ra8_sdramc();
  reg->SDSR                     = (uint8_t)k_test_sdsr_inist;

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdramc_enter_self_refresh());

  TEST_END("sdramc enter_self_refresh: SDSR.INIST set returns invalid_state");
}

/* =========================================================================
 * Exit precondition error paths
 * =========================================================================
 */

/**
 * @par MC/DC:
 * Decision: `(SDSELF & k_ra8_sdself_sfen) == 0U` (single condition).
 * - Vector 1 (covered by test_exit_happy_path): SFEN==1 -> false -> k_ra8_ok.
 * - Vector 2 (this test): SFEN==0 -> true  -> k_ra8_err_invalid_state.
 * N+1 = 2 vectors for N=1 condition.
 */
static void test_exit_not_in_self_refresh(void)
{
  TEST_BEGIN("sdramc exit_self_refresh: SFEN==0 returns invalid_state");
  reset_world();

  /* SDSELF.SFEN is 0 after reset -- not in self-refresh. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdramc_exit_self_refresh());

  volatile r_sdramc_regs_t* reg = ra8_sdramc();
  /* SDRFEN must remain untouched. */
  TEST_ASSERT_EQ(0U, reg->SDRFEN);

  TEST_END("sdramc exit_self_refresh: SFEN==0 returns invalid_state");
}

/**
 * @par MC/DC:
 * Decision: `(SDSR & k_ra8_sdsr_srfst) != 0U` (single condition).
 * - Vector 1 (covered by test_exit_happy_path): SRFST==0 -> false -> k_ra8_ok.
 * - Vector 2 (this test): SRFST==1 -> true  -> k_ra8_err_invalid_state.
 * N+1 = 2 vectors for N=1 condition.
 */
static void test_exit_srfst_busy_returns_invalid_state(void)
{
  TEST_BEGIN("sdramc exit_self_refresh: SDSR.SRFST busy returns invalid_state");
  reset_world();

  volatile r_sdramc_regs_t* reg = ra8_sdramc();
  /* Put controller into self-refresh, then inject a stale SRFST. */
  reg->SDSELF = (uint8_t)k_test_sdself_sfen;
  reg->SDSR   = (uint8_t)k_test_sdsr_srfst;

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdramc_exit_self_refresh());

  /* SDRFEN must not have been touched. */
  TEST_ASSERT_EQ(0U, reg->SDRFEN);

  TEST_END("sdramc exit_self_refresh: SDSR.SRFST busy returns invalid_state");
}

/* =========================================================================
 * Register-image verification tests
 * =========================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions -- verifies that SDRFEN is disabled before
 * SDSELF is asserted on entry, preserving the correct sequencing)
 */
static void test_enter_disables_sdrfen_before_setting_sfen(void)
{
  TEST_BEGIN("sdramc enter_self_refresh: SDRFEN cleared, then SDSELF set");
  reset_world();

  /* Pre-load SDRFEN as if auto-refresh is running. */
  volatile r_sdramc_regs_t* reg = ra8_sdramc();
  reg->SDRFEN                   = (uint8_t)k_test_sdrfen_rfen;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_enter_self_refresh());

  /* Both conditions must hold in the final state. */
  TEST_ASSERT_EQ(0U, reg->SDRFEN);
  TEST_ASSERT_EQ(k_test_sdself_sfen, reg->SDSELF);

  TEST_END("sdramc enter_self_refresh: SDRFEN cleared, then SDSELF set");
}

/**
 * @par MC/DC:
 * (no compound decisions -- verifies that SDRFEN is re-enabled after
 * SDSELF is cleared on exit)
 */
static void test_exit_re_enables_sdrfen_after_clearing_sfen(void)
{
  TEST_BEGIN("sdramc exit_self_refresh: SDSELF cleared, then SDRFEN re-enabled");
  reset_world();

  volatile r_sdramc_regs_t* reg = ra8_sdramc();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_enter_self_refresh());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_exit_self_refresh());

  TEST_ASSERT_EQ(0U, reg->SDSELF);
  TEST_ASSERT_EQ(k_test_sdrfen_rfen, reg->SDRFEN);

  TEST_END("sdramc exit_self_refresh: SDSELF cleared, then SDRFEN re-enabled");
}

/* =========================================================================
 * Init error / bounded-poll-timeout paths
 * =========================================================================
 */

/**
 * @par MC/DC:
 * Decision: `(SDSR & mask) == 0` inside the bounded ``internal_sdramc_wait``
 * poll, reached through ``ra8_sdramc_init`` (single condition).
 * - Vector 1 (covered by test_ra8_sdramc.c happy path): SDSR==0 -> the poll
 *   exits with k_ra8_ok on the first read.
 * - Vector 2 (this test): a blocking SDSR bit stays set, so the poll runs to
 *   its static bound and ``ra8_sdramc_init`` forwards k_ra8_err_hw_timeout.
 * N+1 = 2 vectors for N=1 condition.
 */
static void test_init_sdsr_busy_times_out(void)
{
  TEST_BEGIN("ra8_sdramc_init: SDSR stuck busy -> bounded-poll timeout");
  reset_world();
  ra8_pin_validator_reset();

  /* Park a blocking SDSR bit so the first init status-poll never clears.
   * Pin routing still succeeds, so the wait (and its timeout return) run. */
  volatile r_sdramc_regs_t* reg = ra8_sdramc();
  reg->SDSR                     = (uint8_t)k_test_sdsr_inist;

  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sdramc_init());
  TEST_END("ra8_sdramc_init: SDSR stuck busy -> bounded-poll timeout");
}

/**
 * @par MC/DC:
 * Decision: `route_err != k_ra8_ok` inside ``internal_sdramc_route_pins`` and
 * the matching `pin_err != k_ra8_ok` guard in ``ra8_sdramc_init`` (single
 * condition each).
 * - Vector 1 (covered by test_ra8_sdramc.c happy path): every pin routes
 *   cleanly -> both guards take the false branch.
 * - Vector 2 (this test): the first SDRAM bus pin is pre-claimed by a
 *   different owner, so its route returns a conflict that both guards
 *   forward as k_ra8_err_gpio_conflict.
 * N+1 = 2 vectors for N=1 condition.
 */
static void test_init_pin_routing_conflict(void)
{
  TEST_BEGIN("ra8_sdramc_init: pre-claimed bus pin -> routing conflict");
  reset_world();
  ra8_pin_validator_reset();

  /* The first pin ra8_sdramc_init routes is A0 = P10_3. Claim it under a
   * different owner so the driver's route of the same pin conflicts on the
   * very first iteration. */
  const ra8_port_pin_t a0 = RA8_PIN(k_ra8_port_10, k_ra8_pin_3);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pfs_route_peripheral(a0, k_ra8_psel_bus, "test_conflict"));

  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, ra8_sdramc_init());
  TEST_END("ra8_sdramc_init: pre-claimed bus pin -> routing conflict");
}

int main(void)
{
  test_enter_happy_path();
  test_exit_happy_path();
  test_enter_sdsr_busy_returns_invalid_state();
  test_enter_already_in_self_refresh();
  test_enter_sdsr_inist_busy();
  test_exit_not_in_self_refresh();
  test_exit_srfst_busy_returns_invalid_state();
  test_enter_disables_sdrfen_before_setting_sfen();
  test_exit_re_enables_sdrfen_after_clearing_sfen();
  test_init_sdsr_busy_times_out();
  test_init_pin_routing_conflict();
  return 0;
}
