/**
 * @file ra8_ble_mesh.c
 * @brief Bluetooth Mesh wrapper implementation.
 *
 * @par Tag
 * [Ring 5 / LIB] {World: NS}
 *
 * @details
 * Public API documented in ra8_ble_mesh.h.
 *
 * The wrapper validates the application-supplied composition data and
 * forwards it to the upstream NimBLE Mesh stack. On the host
 * unit-test build (RA8_SIMULATOR_MODE) the upstream calls are stubbed
 * out and tests verify the local bookkeeping only.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_ble_mesh.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

#ifdef RA8_TARGET_BUILD
/*
 * Forward-declare the upstream mesh entry points instead of pulling in
 * ``mesh/main.h``: that header chains to ``mesh/glue.h`` which expects
 * the per-app syscfg.h on the include path. The wrapper only needs the
 * three function signatures + the bearer enum below to compile.
 */
typedef enum : uint8_t {
  k_bt_mesh_prov_adv  = 1 << 0, /**< Bt mesh prov adv.  */
  k_bt_mesh_prov_gatt = 1 << 1, /**< Bt mesh prov GATT. */
} ra8_internal_bt_mesh_prov_bearer_t;

extern int  bt_mesh_prov_enable(uint32_t bearers);
extern int  bt_mesh_prov_disable(uint32_t bearers);
extern void bt_mesh_reset(void);

/*
 * Weak fallbacks so this TU stays linkable until the NimBLE Mesh
 * objects are wired into the per-app build. Strong upstream symbols
 * override these once the mesh stack is brought in.
 */
[[gnu::weak]] int bt_mesh_prov_enable(uint32_t bearers)
{
  (void)bearers;
  return 0;
}

[[gnu::weak]] int bt_mesh_prov_disable(uint32_t bearers)
{
  (void)bearers;
  return 0;
}

[[gnu::weak]] void bt_mesh_reset(void) {}
#endif

/* ============================================================ */
/* Internal state */
/* ============================================================ */

/**
 * @struct ra8_ble_mesh_state_t
 * @brief Internal singleton state.
 */
typedef struct {
  uint8_t                 initialized;     /**< 1 once init succeeded. */
  ra8_ble_mesh_config_t   config;          /**< Captured config.       */
  ra8_ble_mesh_event_fn_t event_fn;        /**< User callback.         */
  void*                   event_ctx;       /**< User context.          */
  uint8_t                 provisioning_on; /**< 1 = beacon on.         */
} ra8_ble_mesh_state_t;

/**
 * @var s_state
 * @brief File-scope mesh state.
 */
static ra8_ble_mesh_state_t s_state;

/* ============================================================ */
/* Helpers */
/* ============================================================ */

/**
 * @brief Validate a mesh composition config.
 *
 * @details Checks element / model counts against the limits in
 *          ra8_ble_mesh_limits_t (see Bluetooth Mesh Profile 1.0.1
 *          sec 4.2 "Composition Data" for the underlying concept).
 *
 * @param[in] cfg Mesh config. Must not be NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Config OK.
 * @retval k_ra8_err_null_ptr     cfg == NULL.
 * @retval k_ra8_err_invalid_arg  Element / model count out of range.
 *
 * @pre Called from ra8_ble_mesh_init.
 * @pre Caller is single-threaded.
 * @post No state mutation.
 * @post Return value reflects validity of cfg only.
 *
 * @note Not thread-safe; called from init context.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate(const ra8_ble_mesh_config_t* cfg)
{
  if (cfg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg->element_count == 0U || cfg->element_count > (uint8_t)k_ra8_ble_mesh_max_elements) {
    return k_ra8_err_invalid_arg;
  }
  for (uint8_t i = 0U; i < cfg->element_count; i++) {
    if (cfg->elements[i].model_count > (uint8_t)k_ra8_ble_mesh_max_models_per_el) {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}

/* ============================================================ */
/* Public API */
/* ============================================================ */

/**
 * @brief Initialize the Bluetooth Mesh wrapper.
 *
 * @details Validates the supplied composition data (Mesh Profile
 *          1.0.1 sec 4.2) and captures it into the singleton state.
 *          The upstream NimBLE Mesh stack is left dormant until
 *          ra8_ble_mesh_prov_enable is called.
 *
 * @param[in] cfg Mesh composition config. Must not be NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Wrapper initialized.
 * @retval k_ra8_err_null_ptr     cfg == NULL.
 * @retval k_ra8_err_invalid_arg  Composition out of range.
 *
 * @pre cfg points to a valid composition struct.
 * @pre Wrapper is not already initialized.
 * @post On success s_state.initialized == 1.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called once at boot.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_mesh_init(const ra8_ble_mesh_config_t* cfg)
{
  ra8_err_t err = internal_validate(cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  s_state.config          = *cfg;
  s_state.event_fn        = nullptr;
  s_state.event_ctx       = nullptr;
  s_state.provisioning_on = 0U;

  s_state.initialized = 1U;
  return k_ra8_ok;
}

/**
 * @brief Tear down the Bluetooth Mesh wrapper.
 *
 * @details Clears the captured config and forgets any registered
 *          event handler. Does not unprovision an already-provisioned
 *          node; use ra8_ble_mesh_factory_reset for that.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Wrapper torn down.
 * @retval k_ra8_err_not_initialized Wrapper was never initialized.
 *
 * @pre ra8_ble_mesh_init has succeeded.
 * @pre Caller is single-threaded.
 * @post Wrapper is no longer initialized.
 * @post Provisioning beacon is off.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_mesh_close(void)
{
  if (s_state.initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
  s_state.initialized     = 0U;
  s_state.provisioning_on = 0U;
  s_state.event_fn        = nullptr;
  s_state.event_ctx       = nullptr;
  return k_ra8_ok;
}

/**
 * @brief Start advertising the unprovisioned-device beacon.
 *
 * @details Mesh Profile 1.0.1 sec 3.9 "Provisioning". Enables both
 *          the PB-ADV bearer and PB-GATT so phones and dedicated
 *          provisioners can both reach the node.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Beaconing started.
 * @retval k_ra8_err_not_initialized Wrapper not initialized.
 * @retval k_ra8_err_hw_error        Upstream NimBLE Mesh rejected.
 *
 * @pre ra8_ble_mesh_init has succeeded.
 * @pre Caller is single-threaded.
 * @post On success s_state.provisioning_on == 1.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_mesh_prov_enable(void)
{
  if (s_state.initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
#ifdef RA8_TARGET_BUILD
  int rc = bt_mesh_prov_enable((uint32_t)(k_bt_mesh_prov_adv | k_bt_mesh_prov_gatt));
  /* cppcheck-suppress knownConditionTrueFalse -- the in-tree weak fallback returns 0; the strong upstream NimBLE symbol returns non-zero on failure. */
  if (rc != 0) {
    return k_ra8_err_hw_error;
  }
#endif
  s_state.provisioning_on = 1U;
  return k_ra8_ok;
}

/**
 * @brief Stop advertising the unprovisioned-device beacon.
 *
 * @details Mesh Profile 1.0.1 sec 3.9. Disables both PB-ADV and
 *          PB-GATT bearers.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Beaconing stopped.
 * @retval k_ra8_err_not_initialized Wrapper not initialized.
 * @retval k_ra8_err_hw_error        Upstream NimBLE Mesh rejected.
 *
 * @pre ra8_ble_mesh_init has succeeded.
 * @pre Caller is single-threaded.
 * @post s_state.provisioning_on == 0.
 * @post Underlying mesh stack no longer beacons.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_mesh_prov_disable(void)
{
  if (s_state.initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
#ifdef RA8_TARGET_BUILD
  int rc = bt_mesh_prov_disable((uint32_t)(k_bt_mesh_prov_adv | k_bt_mesh_prov_gatt));
  /* cppcheck-suppress knownConditionTrueFalse -- the in-tree weak fallback returns 0; the strong upstream NimBLE symbol returns non-zero on failure. */
  if (rc != 0) {
    return k_ra8_err_hw_error;
  }
#endif
  s_state.provisioning_on = 0U;
  return k_ra8_ok;
}

/**
 * @brief Wipe persisted Mesh provisioning data.
 *
 * @details Mesh Profile 1.0.1 sec 3.10 "Node Removal". Calls into
 *          NimBLE's bt_mesh_reset, which forgets the provisioning
 *          data, network keys, app keys and bindings.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Reset issued.
 * @retval k_ra8_err_not_initialized Wrapper not initialized.
 *
 * @pre ra8_ble_mesh_init has succeeded.
 * @pre Caller is single-threaded.
 * @post Persisted provisioning state is wiped.
 * @post s_state.provisioning_on == 0.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_mesh_factory_reset(void)
{
  if (s_state.initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
#ifdef RA8_TARGET_BUILD
  bt_mesh_reset();
#endif
  s_state.provisioning_on = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_ble_mesh_attach_event_handler(ra8_ble_mesh_event_fn_t fn, void* ctx)
{
  s_state.event_fn  = fn;
  s_state.event_ctx = ctx;
  return k_ra8_ok;
}

#ifdef UNIT_TEST
/* Test-hook prototypes (external linkage). */
RA8_TEST_HELPER void    ra8_ble_mesh_test_emit_event(const ra8_ble_mesh_event_t* evt);
RA8_TEST_HELPER uint8_t ra8_ble_mesh_test_prov_active(void);

/**
 * @brief Test hook -- emit a mesh lifecycle event.
 *
 * @details Forwards a synthetic event payload to the registered
 *          handler so tests can verify their callback dispatch.
 *
 * @param[in] evt Event payload (must not be NULL for delivery).
 *
 * @pre Wrapper is initialized.
 * @pre Caller is the unit-test harness (single-threaded).
 * @post If a handler is registered and evt != NULL it has been invoked.
 * @post No internal state is mutated.
 *
 * @note Not thread-safe; for unit-test harness use only.
 *
 * @since 0.1.0
 */
void ra8_ble_mesh_test_emit_event(const ra8_ble_mesh_event_t* evt)
{
  if (s_state.event_fn != nullptr && evt != nullptr) {
    s_state.event_fn(s_state.event_ctx, evt);
  }
}

/**
 * @brief Test hook -- return whether prov beaconing is on.
 *
 * @details Reads the cached s_state.provisioning_on flag so tests can
 *          assert the lifecycle of ra8_ble_mesh_prov_enable /
 *          ra8_ble_mesh_prov_disable.
 *
 * @return uint8_t 1 when prov beaconing is on, 0 otherwise.
 * @retval 0 Provisioning beacon is off.
 * @retval 1 Provisioning beacon is on.
 *
 * @pre Wrapper is initialized.
 * @pre Caller is the unit-test harness (single-threaded).
 * @post No state is mutated.
 * @post Return value matches s_state.provisioning_on.
 *
 * @note Not thread-safe; for unit-test harness use only.
 *
 * @since 0.1.0
 */
uint8_t ra8_ble_mesh_test_prov_active(void)
{
  return s_state.provisioning_on;
}
#endif
