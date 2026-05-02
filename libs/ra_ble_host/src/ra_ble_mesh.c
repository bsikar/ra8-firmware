/**
 * @file ra_ble_mesh.c
 * @brief Bluetooth Mesh wrapper implementation.
 *
 * @par Tag
 * [Ring 5 / LIB] {World: NS}
 *
 * @details
 * Public API documented in ra_ble_mesh.h.
 *
 * The wrapper validates the application-supplied composition data and
 * forwards it to the upstream NimBLE Mesh stack. On the host
 * unit-test build (RA_SIMULATOR_MODE) the upstream calls are stubbed
 * out and tests verify the local bookkeeping only.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

// NOLINTBEGIN(readability-magic-numbers,readability-function-size)
#include "ra_ble_mesh.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_err.h"

#ifdef RA_TARGET_BUILD
/*
 * Forward-declare the upstream mesh entry points instead of pulling in
 * ``mesh/main.h``: that header chains to ``mesh/glue.h`` which expects
 * the per-app syscfg.h on the include path. The wrapper only needs the
 * three function signatures + the bearer enum below to compile.
 */
typedef enum {
  k_bt_mesh_prov_adv  = 1 << 0,
  k_bt_mesh_prov_gatt = 1 << 1,
} ra_internal_bt_mesh_prov_bearer_t;

extern int  bt_mesh_prov_enable(uint32_t bearers);
extern int  bt_mesh_prov_disable(uint32_t bearers);
extern void bt_mesh_reset(void);

/*
 * Weak fallbacks so this TU stays linkable until the NimBLE Mesh
 * objects are wired into the per-app build. Strong upstream symbols
 * override these once the mesh stack is brought in.
 */
__attribute__((weak)) int bt_mesh_prov_enable(uint32_t bearers)
{
  (void)bearers;
  return 0;
}

__attribute__((weak)) int bt_mesh_prov_disable(uint32_t bearers)
{
  (void)bearers;
  return 0;
}

__attribute__((weak)) void bt_mesh_reset(void) {}
#endif

/* ============================================================ */
/* Internal state                                               */
/* ============================================================ */

/**
 * @struct ra_ble_mesh_state_t
 * @brief Internal singleton state.
 */
typedef struct {
  uint8_t                initialized;     /**< 1 once init succeeded.       */
  ra_ble_mesh_config_t   config;          /**< Captured config.             */
  ra_ble_mesh_event_fn_t event_fn;        /**< User callback.               */
  void*                  event_ctx;       /**< User context.                */
  uint8_t                provisioning_on; /**< 1 = beacon on.               */
} ra_ble_mesh_state_t;

/**
 * @var s_state
 * @brief File-scope mesh state.
 */
static ra_ble_mesh_state_t s_state;

/* ============================================================ */
/* Helpers                                                      */
/* ============================================================ */

/**
 * @brief Validate a mesh composition config.
 *
 * @param[in] cfg Mesh config. Must not be NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Config OK.
 * @retval k_ra_err_null_ptr     ``cfg == NULL``.
 * @retval k_ra_err_invalid_arg  Element / model count out of range.
 *
 * @since 0.1.0
 */
static ra_err_t internal_validate(const ra_ble_mesh_config_t* cfg)
{
  if (cfg == NULL) {
    return k_ra_err_null_ptr;
  }
  if (cfg->element_count == 0U || cfg->element_count > (uint8_t)k_ra_ble_mesh_max_elements) {
    return k_ra_err_invalid_arg;
  }
  for (uint8_t i = 0U; i < cfg->element_count; i++) {
    if (cfg->elements[i].model_count > (uint8_t)k_ra_ble_mesh_max_models_per_el) {
      return k_ra_err_invalid_arg;
    }
  }
  return k_ra_ok;
}

/* ============================================================ */
/* Public API                                                   */
/* ============================================================ */

ra_err_t ra_ble_mesh_init(const ra_ble_mesh_config_t* cfg)
{
  ra_err_t err = internal_validate(cfg);
  if (err != k_ra_ok) {
    return err;
  }
  s_state.config          = *cfg;
  s_state.event_fn        = NULL;
  s_state.event_ctx       = NULL;
  s_state.provisioning_on = 0U;

  s_state.initialized = 1U;
  return k_ra_ok;
}

ra_err_t ra_ble_mesh_close(void)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  s_state.initialized     = 0U;
  s_state.provisioning_on = 0U;
  s_state.event_fn        = NULL;
  s_state.event_ctx       = NULL;
  return k_ra_ok;
}

ra_err_t ra_ble_mesh_prov_enable(void)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
#ifdef RA_TARGET_BUILD
  int rc = bt_mesh_prov_enable((uint32_t)(k_bt_mesh_prov_adv | k_bt_mesh_prov_gatt));
  /* cppcheck-suppress knownConditionTrueFalse
   * Weak fallback returns 0; strong upstream returns non-zero on failure. */
  if (rc != 0) {
    return k_ra_err_hw_error;
  }
#endif
  s_state.provisioning_on = 1U;
  return k_ra_ok;
}

ra_err_t ra_ble_mesh_prov_disable(void)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
#ifdef RA_TARGET_BUILD
  int rc = bt_mesh_prov_disable((uint32_t)(k_bt_mesh_prov_adv | k_bt_mesh_prov_gatt));
  /* cppcheck-suppress knownConditionTrueFalse
   * Weak fallback returns 0; strong upstream returns non-zero on failure. */
  if (rc != 0) {
    return k_ra_err_hw_error;
  }
#endif
  s_state.provisioning_on = 0U;
  return k_ra_ok;
}

ra_err_t ra_ble_mesh_factory_reset(void)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
#ifdef RA_TARGET_BUILD
  bt_mesh_reset();
#endif
  s_state.provisioning_on = 0U;
  return k_ra_ok;
}

ra_err_t ra_ble_mesh_attach_event_handler(ra_ble_mesh_event_fn_t fn, void* ctx)
{
  s_state.event_fn  = fn;
  s_state.event_ctx = ctx;
  return k_ra_ok;
}

#ifdef UNIT_TEST
/* Test-hook prototypes (external linkage). */
void    ra_ble_mesh_test_emit_event(const ra_ble_mesh_event_t* evt);
uint8_t ra_ble_mesh_test_prov_active(void);

/**
 * @brief Test hook -- emit a mesh lifecycle event.
 *
 * @since 0.1.0
 */
void ra_ble_mesh_test_emit_event(const ra_ble_mesh_event_t* evt)
{
  if (s_state.event_fn != NULL && evt != NULL) {
    s_state.event_fn(s_state.event_ctx, evt);
  }
}

/**
 * @brief Test hook -- return whether prov beaconing is on.
 *
 * @since 0.1.0
 */
uint8_t ra_ble_mesh_test_prov_active(void)
{
  return s_state.provisioning_on;
}
#endif

// NOLINTEND(readability-magic-numbers,readability-function-size)
