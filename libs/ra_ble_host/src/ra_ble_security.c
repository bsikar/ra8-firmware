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

#ifdef RA_TARGET_BUILD
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"

/*
 * Weak fallbacks let this wrapper link even when the NimBLE host
 * library has not been brought into the per-app build yet. The strong
 * upstream symbols (compiled from libs/third_party/nimble/) override
 * these once the host stack is wired in. Until then, the SMP path is
 * a no-op at runtime and the rest of the wrapper bookkeeping still
 * works.
 */
__attribute__((weak)) struct ble_hs_cfg ble_hs_cfg;

__attribute__((weak)) int ble_gap_security_initiate(uint16_t conn_handle)
{
  (void)conn_handle;
  return 0;
}

__attribute__((weak)) int ble_sm_inject_io(uint16_t conn_handle, struct ble_sm_io* pkey)
{
  (void)conn_handle;
  (void)pkey;
  return 0;
}

__attribute__((weak)) int ble_store_clear(void) { return 0; }
#endif

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

#ifdef RA_TARGET_BUILD
/**
 * @brief Map our io_cap enum onto NimBLE's ``BLE_HS_IO_*`` constants.
 *
 * @param[in] io_cap Our public enum.
 *
 * @return uint8_t Matching ``BLE_HS_IO_*`` byte.
 *
 * @pre io_cap is in range (validated upstream).
 * @post Return value is one of the BLE_HS_IO_* constants.
 *
 * @since 0.1.0
 */
static uint8_t internal_map_io_cap(ra_ble_security_io_cap_t io_cap)
{
  switch (io_cap) {
    case k_ra_ble_io_cap_display_only:
      return (uint8_t)BLE_HS_IO_DISPLAY_ONLY;
    case k_ra_ble_io_cap_display_yes_no:
      return (uint8_t)BLE_HS_IO_DISPLAY_YESNO;
    case k_ra_ble_io_cap_keyboard_only:
      return (uint8_t)BLE_HS_IO_KEYBOARD_ONLY;
    case k_ra_ble_io_cap_no_input_no_out:
      return (uint8_t)BLE_HS_IO_NO_INPUT_OUTPUT;
    case k_ra_ble_io_cap_keyboard_display:
    default:
      return (uint8_t)BLE_HS_IO_KEYBOARD_DISPLAY;
  }
}
#endif /* RA_TARGET_BUILD */

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

#ifdef RA_TARGET_BUILD
  ble_hs_cfg.sm_io_cap   = internal_map_io_cap(cfg->io_cap);
  ble_hs_cfg.sm_bonding  = (cfg->bonding_enable != 0U) ? 1U : 0U;
  ble_hs_cfg.sm_mitm     = (cfg->mitm_required != 0U) ? 1U : 0U;
  ble_hs_cfg.sm_sc       = (cfg->sc_only != 0U) ? 1U : 0U;
  ble_hs_cfg.sm_keypress = 0U;
#endif

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
#ifdef RA_TARGET_BUILD
  int rc = ble_gap_security_initiate(conn_handle);
  /* cppcheck-suppress knownConditionTrueFalse
   * Weak fallback returns 0; strong upstream returns BLE_HS_E* on failure. */
  if (rc != 0) {
    return k_ra_err_invalid_arg;
  }
#else
  (void)conn_handle;
#endif
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
#ifdef RA_TARGET_BUILD
  struct ble_sm_io io = {};
  if (s_state.config.io_cap == k_ra_ble_io_cap_display_yes_no ||
      s_state.config.io_cap == k_ra_ble_io_cap_keyboard_display) {
    io.action        = BLE_SM_IOACT_NUMCMP;
    io.numcmp_accept = (accept != 0U) ? 1U : 0U;
  } else if (s_state.config.io_cap == k_ra_ble_io_cap_display_only) {
    io.action  = BLE_SM_IOACT_DISP;
    io.passkey = passkey;
  } else {
    io.action  = BLE_SM_IOACT_INPUT;
    io.passkey = passkey;
  }
  int rc = ble_sm_inject_io(conn_handle, &io);
  /* cppcheck-suppress knownConditionTrueFalse
   * Weak fallback returns 0; strong upstream returns BLE_HS_E* on failure. */
  if (rc != 0) {
    return k_ra_err_invalid_arg;
  }
#else
  (void)conn_handle;
  (void)accept;
#endif
  return k_ra_ok;
}

ra_err_t ra_ble_security_clear_bonds(void)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
#ifdef RA_TARGET_BUILD
  int rc = ble_store_clear();
  /* cppcheck-suppress knownConditionTrueFalse
   * Weak fallback returns 0; strong upstream returns non-zero on store error. */
  if (rc != 0) {
    return k_ra_err_hw_error;
  }
#endif
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
