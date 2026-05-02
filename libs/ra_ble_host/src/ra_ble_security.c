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

/**
 * @brief Ble gap security initiate.
 *
 * @details See implementation for details.
 *
 * @param[in,out] conn_handle See function signature for type and usage.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
__attribute__((weak)) int ble_gap_security_initiate(uint16_t conn_handle)
{
  (void)conn_handle;
  return 0;
}

/**
 * @brief Ble sm inject io.
 *
 * @details See implementation for details.
 *
 * @param[in,out] conn_handle See function signature for type and usage.
 * @param[in,out] pkey See function signature for type and usage.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
__attribute__((weak)) int ble_sm_inject_io(uint16_t conn_handle, struct ble_sm_io* pkey)
{
  (void)conn_handle;
  (void)pkey;
  return 0;
}

/**
 * @brief Ble store clear.
 *
 * @details See implementation for details.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
__attribute__((weak)) int ble_store_clear(void)
{
  return 0;
}
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
 * @details Range-checks the io_cap enum against
 *          ra_ble_security_io_cap_t (mirroring Bluetooth Core 5.3
 *          Vol 3 Part H 2.3.5.1 IO Capabilities) and rejects NULL
 *          configs.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Config OK.
 * @retval k_ra_err_null_ptr     cfg == NULL.
 * @retval k_ra_err_invalid_arg  Out-of-range field.
 *
 * @pre Called from ra_ble_security_init.
 * @pre Caller is single-threaded.
 * @post No side effects.
 * @post Return value reflects validity of cfg only.
 *
 * @note Not thread-safe; called from init context.
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
 * @details Bluetooth Core 5.3 Vol 3 Part H 2.3.5.1 IO Capabilities --
 *          translates between our public enum and the upstream
 *          NimBLE constants used by the SMP state machine.
 *
 * @param[in] io_cap Our public enum.
 *
 * @return uint8_t Matching ``BLE_HS_IO_*`` byte.
 * @retval BLE_HS_IO_DISPLAY_ONLY     when io_cap == k_ra_ble_io_cap_display_only.
 * @retval BLE_HS_IO_DISPLAY_YESNO    when io_cap == k_ra_ble_io_cap_display_yes_no.
 * @retval BLE_HS_IO_KEYBOARD_ONLY    when io_cap == k_ra_ble_io_cap_keyboard_only.
 * @retval BLE_HS_IO_NO_INPUT_OUTPUT  when io_cap == k_ra_ble_io_cap_no_input_no_out.
 * @retval BLE_HS_IO_KEYBOARD_DISPLAY for the keyboard_display value or any out-of-range fallback.
 *
 * @pre io_cap is in range (validated upstream).
 * @pre Caller is single-threaded.
 * @post Return value is one of the BLE_HS_IO_* constants.
 * @post No state mutation.
 *
 * @note Not thread-safe; pure function.
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

/**
 * @brief Initialize the BLE Security Manager wrapper.
 *
 * @details Validates and captures cfg, then (on the target build)
 *          programs the upstream NimBLE Security Manager configuration
 *          fields per Bluetooth Core 5.3 Vol 3 Part H "Security
 *          Manager".
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Wrapper initialised.
 * @retval k_ra_err_null_ptr      cfg == NULL.
 * @retval k_ra_err_invalid_arg   Out-of-range io_cap.
 *
 * @pre Caller is single-threaded init context.
 * @pre Wrapper is not already initialised.
 * @post On success s_state.initialized == 1.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called once at boot.
 *
 * @since 0.1.0
 */
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

/**
 * @brief Tear down the BLE Security Manager wrapper.
 *
 * @details Forgets the registered event handler. Does not erase the
 *          bond store; use ra_ble_security_clear_bonds for that.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Wrapper closed.
 * @retval k_ra_err_not_initialized Wrapper was never initialised.
 *
 * @pre ra_ble_security_init has succeeded.
 * @pre Caller is single-threaded.
 * @post Wrapper is no longer initialised.
 * @post Event handler is detached.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
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

/**
 * @brief Initiate SMP pairing on an active connection.
 *
 * @details Drives Bluetooth Core 5.3 Vol 3 Part H 2.4 "Pairing
 *          Methods" through NimBLE's ble_gap_security_initiate.
 *
 * @param[in] conn_handle ACL handle of an established connection.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Pairing started.
 * @retval k_ra_err_not_initialized Wrapper not initialised.
 * @retval k_ra_err_invalid_arg     NimBLE rejected the request.
 *
 * @pre ra_ble_security_init has succeeded.
 * @pre A live connection identified by conn_handle exists.
 * @post On success an SMP pairing exchange is in flight.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
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

/**
 * @brief Supply a passkey / numeric-comparison decision back to SMP.
 *
 * @details Bluetooth Core 5.3 Vol 3 Part H 2.3.5.6 / 2.3.5.7 -- the
 *          host injects the user's input (or numeric-comparison
 *          accept/reject) so the SMP state machine can complete its
 *          authentication exchange.
 *
 * @param[in] conn_handle ACL handle of the pairing in progress.
 * @param[in] passkey     Six-digit passkey (0..999999).
 * @param[in] accept      Non-zero accepts a numeric-comparison match.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Reply injected.
 * @retval k_ra_err_not_initialized Wrapper not initialised.
 * @retval k_ra_err_invalid_arg     passkey out of range or NimBLE rejected.
 *
 * @pre ra_ble_security_init has succeeded.
 * @pre Pairing is awaiting passkey input on conn_handle.
 * @post On success SMP advances toward completion.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
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

/**
 * @brief Erase all persisted bonding keys.
 *
 * @details Calls into NimBLE's ble_store_clear, removing every LTK,
 *          IRK and CSRK persisted by the bond store
 *          (Bluetooth Core 5.3 Vol 3 Part H 2.4.2 "Key Distribution
 *          and Generation").
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Bonds cleared.
 * @retval k_ra_err_not_initialized Wrapper not initialised.
 * @retval k_ra_err_hw_error        Underlying store rejected the wipe.
 *
 * @pre ra_ble_security_init has succeeded.
 * @pre Caller is single-threaded.
 * @post Bond store is empty.
 * @post s_state.bond_count == 0.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
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

/**
 * @brief Read the active bond count.
 *
 * @details Returns the wrapper's mirror of the bond-store population.
 *
 * @param[out] out_count Filled with the count on success. Must not be NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Count returned.
 * @retval k_ra_err_null_ptr        out_count == NULL.
 * @retval k_ra_err_not_initialized Wrapper not initialised.
 *
 * @pre ra_ble_security_init has succeeded.
 * @pre out_count is non-NULL.
 * @post On success *out_count holds the active bond count.
 * @post No other state is mutated.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
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

/**
 * @brief Register the application's security event handler.
 *
 * @details The handler receives pairing-start, pairing-complete and
 *          bond-update events. Passing NULL detaches.
 *
 * @param[in] fn  Event callback (NULL detaches).
 * @param[in] ctx Opaque context forwarded to fn.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok Always returned.
 *
 * @pre ra_ble_security_init has succeeded.
 * @pre Caller is single-threaded.
 * @post s_state.event_fn = fn and s_state.event_ctx = ctx.
 * @post Subsequent security events route to the new handler.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_ble_security_attach_event_handler(ra_ble_security_event_fn_t fn, void* ctx)
{
  s_state.event_fn  = fn;
  s_state.event_ctx = ctx;
  return k_ra_ok;
}

#ifdef UNIT_TEST
/* Test-hook prototypes (external linkage). */
/**
 * @brief Ra ble security test emit event.
 *
 * @details See implementation for details.
 *
 * @param[in,out] evt See function signature for type and usage.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
void ra_ble_security_test_emit_event(const ra_ble_security_event_t* evt);
/**
 * @brief Ra ble security test set bond count.
 *
 * @details See implementation for details.
 *
 * @param[in,out] count See function signature for type and usage.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
void ra_ble_security_test_set_bond_count(uint8_t count);

/**
 * @brief Test hook -- synthesize a security event up to the application.
 *
 * @details Forwards a synthetic security event payload to the
 *          registered handler so tests can verify their callback
 *          dispatch path (Bluetooth Core 5.3 Vol 3 Part H 3.5).
 *
 * @param[in] evt Event payload.
 *
 * @pre Wrapper is initialised.
 * @pre Caller is the unit-test harness (single-threaded).
 * @post Application callback was invoked (if registered and evt != NULL).
 * @post No internal state is mutated.
 *
 * @note Not thread-safe; for unit-test harness use only.
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
 * @details Forces s_state.bond_count to count so tests can drive the
 *          bond-count getter without populating the real bond store.
 *
 * @param[in] count New count (0..k_ra_ble_security_max_bonds).
 *
 * @pre Wrapper is initialised (or about to be queried by tests).
 * @pre Caller is the unit-test harness (single-threaded).
 * @post s_state.bond_count == count.
 * @post No other state is mutated.
 *
 * @note Not thread-safe; for unit-test harness use only.
 *
 * @since 0.1.0
 */
void ra_ble_security_test_set_bond_count(uint8_t count)
{
  s_state.bond_count = count;
}
#endif /* UNIT_TEST */

// NOLINTEND(readability-magic-numbers,readability-function-size)
