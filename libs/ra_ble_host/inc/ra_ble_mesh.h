/**
 * @file ra_ble_mesh.h
 * @brief Bluetooth Mesh provisioning + element/model registration API.
 *
 * @par Tag
 * [Ring 5 / LIB] {World: NS}
 *
 * @details
 * Wraps the upstream NimBLE Mesh stack (``mesh/glue.h``,
 * ``mesh_models.c``). Apps describe their mesh node as a list of
 * elements, each carrying a list of SIG / vendor models. The wrapper
 * registers the composition data with the underlying stack, runs
 * provisioning either via PB-ADV or PB-GATT, and installs a single
 * application callback that fires on key lifecycle events
 * (provisioned, unprovisioned, key-refresh, IV-update).
 *
 * Bluetooth Mesh Profile 1.0.1 references:
 *   - Section 3.7   -- Composition Data.
 *   - Section 5.4   -- Provisioning.
 *   - Section 7     -- Foundation Models.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/* ============================================================ */
/* Limits                                                       */
/* ============================================================ */

/**
 * @enum ra_ble_mesh_limits_t
 * @brief Static-allocation upper bounds.
 */
typedef enum : uint8_t {
  k_ra_ble_mesh_max_elements      = 4U,  /**< Elements per node.            */
  k_ra_ble_mesh_max_models_per_el = 8U,  /**< Models per element.           */
  k_ra_ble_mesh_uuid_bytes        = 16U, /**< Device UUID length.           */
} ra_ble_mesh_limits_t;

/**
 * @enum ra_ble_mesh_event_kind_t
 * @brief Lifecycle event delivered to the user callback.
 */
typedef enum : uint8_t {
  k_ra_ble_mesh_evt_provisioned   = 0U, /**< Node has been provisioned.       */
  k_ra_ble_mesh_evt_unprovisioned = 1U, /**< Node has been factory-reset.     */
  k_ra_ble_mesh_evt_key_refresh   = 2U, /**< Mesh key refresh phase changed.  */
  k_ra_ble_mesh_evt_iv_update     = 3U, /**< IV update procedure tick.        */
} ra_ble_mesh_event_kind_t;

/* ============================================================ */
/* Composition data                                             */
/* ============================================================ */

/**
 * @struct ra_ble_mesh_model_id_t
 * @brief One model identifier inside an element.
 *
 * @details
 * Either a 16-bit SIG model ID (``vendor == 0``) or a vendor-specific
 * pair (``vendor`` is the company ID; ``id`` is the vendor model ID).
 */
typedef struct {
  uint16_t vendor; /**< 0 for SIG models; else company ID.        */
  uint16_t id;     /**< SIG or vendor model identifier.           */
} ra_ble_mesh_model_id_t;

/**
 * @struct ra_ble_mesh_element_t
 * @brief One element in the node composition.
 */
typedef struct {
  uint16_t               location;    /**< GATT Bluetooth Namespace.    */
  uint8_t                model_count; /**< Number of valid model IDs.   */
  ra_ble_mesh_model_id_t models[k_ra_ble_mesh_max_models_per_el]; /**< Models. */
} ra_ble_mesh_element_t;

/**
 * @struct ra_ble_mesh_config_t
 * @brief Configuration passed to ``ra_ble_mesh_init``.
 */
typedef struct {
  uint16_t              cid;                            /**< Company ID for composition data.   */
  uint16_t              pid;                            /**< Product ID.                        */
  uint16_t              vid;                            /**< Version ID.                        */
  uint8_t               uuid[k_ra_ble_mesh_uuid_bytes]; /**< Device UUID.       */
  uint8_t               element_count;                  /**< Number of valid elements.          */
  ra_ble_mesh_element_t elements[k_ra_ble_mesh_max_elements]; /**< Elements.    */
} ra_ble_mesh_config_t;

/**
 * @struct ra_ble_mesh_event_t
 * @brief Event payload delivered to the user callback.
 */
typedef struct {
  ra_ble_mesh_event_kind_t kind;    /**< Event kind.                  */
  uint16_t                 net_idx; /**< Active subnet (NetKey idx).  */
  uint16_t                 addr;    /**< Primary unicast address.     */
} ra_ble_mesh_event_t;

/**
 * @typedef ra_ble_mesh_event_fn_t
 * @brief Mesh lifecycle event callback.
 */
typedef void (*ra_ble_mesh_event_fn_t)(void* ctx, const ra_ble_mesh_event_t* evt);

/* ============================================================ */
/* Lifecycle                                                    */
/* ============================================================ */

/**
 * @brief Bring up the Bluetooth Mesh stack.
 *
 * @details
 * 1. Validate ``cfg`` (counts in range, model arrays consistent).
 * 2. Translate ``cfg`` into the upstream ``struct bt_mesh_elem`` /
 *    ``struct bt_mesh_model`` form.
 * 3. Call ``bt_mesh_init`` which starts the proxy / provisioning
 *    bearers wired up by syscfg.h (PB-ADV + PB-GATT).
 *
 * @param[in] cfg Composition data. Must not be NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                   Mesh stack up.
 * @retval k_ra_err_null_ptr         ``cfg == NULL``.
 * @retval k_ra_err_invalid_arg      Counts out of range.
 * @retval k_ra_err_not_initialized  ra_ble_host_init was not called.
 *
 * @pre ra_ble_host_init() succeeded.
 * @post Node is in unprovisioned-and-beaconing state until provisioned.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_mesh_init(const ra_ble_mesh_config_t* cfg);

/**
 * @brief Tear down the mesh stack.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                   Stopped.
 * @retval k_ra_err_not_initialized  Not initialized.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_mesh_close(void);

/* ============================================================ */
/* Provisioning controls                                        */
/* ============================================================ */

/**
 * @brief Start the unprovisioned beacon broadcast.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                   Beacon started.
 * @retval k_ra_err_not_initialized  Stack not initialized.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_mesh_prov_enable(void);

/**
 * @brief Stop the unprovisioned beacon broadcast.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                   Beacon stopped.
 * @retval k_ra_err_not_initialized  Stack not initialized.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_mesh_prov_disable(void);

/**
 * @brief Reset the node back to unprovisioned, clearing all keys.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                   Reset complete.
 * @retval k_ra_err_not_initialized  Stack not initialized.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_mesh_factory_reset(void);

/**
 * @brief Register the application's mesh event callback.
 *
 * @param[in] fn  Callback. NULL detaches.
 * @param[in] ctx Opaque user context.
 *
 * @return ra_err_t k_ra_ok always.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_mesh_attach_event_handler(ra_ble_mesh_event_fn_t fn, void* ctx);

#ifdef __cplusplus
}
#endif
