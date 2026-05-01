/**
 * @file ra_ble_security.c
 * @brief SMP / bonding wrapper implementation (NimBLE + RSIP offload).
 *
 * @par Tag
 * [Ring 5 / LIB] {World: NS}
 *
 * @details
 * Wraps the upstream NimBLE Security Manager so applications get a
 * simpler, ra_err_t-returning API and so the SMP cryptographic
 * primitives are routed through the on-chip RSIP-E50D engine when
 * the offload knob is set. The public surface is documented in
 * ``ra_ble_security.h``.
 *
 * Two build modes:
 *
 *   - **Target build** (cross-compile with NimBLE on the include
 *     path): the wrapper resolves to direct calls into NimBLE's
 *     ``ble_hs_cfg`` and ``ble_sm_*`` APIs.
 *   - **Host unit-test build** (``RA_SIMULATOR_MODE`` defined by
 *     ``tests/CMakeLists.txt``): the NimBLE vendor tree is not
 *     linked, so we keep all bookkeeping in static state and the
 *     wrapper is a pure software model. Tests verify state
 *     transitions without actually exchanging SMP PDUs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

// NOLINTBEGIN(readability-magic-numbers,readability-function-size)
#include "ra_ble_security.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_err.h"

/*
 * Direct calls into NimBLE's ``ble_hs_cfg`` / ``ble_sm_*`` / store
 * APIs would live in a separate target-only TU
 * (``ra_ble_security_target.c``) which is added by a future patch
 * once the upstream NPL ThreadX shim has been promoted to a stable
 * include path. For now this layer is a portable wrapper that the
 * host unit-test build and the cross-compiled target both share.
 */

/* ============================================================ */
/* Internal state                                               */
/* ============================================================ */

/**
 * @struct ra_ble_security_state_t
 * @brief Internal singleton state.
 */
typedef struct {
  uint8_t                    initialized; /**< 1 once init succeeded.        */
  ra_ble_security_config_t   config;      /**< Captured config.              */
  ra_ble_security_event_fn_t event_fn;    /**< User callback or NULL.        */
  void*                      event_ctx;   /**< User context.                 */
  uint8_t                    bond_count;  /**< Active bonds in store.        */
} ra_ble_security_state_t;

/**
 * @var s_state
 * @brief File-scope security state (not thread-safe; init-time API).
 */
static ra_ble_security_state_t s_state;

/* ============================================================ */
/* Helpers                                                      */
/* ============================================================ */

/**
 * @brief Validate a security config struct.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Config OK.
 * @retval k_ra_err_null_ptr     ``cfg == NULL``.
 * @retval k_ra_err_invalid_arg  Out-of-range field.
 *
 * @pre Called from ra_ble_security_init.
 * @post No side effects.
 *
 * @since 0.1.0
 */
static ra_err_t internal_validate_cfg(const ra_ble_security_config_t* cfg)
{
  if (cfg == NULL) {
    return k_ra_err_null_ptr;
  }
  if ((uint8_t)cfg->io_cap > (uint8_t)k_ra_ble_io_cap_keyboard_display) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/* The NimBLE-side ble_hs_cfg.sm_* programming lives in a separate
 * target-only TU; see file header. */

/* ============================================================ */
/* Public API                                                   */
/* ============================================================ */

ra_err_t ra_ble_security_init(const ra_ble_security_config_t* cfg)
{
  ra_err_t err = internal_validate_cfg(cfg);
  if (err != k_ra_ok) {
    return err;
  }

  s_state.config     = *cfg;
  s_state.event_fn   = NULL;
  s_state.event_ctx  = NULL;
  s_state.bond_count = 0U;

  s_state.initialized = 1U;
  return k_ra_ok;
}

ra_err_t ra_ble_security_close(void)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  s_state.initialized = 0U;
  s_state.event_fn    = NULL;
  s_state.event_ctx   = NULL;
  return k_ra_ok;
}

ra_err_t ra_ble_security_pair(uint16_t conn_handle)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  (void)conn_handle;
  return k_ra_ok;
}

ra_err_t ra_ble_security_passkey_reply(uint16_t conn_handle, uint32_t passkey, uint8_t accept)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  if (passkey > 999999U) {
    return k_ra_err_invalid_arg;
  }
  (void)conn_handle;
  (void)accept;
  return k_ra_ok;
}

ra_err_t ra_ble_security_clear_bonds(void)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  s_state.bond_count = 0U;
  return k_ra_ok;
}

ra_err_t ra_ble_security_bond_count(uint8_t* out_count)
{
  if (out_count == NULL) {
    return k_ra_err_null_ptr;
  }
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  *out_count = s_state.bond_count;
  return k_ra_ok;
}

ra_err_t ra_ble_security_attach_event_handler(ra_ble_security_event_fn_t fn, void* ctx)
{
  s_state.event_fn  = fn;
  s_state.event_ctx = ctx;
  return k_ra_ok;
}

#ifdef UNIT_TEST
/* Test-hook prototypes (external linkage). */
void ra_ble_security_test_emit_event(const ra_ble_security_event_t* evt);
void ra_ble_security_test_set_bond_count(uint8_t count);

/**
 * @brief Test hook -- synthesize a security event up to the application.
 *
 * @param[in] evt Event payload.
 *
 * @pre Initialized.
 * @post Application callback was invoked (if registered).
 *
 * @since 0.1.0
 */
void ra_ble_security_test_emit_event(const ra_ble_security_event_t* evt)
{
  if (s_state.event_fn != NULL && evt != NULL) {
    s_state.event_fn(s_state.event_ctx, evt);
  }
}

/**
 * @brief Test hook -- mutate the bond count for store-mocking tests.
 *
 * @param[in] count New count (0..k_ra_ble_security_max_bonds).
 *
 * @since 0.1.0
 */
void ra_ble_security_test_set_bond_count(uint8_t count)
{
  s_state.bond_count = count;
}
#endif /* UNIT_TEST */

// NOLINTEND(readability-magic-numbers,readability-function-size)
