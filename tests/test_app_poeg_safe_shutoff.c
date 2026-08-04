/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_poeg_safe_shutoff.c
 * @brief Integration test: POEG safe-shutoff demo logic + model semantics
 *
 * @details
 * Mirrors examples/ek_ra8d2/hil_needs_revalidation/poeg_safe_shutoff/main.c. The POEG
 * register sequencing is owned + covered by ra8_poeg's own unit tests
 * (test_ra8_poeg.c); this test pins down:
 *
 *  - The app verdict ``ok = shutoff && reenabled && running`` with full MC/DC.
 *  - The ra8_emulator POEG STATE-flag derivation (board_periph_poeg.c): an active
 *    request flag latches POEGG.ST; no request clears it. This is the exact
 *    semantics the EIL gate relies on, mirrored here as a pure function.
 *  - The driver assert / clear register mechanics the app drives against the
 *    ra8_fake_mmap host MMIO shim (SSF set on trigger, cleared on ack), and that
 *    the GPT descriptor the app arms is accepted.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpt.h"
#include "ra8_mstp.h"
#include "ra8_poeg.h"
#include "ra8_poeg_regs.h"
#include "unity_minimal.h"

/**
 * @enum app_poeg_safe_shutoff_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint32_t {
  k_poeg_poison_out =
    0xFFFFFFFFU, /**< Poison in a count out-param; a call failing to set it is detectable. */
} app_poeg_safe_shutoff_fixture_t;

/** @brief Mirror of the demo's app-local unit selection. */
typedef enum : uint8_t {
  k_t_poeg_group       = 0U,  /**< POEG group the demo drives.  */
  k_t_poeg_gpt_channel = 0U,  /**< GPT channel the demo drives. */
  k_t_poeg_group_bad   = 99U, /**< Out-of-range group (>= 4).   */
} t_poeg_unit_t;

/** @brief Mirror of the demo's GPT descriptor constants. */
typedef enum : uint32_t {
  k_t_poeg_gpt_prd  = 0x0000FFFFU, /**< 16-bit saw-PWM period. */
  k_t_poeg_gpt_duty = 0x00008000U, /**< Mid-scale (50%) duty.  */
} t_poeg_gpt_const_t;

/**
 * @brief Mirror of the demo's healthy-cycle verdict.
 *
 * @details Reproduces the compound decision in ``poeg_demo_cycle``:
 * ``*out_ok = (shutoff && reenabled && running)``.
 */
static uint8_t compute_ok(uint8_t shutoff, uint8_t reenabled, uint8_t running)
{
  return (shutoff != 0U && reenabled != 0U && running != 0U) ? 1U : 0U;
}

/**
 * @brief Mirror of board_periph_poeg.c's ST-derivation (poeg_with_st).
 *
 * @details The output-disable STATE flag (POEGG.ST) is read-only and reflects
 * whether ANY request flag is set. The ra8_emulator model strips any written ST
 * and re-derives it from the request bits; this mirror captures that exact
 * behaviour so the EIL-gate semantics are unit-tested on the host.
 */
static uint32_t derive_st(uint32_t writable)
{
  const uint32_t trig = (uint32_t)k_ra8_poeg_status_pidf | (uint32_t)k_ra8_poeg_status_iocf |
                        (uint32_t)k_ra8_poeg_status_ovrf | (uint32_t)k_ra8_poeg_status_ssf;
  const uint32_t base = writable & ~(uint32_t)k_ra8_poeg_status_st;
  if ((base & trig) != 0U) {
    return base | (uint32_t)k_ra8_poeg_status_st;
  }
  return base;
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

static ra8_gpt_cfg_t make_gpt_cfg(void)
{
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_4,
    .period     = (uint32_t)k_t_poeg_gpt_prd,
    .duty_a     = (uint32_t)k_t_poeg_gpt_duty,
    .duty_b     = (uint32_t)k_t_poeg_gpt_duty,
    .auto_start = true,
  };
  return cfg;
}

static ra8_poeg_cfg_t make_poeg_cfg(void)
{
  const ra8_poeg_cfg_t cfg = {
    .enable_pin      = true,
    .enable_ioc      = false,
    .enable_osc_stop = false,
    .invert_input    = false,
  };
  return cfg;
}

/**
 * @brief Verdict ``shutoff && reenabled && running`` exercised for MC/DC.
 *
 * @par MC/DC:
 * 3 conditions. N+1 = 4 vectors:
 * - shutoff=1, reenabled=1, running=1 -> 1   (control)
 * - shutoff=0, reenabled=1, running=1 -> 0   (varies shutoff)
 * - shutoff=1, reenabled=0, running=1 -> 0   (varies reenabled)
 * - shutoff=1, reenabled=1, running=0 -> 0   (varies running)
 */
static void test_poeg_verdict_mcdc(void)
{
  TEST_BEGIN("poeg_safe_shutoff: verdict MC/DC");
  TEST_ASSERT_EQ(1U, compute_ok(1U, 1U, 1U)); /* control        */
  TEST_ASSERT_EQ(0U, compute_ok(0U, 1U, 1U)); /* vary shutoff   */
  TEST_ASSERT_EQ(0U, compute_ok(1U, 0U, 1U)); /* vary reenabled */
  TEST_ASSERT_EQ(0U, compute_ok(1U, 1U, 0U)); /* vary running   */
  TEST_END("poeg_safe_shutoff: verdict MC/DC");
}

/**
 * @brief The ra8_emulator ST-derivation latches / clears as the STATE flag does.
 *
 * @par MC/DC:
 * Decision ``(base & trig) != 0`` -- one atomic condition x 2 vectors: a
 * request flag present (ST latched) vs absent (ST clear). A written ST with no
 * request is stripped (read-only), which the third assertion confirms.
 */
static void test_poeg_st_derivation(void)
{
  TEST_BEGIN("poeg_safe_shutoff: ST derivation");
  /* SSF request set -> ST latches high (outputs high-impedance). */
  TEST_ASSERT((derive_st((uint32_t)k_ra8_poeg_status_ssf) & (uint32_t)k_ra8_poeg_status_st) != 0U);
  /* No request (only the PIDE enable) -> ST stays clear (outputs driven). */
  TEST_ASSERT((derive_st((uint32_t)k_ra8_poeg_en_pide) & (uint32_t)k_ra8_poeg_status_st) == 0U);
  /* A written ST with no request is read-only: stripped back to 0. */
  TEST_ASSERT((derive_st((uint32_t)k_ra8_poeg_status_st) & (uint32_t)k_ra8_poeg_status_st) == 0U);
  TEST_END("poeg_safe_shutoff: ST derivation");
}

/**
 * @brief Driver assert / clear mechanics against the host MMIO shim.
 *
 * @par MC/DC:
 * No compound decision -- exercises the public-API contract the app drives:
 * ``ra8_poeg_trigger_stop`` sets POEGG.SSF and ``ra8_poeg_clear_status`` of SSF
 * clears it, matching the demo's shutoff / re-enable calls.
 */
static void test_poeg_driver_cycle(void)
{
  TEST_BEGIN("poeg_safe_shutoff: driver assert + clear");
  prep();

  const ra8_poeg_cfg_t cfg = make_poeg_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_init((uint8_t)k_t_poeg_group, &cfg));

  /* SHUTOFF: assert the software request; SSF must read back set. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_trigger_stop((uint8_t)k_t_poeg_group));
  uint32_t asserted = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_get_status((uint8_t)k_t_poeg_group, &asserted));
  TEST_ASSERT((asserted & (uint32_t)k_ra8_poeg_status_ssf) != 0U);

  /* RE-ENABLE: clear the request; SSF must read back clear. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_poeg_clear_status((uint8_t)k_t_poeg_group, (uint32_t)k_ra8_poeg_status_ssf));
  uint32_t cleared = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_get_status((uint8_t)k_t_poeg_group, &cleared));
  TEST_ASSERT((cleared & (uint32_t)k_ra8_poeg_status_ssf) == 0U);
  TEST_END("poeg_safe_shutoff: driver assert + clear");
}

/**
 * @brief Out-of-range group is rejected by the driver the demo would call.
 *
 * @par MC/DC:
 * No compound decision -- confirms the documented rejection path the app never
 * takes (group is a compile-time constant 0) but which guards a mis-config.
 */
static void test_poeg_bad_group(void)
{
  TEST_BEGIN("poeg_safe_shutoff: bad group rejected");
  prep();

  const ra8_poeg_cfg_t cfg = make_poeg_cfg();
  TEST_ASSERT(ra8_poeg_init((uint8_t)k_t_poeg_group_bad, &cfg) != k_ra8_ok);
  TEST_ASSERT(ra8_poeg_trigger_stop((uint8_t)k_t_poeg_group_bad) != k_ra8_ok);
  TEST_END("poeg_safe_shutoff: bad group rejected");
}

/**
 * @brief The GPT descriptor the demo arms is accepted; NULL cfg is rejected.
 *
 * @par MC/DC:
 * Decision in app: ``ra8_gpt_init != ok``. One atomic condition x 2 vectors --
 * golden acceptance here + the NULL-cfg reject.
 */
static void test_poeg_gpt_arm(void)
{
  TEST_BEGIN("poeg_safe_shutoff: GPT arm + reject");
  prep();

  const ra8_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_init((uint8_t)k_t_poeg_gpt_channel, &cfg));
  uint32_t count = k_poeg_poison_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_read((uint8_t)k_t_poeg_gpt_channel, &count));
  TEST_ASSERT(ra8_gpt_init((uint8_t)k_t_poeg_gpt_channel, nullptr) != k_ra8_ok);
  TEST_END("poeg_safe_shutoff: GPT arm + reject");
}

int main(void)
{
  test_poeg_verdict_mcdc();
  test_poeg_st_derivation();
  test_poeg_driver_cycle();
  test_poeg_bad_group();
  test_poeg_gpt_arm();
  return 0;
}
