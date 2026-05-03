/**
 * @file test_ra_ble_l2cap.c
 * @brief Standalone MC/DC unit tests for libs/ra_ble_host/src/ra_ble_l2cap.c
 *
 * @details
 * Provides labeled MC/DC vector sets for the public-API decisions in
 * ``ra_ble_l2cap.c`` flagged by docs/MCDC_GAPS.csv, including the HCI
 * event-trampoline guards (lines 576/580/597) reached via the
 * UNIT_TEST-only ``ra_ble_host_test_inject_event`` veneer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_ble_host.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_l2cap_role_bogus  = 99U,
  k_test_evt_disconn       = 0x05U,
  k_test_evt_le_meta       = 0x3EU,
  k_test_evt_unrecognised  = 0x12U,
  k_test_subev_conn_compl  = 0x01U,
  k_test_subev_other       = 0x0AU,
  k_test_min_disconn_bytes = 4U,
  k_test_min_lemeta_bytes  = 19U,
  k_test_status_success    = 0x00U,
  k_test_handle_lo         = 0x42U,
  k_test_handle_hi         = 0x00U,
} test_l2cap_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_ble_host_close();
}

static ra_err_t bring_up(void)
{
  prep();
  ra_ble_host_config_t cfg = {.role = k_ra_ble_host_role_peripheral, .appearance = 0U, .name = "t"};
  return ra_ble_host_init(&cfg);
}

/**
 * @test test_mcdc_l2cap_init_role_4cond
 *
 * @par MC/DC:
 * Decision: ``if ((cfg->role != k_ra_ble_host_role_peripheral) &&
 *                 (cfg->role != k_ra_ble_host_role_central) &&
 *                 (cfg->role != k_ra_ble_host_role_observer) &&
 *                 (cfg->role != k_ra_ble_host_role_broadcaster))``
 * (4 conditions, libs/ra_ble_host/src/ra_ble_l2cap.c around line 662 --
 * ra_ble_host_init)
 *
 * Per DO-178C 6.4.4.3 representative-subset for chained ``&&`` decision
 * is N+1 = 5 vectors. Pairs (V1,V5)..(V4,V5) flip C1..C4 in turn.
 */
static void test_mcdc_l2cap_init_role_4cond(void)
{
  TEST_BEGIN("ra_ble_host_init MC/DC: role 4-cond chain");
  ra_ble_host_config_t cfg = {.appearance = 0U, .name = "t"};
  prep();
  cfg.role = k_ra_ble_host_role_peripheral;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ble_host_init(&cfg));
  prep();
  cfg.role = k_ra_ble_host_role_central;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ble_host_init(&cfg));
  prep();
  cfg.role = k_ra_ble_host_role_observer;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ble_host_init(&cfg));
  prep();
  cfg.role = k_ra_ble_host_role_broadcaster;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ble_host_init(&cfg));
  prep();
  cfg.role = (ra_ble_host_role_t)k_test_l2cap_role_bogus;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_ble_host_init(&cfg));
  TEST_END("ra_ble_host_init MC/DC: role 4-cond chain");
}

/**
 * @test test_mcdc_l2cap_evt_trampoline_initguard
 *
 * @par MC/DC:
 * Decision (libs/ra_ble_host/src/ra_ble_l2cap.c line 576):
 *   ``if ((params == NULL) || (s_state.initialized == 0U))``
 * (2 conditions, OR; N+1 = 3 vectors).
 *
 * - V1: host uninitialised, params valid -> C1=F C2=T -> dec T (return).
 * - V2: host initialised,   params NULL  -> C1=T short -> dec T (return).
 * - V3: host initialised,   params valid -> C1=F C2=F -> dec F (proceed).
 * V1+V3 flip C2; V2+V3 flip C1. Minimal MC/DC. Driven through the
 * UNIT_TEST veneer ra_ble_host_test_inject_event so the production
 * decision is exercised on the real source.
 */
static void test_mcdc_l2cap_evt_trampoline_initguard(void)
{
  TEST_BEGIN("ra_ble_l2cap MC/DC: evt-trampoline init/null guard (576)");
  static const uint8_t s_dummy_params[1] = {0U};

  /* V1: not initialised, params!=NULL -> C2=T (early return; no event). */
  prep();
  const uint32_t before_v1 = ra_ble_host_test_event_count();
  ra_ble_host_test_inject_event((uint8_t)k_test_evt_unrecognised, s_dummy_params, 1U);
  TEST_ASSERT_EQ((int32_t)before_v1, (int32_t)ra_ble_host_test_event_count());

  /* V2: initialised, params==NULL -> C1=T (early return). */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)bring_up());
  const uint32_t before_v2 = ra_ble_host_test_event_count();
  ra_ble_host_test_inject_event((uint8_t)k_test_evt_unrecognised, NULL, 0U);
  TEST_ASSERT_EQ((int32_t)before_v2, (int32_t)ra_ble_host_test_event_count());

  /* V3: initialised, params!=NULL but unrecognised event -> C1=F C2=F
   * (falls through to no-op tail; event count unchanged). */
  const uint32_t before_v3 = ra_ble_host_test_event_count();
  ra_ble_host_test_inject_event((uint8_t)k_test_evt_unrecognised, s_dummy_params, 1U);
  TEST_ASSERT_EQ((int32_t)before_v3, (int32_t)ra_ble_host_test_event_count());

  TEST_END("ra_ble_l2cap MC/DC: evt-trampoline init/null guard (576)");
}

/**
 * @test test_mcdc_l2cap_evt_trampoline_lemeta_3cond
 *
 * @par MC/DC:
 * Decision (libs/ra_ble_host/src/ra_ble_l2cap.c line 580):
 *   ``if ((evt_code == k_evt_le_meta) &&
 *         (params_len >= k_min_lemeta_param_bytes) &&
 *         (params[k_lemeta_subev_idx] == k_subev_le_conn_complete))``
 * (3 conditions, AND; N+1 = 4 vectors).
 *
 * - V1: evt!=LE_META, len>=19, sub==CONN -> C1=F. Decision F.
 * - V2: evt==LE_META, len<19,  sub==CONN -> C1=T C2=F. Decision F.
 * - V3: evt==LE_META, len>=19, sub!=CONN -> C1=T C2=T C3=F. Decision F.
 * - V4: evt==LE_META, len>=19, sub==CONN -> all T. Decision T -> dispatched.
 * Pairs (V1,V4),(V2,V4),(V3,V4) flip C1,C2,C3 independently.
 */
static void test_mcdc_l2cap_evt_trampoline_lemeta_3cond(void)
{
  TEST_BEGIN("ra_ble_l2cap MC/DC: LE_Meta 3-cond AND (580)");
  /* Build a 19-byte LE_Connection_Complete parameter array.
   * params[0]=subev, [1]=status, [2..3]=handle_le, [4..18]=role/peer/etc. */
  static uint8_t s_params_full[19] = {0U};
  s_params_full[0]                 = (uint8_t)k_test_subev_conn_compl;
  s_params_full[1]                 = (uint8_t)k_test_status_success;
  s_params_full[2]                 = (uint8_t)k_test_handle_lo;
  s_params_full[3]                 = (uint8_t)k_test_handle_hi;

  /* V1: wrong evt_code, but params still well-formed. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)bring_up());
  uint32_t before = ra_ble_host_test_event_count();
  ra_ble_host_test_inject_event((uint8_t)k_test_evt_unrecognised, s_params_full, 19U);
  TEST_ASSERT_EQ((int32_t)before, (int32_t)ra_ble_host_test_event_count());

  /* V2: right evt_code, short params (still non-NULL so guard passes). */
  static const uint8_t s_short_params[2] = {0U, 0U};
  before                                 = ra_ble_host_test_event_count();
  ra_ble_host_test_inject_event((uint8_t)k_test_evt_le_meta, s_short_params, 2U);
  TEST_ASSERT_EQ((int32_t)before, (int32_t)ra_ble_host_test_event_count());

  /* V3: right evt_code + len, but wrong subev. */
  static uint8_t s_other_sub[19] = {0U};
  s_other_sub[0]                 = (uint8_t)k_test_subev_other;
  before                         = ra_ble_host_test_event_count();
  ra_ble_host_test_inject_event((uint8_t)k_test_evt_le_meta, s_other_sub, 19U);
  TEST_ASSERT_EQ((int32_t)before, (int32_t)ra_ble_host_test_event_count());

  /* V4: all three conditions true -> a connected event is dispatched. */
  before = ra_ble_host_test_event_count();
  ra_ble_host_test_inject_event((uint8_t)k_test_evt_le_meta, s_params_full, 19U);
  TEST_ASSERT(ra_ble_host_test_event_count() == before + 1U);

  TEST_END("ra_ble_l2cap MC/DC: LE_Meta 3-cond AND (580)");
}

/**
 * @test test_mcdc_l2cap_evt_trampoline_disconn_2cond
 *
 * @par MC/DC:
 * Decision (libs/ra_ble_host/src/ra_ble_l2cap.c line 597):
 *   ``else if ((evt_code == k_evt_disconn_complete) &&
 *              (params_len >= k_min_disconn_param_bytes))``
 * (2 conditions, AND; N+1 = 3 vectors).
 *
 * - V1: evt!=DISCONN, len>=4 -> C1=F. Decision F.
 * - V2: evt==DISCONN, len<4  -> C1=T C2=F. Decision F.
 * - V3: evt==DISCONN, len>=4 -> all T. Decision T (matches active conn,
 *       dispatches a disconnected event).
 * V1+V3 flip C1; V2+V3 flip C2. Minimal MC/DC.
 *
 * The branch also hangs off the LE_Meta else-if, so V1 keeps the
 * LE_Meta cond F by setting evt_code to a value that is neither LE_Meta
 * nor DISCONN.
 */
static void test_mcdc_l2cap_evt_trampoline_disconn_2cond(void)
{
  TEST_BEGIN("ra_ble_l2cap MC/DC: Disconn 2-cond AND (597)");

  /* Bring the host up and force a known conn_handle so V3 actually
   * matches s_state.conn_handle and emits the disconnected event. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)bring_up());
  const uint16_t conn_local = 0x0042U;
  ra_ble_host_test_inject_connect(conn_local);

  /* V1: wrong evt_code, valid len. No new dispatch beyond the
   * inject_connect we just did. */
  static const uint8_t s_dummy_params4[4] = {0U, 0U, 0U, 0U};
  uint32_t             before             = ra_ble_host_test_event_count();
  ra_ble_host_test_inject_event((uint8_t)k_test_evt_unrecognised, s_dummy_params4, 4U);
  TEST_ASSERT_EQ((int32_t)before, (int32_t)ra_ble_host_test_event_count());

  /* V2: right evt_code, but len < 4. */
  static const uint8_t s_short[1] = {0U};
  before                          = ra_ble_host_test_event_count();
  ra_ble_host_test_inject_event((uint8_t)k_test_evt_disconn, s_short, 1U);
  TEST_ASSERT_EQ((int32_t)before, (int32_t)ra_ble_host_test_event_count());

  /* V3: matching disconnect for the active connection handle -> event. */
  static uint8_t s_disconn_params[4] = {0U};
  s_disconn_params[1]                = (uint8_t)(conn_local & 0xFFU);
  s_disconn_params[2]                = (uint8_t)((conn_local >> 8U) & 0xFFU);
  before                             = ra_ble_host_test_event_count();
  ra_ble_host_test_inject_event((uint8_t)k_test_evt_disconn, s_disconn_params, 4U);
  TEST_ASSERT(ra_ble_host_test_event_count() == before + 1U);

  TEST_END("ra_ble_l2cap MC/DC: Disconn 2-cond AND (597)");
}

int32_t main(void)
{
  test_mcdc_l2cap_init_role_4cond();
  test_mcdc_l2cap_evt_trampoline_initguard();
  test_mcdc_l2cap_evt_trampoline_lemeta_3cond();
  test_mcdc_l2cap_evt_trampoline_disconn_2cond();
  (void)fprintf(stderr, "[OK ] test_ra_ble_l2cap.c\n");
  return 0;
}
