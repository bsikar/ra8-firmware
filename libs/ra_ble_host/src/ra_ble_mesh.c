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

/*
 * The translation onto the upstream NimBLE Mesh stack
 * (``bt_mesh_init`` / ``bt_mesh_prov_enable`` / ``bt_mesh_reset``)
 * lives in a separate target-only TU (``ra_ble_mesh_target.c``)
 * to keep the host unit-test build free of the mesh vendor headers.
 * That TU is added by a future patch; this file is a portable
 * wrapper that the host and target builds share.
 */

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
  s_state.provisioning_on = 1U;
  return k_ra_ok;
}

ra_err_t ra_ble_mesh_prov_disable(void)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  s_state.provisioning_on = 0U;
  return k_ra_ok;
}

ra_err_t ra_ble_mesh_factory_reset(void)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
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
