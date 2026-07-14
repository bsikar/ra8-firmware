/**
 * @file ra8_ble_security.h
 * @brief BLE Security Manager Protocol (SMP) bonding + crypto offload.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 5 / LIB] {World: NS}
 *
 * @details
 * Public surface for the SMP / pairing / bonding wrapper layered on
 * top of Apache NimBLE's ``ble_sm`` module. The layer:
 *
 *   - Routes SMP cryptographic primitives (AES-CMAC, AES-128-ECB and
 *     ECDH on the NIST P-256 curve) through the on-chip RSIP-E50D
 *     security engine via libs/ra8_hal/ra8_rsip rather than the
 *     bundled tinycrypt software fallback. This keeps long-term
 *     keys off the M85 cores and benefits from RSIP DPA hardening.
 *   - Exposes a small bonding API that wraps the NimBLE host key
 *     store: enable / disable bonding, clear the persistent store,
 *     enumerate currently bonded peers, and force a re-pair.
 *   - Surfaces an event callback so applications can prompt the user
 *     for passkey entry / numeric comparison without touching the
 *     NimBLE host event loop directly.
 *
 * The layer is initialized by ``ra8_ble_security_init`` after
 * ``ra8_ble_host_init`` has brought the controller + NimBLE host
 * stack up. Deinit is implicit in ``ra8_ble_host_close``.
 *
 * Bluetooth Core 5.3 references:
 *   - Vol 3 Part H 2 -- Security Manager.
 *   - Vol 3 Part H 2.3 -- Pairing methods.
 *   - Vol 3 Part H 2.4.4 -- LE Secure Connections key generation
 *     (uses the ECDH P-256 + f4/f5/f6 cryptographic toolbox).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/* ============================================================ */
/* Limits */
/* ============================================================ */

/**
 * @enum ra8_ble_security_limits_t
 * @brief Static-allocation upper bounds for the security layer.
 */
typedef enum : uint8_t {
  k_ra8_ble_security_max_bonds      = 4U,  /**< Persistent-store bond slots. */
  k_ra8_ble_security_passkey_digits = 6U,  /**< SMP passkey digit count.     */
  k_ra8_ble_security_addr_bytes     = 6U,  /**< 48-bit BD_ADDR length.       */
  k_ra8_ble_security_p256_key_bytes = 32U, /**< P-256 private key length.    */
} ra8_ble_security_limits_t;

/* ============================================================ */
/* I/O capabilities + auth requirements */
/* ============================================================ */

/**
 * @enum ra8_ble_security_io_cap_t
 * @brief I/O capability advertised in the Pairing Request.
 *
 * @details
 * Bluetooth Core 5.3 Vol 3 Part H 2.3.2 Table 2.4. The chosen value
 * gates which pairing method (Just Works / Passkey / Numeric
 * Comparison / OOB) the SMP state machine selects.
 */
typedef enum : uint8_t {
  k_ra8_ble_io_cap_display_only     = 0U, /**< Display passkey, no input.  */
  k_ra8_ble_io_cap_display_yes_no   = 1U, /**< Display + yes/no buttons.   */
  k_ra8_ble_io_cap_keyboard_only    = 2U, /**< Numeric keypad, no display. */
  k_ra8_ble_io_cap_no_input_no_out  = 3U, /**< Just Works only.            */
  k_ra8_ble_io_cap_keyboard_display = 4U, /**< Both keypad and display.    */
} ra8_ble_security_io_cap_t;

/**
 * @enum ra8_ble_security_event_kind_t
 * @brief Event delivered to the user-supplied security callback.
 */
typedef enum : uint8_t {
  k_ra8_ble_sec_evt_passkey_display = 0U, /**< Display the 6-digit passkey.    */
  k_ra8_ble_sec_evt_passkey_request = 1U, /**< User must enter peer's passkey. */
  k_ra8_ble_sec_evt_numeric_compare = 2U, /**< Confirm numeric match (yes/no). */
  k_ra8_ble_sec_evt_pairing_pass    = 3U, /**< Pairing complete + bonded.      */
  k_ra8_ble_sec_evt_pairing_fail    = 4U, /**< Pairing failed (see status).    */
} ra8_ble_security_event_kind_t;

/* ============================================================ */
/* Configuration / event payload */
/* ============================================================ */

/**
 * @struct ra8_ble_security_config_t
 * @brief Configuration passed to ``ra8_ble_security_init``.
 *
 * @details
 * Mirrors the per-side SMP knobs in NimBLE's ``ble_sm`` module so
 * applications never have to call ble_hs_cfg directly.
 */
typedef struct {
  ra8_ble_security_io_cap_t io_cap; /**< Local I/O capability. */
  // cppcheck-suppress unusedStructMember
  uint8_t bonding_enable; /**< 1 = persist keys to store. */
  // cppcheck-suppress unusedStructMember
  uint8_t mitm_required; /**< 1 = require MITM protection. */
  // cppcheck-suppress unusedStructMember
  uint8_t sc_only; /**< 1 = LE Secure Connections only. */
  // cppcheck-suppress unusedStructMember
  uint8_t use_rsip_offload; /**< 1 = ECDH/CMAC via RSIP. */
} ra8_ble_security_config_t;

/**
 * @struct ra8_ble_security_event_t
 * @brief Event payload delivered to the user callback.
 */
typedef struct {
  ra8_ble_security_event_kind_t kind; /**< Event kind. */
  // cppcheck-suppress unusedStructMember
  uint16_t conn_handle; /**< ACL connection handle. */
  // cppcheck-suppress unusedStructMember
  uint32_t passkey; /**< Passkey (display / compare evts). */
  // cppcheck-suppress unusedStructMember
  uint8_t status; /**< SMP failure reason on _pairing_fail. */
} ra8_ble_security_event_t;

/**
 * @typedef ra8_ble_security_event_fn_t
 * @brief Application-level security event callback.
 *
 * @param[in] ctx Opaque user context.
 * @param[in] evt Event payload, valid only for the call duration.
 */
typedef void (*ra8_ble_security_event_fn_t)(void* ctx, const ra8_ble_security_event_t* evt);

/* ============================================================ */
/* Lifecycle */
/* ============================================================ */

/**
 * @brief Bring up the SMP / bonding wrapper.
 *
 * @details
 * 1. Validate ``cfg``.
 * 2. Programme NimBLE's ``ble_hs_cfg.sm_*`` knobs from the config.
 * 3. Optionally install RSIP-backed implementations of the SMP
 *    cryptographic primitives via ``ble_sm_alg_*_hw_set``.
 * 4. Reset the host key store (if bonding disabled) or attach the
 *    in-RAM key store (if bonding enabled).
 *
 * @param[in] cfg Security configuration. Must not be NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   SMP layer is up.
 * @retval k_ra8_err_null_ptr         ``cfg == NULL``.
 * @retval k_ra8_err_invalid_arg      ``cfg->io_cap`` out of range.
 * @retval k_ra8_err_not_initialized  ra8_ble_host_init was never called.
 * @retval k_ra8_err_hw_init_failed   RSIP self-test failed (offload only).
 *
 * @pre ra8_ble_host_init() succeeded.
 * @pre RSIP power gating cleared (HUM Ch 11 MSTPCRC.MSTPC31).
 * @post ble_hs_cfg.sm_* knobs reflect ``cfg``.
 * @post Subsequent SMP-protected GATT operations work end-to-end.
 *
 * @note Init-time call. Not thread-safe.
 *
 * @see ra8_ble_security_close()
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_security_init(const ra8_ble_security_config_t* cfg);

/**
 * @brief Tear down the SMP wrapper.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Stack closed.
 * @retval k_ra8_err_not_initialized Not initialized.
 *
 * @pre ra8_ble_security_init succeeded previously.
 * @post Subsequent SMP calls return k_ra8_err_not_initialized.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_security_close(void);

/* ============================================================ */
/* Pairing / bonding APIs */
/* ============================================================ */

/**
 * @brief Initiate pairing on an existing ACL connection.
 *
 * @details
 * Issues an SMP Pairing Request on ``conn_handle``. The pairing flow
 * runs to completion in the NimBLE host event loop and the result
 * surfaces through the registered security event callback.
 *
 * @param[in] conn_handle Active ACL connection handle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Pairing request queued.
 * @retval k_ra8_err_not_initialized  Security wrapper not initialized.
 * @retval k_ra8_err_invalid_arg      ``conn_handle`` is not connected.
 *
 * @pre ra8_ble_security_init() succeeded.
 * @pre ``conn_handle`` references an established LE connection.
 * @post Pairing event is posted to the host event queue.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_security_pair(uint16_t conn_handle);

/**
 * @brief Reply to a passkey-entry / numeric-compare event.
 *
 * @details
 * Used by the application's UI thread after it has prompted the user
 * for the 6-digit passkey or for a yes/no numeric-comparison
 * confirmation.
 *
 * @param[in] conn_handle Active ACL connection handle.
 * @param[in] passkey     6-digit passkey (0..999999); ignored for
 *                        numeric-compare confirmations.
 * @param[in] accept      Non-zero accepts the comparison, zero rejects.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Reply delivered to NimBLE.
 * @retval k_ra8_err_not_initialized  Security wrapper not initialized.
 * @retval k_ra8_err_invalid_arg      ``passkey > 999999`` or unknown handle.
 *
 * @pre A k_ra8_ble_sec_evt_passkey_request or _numeric_compare event
 *      was previously delivered for ``conn_handle``.
 * @post NimBLE's SMP state machine advances or fails.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ble_security_passkey_reply(uint16_t conn_handle, uint32_t passkey, uint8_t accept);

/**
 * @brief Erase every persistent bond.
 *
 * @details
 * Walks the host key store and deletes every LTK / IRK / CSRK
 * entry, then clears the CCCD cache. Useful as a "factory reset"
 * trigger.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Store cleared.
 * @retval k_ra8_err_not_initialized  Security wrapper not initialized.
 *
 * @pre ra8_ble_security_init() succeeded.
 * @post Host key store is empty.
 * @post Subsequent reconnections require fresh pairing.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_security_clear_bonds(void);

/**
 * @brief Return the count of currently stored bonds.
 *
 * @param[out] out_count Receives the count (0..k_ra8_ble_security_max_bonds).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Count written.
 * @retval k_ra8_err_null_ptr         ``out_count == NULL``.
 * @retval k_ra8_err_not_initialized  Security wrapper not initialized.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_security_bond_count(uint8_t* out_count);

/* ============================================================ */
/* Event handler */
/* ============================================================ */

/**
 * @brief Register the application's security event callback.
 *
 * @param[in] fn  Callback. NULL detaches.
 * @param[in] ctx Opaque user context.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Always.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ble_security_attach_event_handler(ra8_ble_security_event_fn_t fn,
                                                              void*                       ctx);

#ifdef __cplusplus
}
#endif
