/**
 * @file test_ra_ble_mesh.c
 * @brief Unit tests for libs/ra_ble_host mesh wrapper.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_ble_mesh.h"
#include "ra_err.h"
#include "unity_minimal.h"

extern void    ra_ble_mesh_test_emit_event(const ra_ble_mesh_event_t* evt);
extern uint8_t ra_ble_mesh_test_prov_active(void);

static uint32_t s_evt_count;

static void evt_cb(void* ctx, const ra_ble_mesh_event_t* evt)
{
  (void)ctx;
  (void)evt;
  s_evt_count++;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null(void)
{
  TEST_BEGIN("test_init_null");
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_ble_mesh_init(nullptr));
  TEST_END("test_init_null");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_invalid_count(void)
{
  TEST_BEGIN("test_init_invalid_count");
  ra_ble_mesh_config_t cfg = {};
  cfg.element_count        = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_mesh_init(&cfg));
  cfg.element_count = (uint8_t)(k_ra_ble_mesh_max_elements + 1U);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_mesh_init(&cfg));
  TEST_END("test_init_invalid_count");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_lifecycle(void)
{
  TEST_BEGIN("test_lifecycle");
  ra_ble_mesh_config_t cfg     = {};
  cfg.element_count            = 1U;
  cfg.elements[0].model_count  = 1U;
  cfg.elements[0].models[0].id = 0x0000U; /* Configuration Server. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_init(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_attach_event_handler(evt_cb, nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_prov_enable());
  TEST_ASSERT_EQ(1U, ra_ble_mesh_test_prov_active());
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_prov_disable());
  TEST_ASSERT_EQ(0U, ra_ble_mesh_test_prov_active());
  s_evt_count             = 0U;
  ra_ble_mesh_event_t evt = {.kind = k_ra_ble_mesh_evt_provisioned};
  ra_ble_mesh_test_emit_event(&evt);
  TEST_ASSERT_EQ(1U, s_evt_count);
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_factory_reset());
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_close());
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_ble_mesh_close());
  TEST_END("test_lifecycle");
}

/* --------------------------------------------------------------------- */
/* MC/DC vector tests for libs/ra_ble_host/src/ra_ble_mesh.c             */
/* --------------------------------------------------------------------- */

/**
 * @test test_mcdc_mesh_validate_element_count
 *
 * @par MC/DC:
 * Decision: `(cfg->element_count == 0U) || (cfg->element_count > MAX)`
 * (2 conditions, libs/ra_ble_host/src/ra_ble_mesh.c line 108)
 * Reached through ra_ble_mesh_init() -> internal_validate().
 * - V1 element_count=1 -> C1=F, C2=F. Decision F (ok).
 * - V2 element_count=0 -> C1=T short-circuits. Decision T -> invalid_arg.
 * - V3 element_count=MAX+1 -> C1=F, C2=T. Decision T -> invalid_arg.
 * V1+V2 vary C1 with C2 held F. V1+V3 vary C2 with C1 held F. N+1=3.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 */
static void test_mcdc_mesh_validate_element_count(void)
{
  TEST_BEGIN("mcdc mesh validate (element_count==0 || >MAX)");
  (void)ra_ble_mesh_close();
  ra_ble_mesh_config_t cfg    = {};
  cfg.element_count           = 1U;
  cfg.elements[0].model_count = 1U;
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_init(&cfg));
  (void)ra_ble_mesh_close();
  cfg.element_count = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_mesh_init(&cfg));
  cfg.element_count = (uint8_t)((uint8_t)k_ra_ble_mesh_max_elements + 1U);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_mesh_init(&cfg));
  TEST_END("mcdc mesh validate (element_count==0 || >MAX)");
}

/**
 * @test test_mcdc_mesh_emit_event_guard
 *
 * @par MC/DC:
 * Decision: `(s_state.event_fn != NULL) && (evt != NULL)`
 * (2 conditions, libs/ra_ble_host/src/ra_ble_mesh.c line 215)
 * Reached via the UNIT_TEST hook ra_ble_mesh_test_emit_event().
 * - V1 fn=non-NULL, evt=non-NULL -> both T. Decision T (callback fires).
 * - V2 fn=NULL,     evt=non-NULL -> C1=F. Decision F (no fire).
 * - V3 fn=non-NULL, evt=NULL     -> C1=T, C2=F. Decision F (no fire).
 * V1+V2 vary C1 with C2 held T. V1+V3 vary C2 with C1 held T. N+1=3.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 */
static void test_mcdc_mesh_emit_event_guard(void)
{
  TEST_BEGIN("mcdc mesh emit_event (fn!=NULL && evt!=NULL)");
  (void)ra_ble_mesh_close();
  ra_ble_mesh_config_t cfg     = {};
  cfg.element_count            = 1U;
  cfg.elements[0].model_count  = 1U;
  cfg.elements[0].models[0].id = 0x0000U;
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_init(&cfg));
  ra_ble_mesh_event_t evt = {.kind = k_ra_ble_mesh_evt_provisioned};
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_attach_event_handler(evt_cb, nullptr));
  s_evt_count = 0U;
  ra_ble_mesh_test_emit_event(&evt);
  TEST_ASSERT_EQ(1U, s_evt_count);
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_attach_event_handler(nullptr, nullptr));
  ra_ble_mesh_test_emit_event(&evt);
  TEST_ASSERT_EQ(1U, s_evt_count);
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_attach_event_handler(evt_cb, nullptr));
  ra_ble_mesh_test_emit_event(nullptr);
  TEST_ASSERT_EQ(1U, s_evt_count);
  TEST_END("mcdc mesh emit_event (fn!=NULL && evt!=NULL)");
}

int main(void)
{
  test_init_null();
  test_init_invalid_count();
  test_lifecycle();
  test_mcdc_mesh_validate_element_count();
  test_mcdc_mesh_emit_event_guard();
  return 0;
}
