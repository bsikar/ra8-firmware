/**
 * @file ra_ble_gatt_client.c
 * @brief GATT client wrapper implementation.
 *
 * @par Tag
 * [Ring 5 / LIB] {World: NS}
 *
 * @details
 * Public API documented in ra_ble_gatt_client.h.
 *
 * Two build modes:
 *   - Target build: ``ble_gattc_*`` from NimBLE handles the actual
 *     PDUs.
 *   - Host unit-test build (``RA_SIMULATOR_MODE``): the wrapper keeps
 *     bookkeeping but never sends anything. Tests can drive the
 *     callbacks via the ``UNIT_TEST`` hooks at the bottom of the TU.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

// NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity)
#include "ra_ble_gatt_client.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_err.h"

#ifdef RA_TARGET_BUILD
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"

/*
 * Weak fallbacks to keep this TU linkable even before the NimBLE host
 * library has been brought into the per-app build. The strong upstream
 * symbols override these once the host stack is wired in.
 */
__attribute__((weak)) int ble_gattc_disc_all_svcs(uint16_t              conn_handle,
                                                  ble_gatt_disc_svc_fn* cb,
                                                  void*                 cb_arg)
{
  (void)conn_handle;
  (void)cb;
  (void)cb_arg;
  return 0;
}

__attribute__((weak)) int ble_gattc_read(uint16_t          conn_handle,
                                         uint16_t          attr_handle,
                                         ble_gatt_attr_fn* cb,
                                         void*             cb_arg)
{
  (void)conn_handle;
  (void)attr_handle;
  (void)cb;
  (void)cb_arg;
  return 0;
}

__attribute__((weak)) int ble_gattc_write_flat(uint16_t          conn_handle,
                                               uint16_t          attr_handle,
                                               const void*       data,
                                               uint16_t          data_len,
                                               ble_gatt_attr_fn* cb,
                                               void*             cb_arg)
{
  (void)conn_handle;
  (void)attr_handle;
  (void)data;
  (void)data_len;
  (void)cb;
  (void)cb_arg;
  return 0;
}

__attribute__((weak)) int ble_gattc_write_no_rsp_flat(uint16_t    conn_handle,
                                                      uint16_t    attr_handle,
                                                      const void* data,
                                                      uint16_t    data_len)
{
  (void)conn_handle;
  (void)attr_handle;
  (void)data;
  (void)data_len;
  return 0;
}

__attribute__((weak)) int ble_hs_mbuf_to_flat(const struct os_mbuf* om,
                                              void*                 flat,
                                              uint16_t              max_len,
                                              uint16_t*             out_copy_len)
{
  (void)om;
  (void)flat;
  (void)max_len;
  if (out_copy_len != NULL) {
    *out_copy_len = 0U;
  }
  return 0;
}
#endif

/* ============================================================ */
/* Internal state                                               */
/* ============================================================ */

/**
 * @struct ra_ble_gatt_client_pending_t
 * @brief Per-request bookkeeping (one in flight at a time).
 */
typedef struct {
  uint8_t                in_use;
  uint16_t               conn_handle;
  ra_ble_gatt_disc_fn_t  disc_cb;
  ra_ble_gatt_read_fn_t  read_cb;
  ra_ble_gatt_write_fn_t write_cb;
  void*                  ctx;
} ra_ble_gatt_client_pending_t;

/**
 * @struct ra_ble_gatt_client_sub_t
 * @brief Tracked notification subscription.
 */
typedef struct {
  uint8_t                 in_use;
  uint16_t                conn_handle;
  uint16_t                cccd_handle;
  ra_ble_gatt_notify_fn_t notify_cb;
  void*                   ctx;
} ra_ble_gatt_client_sub_t;

typedef enum : uint8_t {
  k_ra_gatt_client_max_subs = 4U,
} ra_ble_gatt_client_internal_t;

/**
 * @var s_pending_disc
 * @brief Discovery operation in flight (singleton).
 */
static ra_ble_gatt_client_pending_t s_pending_disc;

/**
 * @var s_pending_read
 * @brief Read operation in flight (singleton).
 */
static ra_ble_gatt_client_pending_t s_pending_read;

/**
 * @var s_pending_write
 * @brief Write operation in flight (singleton).
 */
static ra_ble_gatt_client_pending_t s_pending_write;

/**
 * @var s_subs
 * @brief Subscription table.
 */
static ra_ble_gatt_client_sub_t s_subs[k_ra_gatt_client_max_subs];

/* ============================================================ */
/* Helpers                                                      */
/* ============================================================ */

#ifdef RA_TARGET_BUILD
/**
 * @brief Trampoline mapping NimBLE's discovery callback onto our API.
 *
 * @param[in] conn_handle ACL handle.
 * @param[in] error       NimBLE error struct (NULL on per-row).
 * @param[in] service     Discovered service row, or NULL on completion.
 * @param[in] arg         User-provided context (unused; we use s_pending_disc).
 *
 * @return int 0 to continue.
 *
 * @pre s_pending_disc.in_use == 1.
 * @post On final invocation s_pending_disc.in_use is cleared.
 *
 * @since 0.1.0
 */
static int internal_disc_trampoline(uint16_t                       conn_handle,
                                    const struct ble_gatt_error*   error,
                                    const struct ble_gatt_svc*     service,
                                    void*                          arg)
{
  (void)conn_handle;
  (void)arg;
  ra_ble_gatt_disc_fn_t cb  = s_pending_disc.disc_cb;
  void*                 ctx = s_pending_disc.ctx;
  uint16_t              status =
    (error != NULL) ? (uint16_t)error->status : (uint16_t)0U;
  if (service != NULL && cb != NULL) {
    ra_ble_gatt_service_t row = {};
    row.start_handle          = service->start_handle;
    row.end_handle            = service->end_handle;
    /* Copy whichever UUID form NimBLE supplied, zero-padding to 128. */
    const ble_uuid_t* u = &service->uuid.u;
    if (u->type == BLE_UUID_TYPE_16) {
      uint16_t v = ((const ble_uuid16_t*)u)->value;
      row.uuid_128[0] = (uint8_t)(v & 0xFFU);
      row.uuid_128[1] = (uint8_t)((v >> 8) & 0xFFU);
    } else if (u->type == BLE_UUID_TYPE_32) {
      uint32_t v = ((const ble_uuid32_t*)u)->value;
      row.uuid_128[0] = (uint8_t)(v & 0xFFU);
      row.uuid_128[1] = (uint8_t)((v >> 8) & 0xFFU);
      row.uuid_128[2] = (uint8_t)((v >> 16) & 0xFFU);
      row.uuid_128[3] = (uint8_t)((v >> 24) & 0xFFU);
    } else {
      memcpy(row.uuid_128, ((const ble_uuid128_t*)u)->value,
             sizeof(row.uuid_128));
    }
    cb(ctx, &row, status);
    return 0;
  }
  /* Final completion. */
  s_pending_disc.in_use = 0U;
  if (cb != NULL) {
    cb(ctx, NULL, status);
  }
  return 0;
}

/**
 * @brief Trampoline mapping NimBLE's read callback onto our API.
 *
 * @since 0.1.0
 */
static int internal_read_trampoline(uint16_t                     conn_handle,
                                    const struct ble_gatt_error* error,
                                    struct ble_gatt_attr*        attr,
                                    void*                        arg)
{
  (void)conn_handle;
  (void)arg;
  ra_ble_gatt_read_fn_t cb     = s_pending_read.read_cb;
  void*                 ctx    = s_pending_read.ctx;
  uint16_t              status =
    (error != NULL) ? (uint16_t)error->status : (uint16_t)0U;
  s_pending_read.in_use = 0U;
  if (cb != NULL) {
    if (attr != NULL && attr->om != NULL) {
      uint16_t len = OS_MBUF_PKTLEN(attr->om);
      if (len > (uint16_t)k_ra_ble_gatt_client_max_read_bytes) {
        len = (uint16_t)k_ra_ble_gatt_client_max_read_bytes;
      }
      uint8_t  buf[k_ra_ble_gatt_client_max_read_bytes];
      uint16_t out_len = 0U;
      int mc = ble_hs_mbuf_to_flat(attr->om, buf, len, &out_len);
      /* cppcheck-suppress knownConditionTrueFalse
       * Weak fallback returns 0; strong upstream returns non-zero on copy failure. */
      cb(ctx, buf, (mc == 0) ? out_len : 0U, status);
    } else {
      cb(ctx, NULL, 0U, status);
    }
  }
  return 0;
}

/**
 * @brief Trampoline mapping NimBLE's write completion onto our API.
 *
 * @since 0.1.0
 */
static int internal_write_trampoline(uint16_t                     conn_handle,
                                     const struct ble_gatt_error* error,
                                     struct ble_gatt_attr*        attr,
                                     void*                        arg)
{
  (void)conn_handle;
  (void)attr;
  (void)arg;
  ra_ble_gatt_write_fn_t cb     = s_pending_write.write_cb;
  void*                  ctx    = s_pending_write.ctx;
  uint16_t               status =
    (error != NULL) ? (uint16_t)error->status : (uint16_t)0U;
  s_pending_write.in_use = 0U;
  if (cb != NULL) {
    cb(ctx, status);
  }
  return 0;
}
#endif /* RA_TARGET_BUILD */

/* ============================================================ */
/* Public API                                                   */
/* ============================================================ */

ra_err_t ra_ble_gatt_discover_services(uint16_t conn_handle, ra_ble_gatt_disc_fn_t cb, void* ctx)
{
  if (cb == NULL) {
    return k_ra_err_null_ptr;
  }
  if (s_pending_disc.in_use != 0U) {
    return k_ra_err_busy;
  }
  s_pending_disc.in_use      = 1U;
  s_pending_disc.conn_handle = conn_handle;
  s_pending_disc.disc_cb     = cb;
  s_pending_disc.ctx         = ctx;
#ifdef RA_TARGET_BUILD
  int rc = ble_gattc_disc_all_svcs(conn_handle, internal_disc_trampoline, NULL);
  /* cppcheck-suppress knownConditionTrueFalse
   * The weak fallback above returns 0 unconditionally; the strong upstream
   * symbol returns BLE_HS_E* on real failure. */
  if (rc != 0) {
    s_pending_disc.in_use = 0U;
    return k_ra_err_invalid_arg;
  }
#endif
  return k_ra_ok;
}

ra_err_t
ra_ble_gatt_read(uint16_t conn_handle, uint16_t value_handle, ra_ble_gatt_read_fn_t cb, void* ctx)
{
  if (cb == NULL) {
    return k_ra_err_null_ptr;
  }
  if (s_pending_read.in_use != 0U) {
    return k_ra_err_busy;
  }
  s_pending_read.in_use      = 1U;
  s_pending_read.conn_handle = conn_handle;
  s_pending_read.read_cb     = cb;
  s_pending_read.ctx         = ctx;
#ifdef RA_TARGET_BUILD
  int rc = ble_gattc_read(conn_handle, value_handle, internal_read_trampoline, NULL);
  /* cppcheck-suppress knownConditionTrueFalse
   * Weak fallback returns 0; strong upstream returns BLE_HS_E* on failure. */
  if (rc != 0) {
    s_pending_read.in_use = 0U;
    return k_ra_err_invalid_arg;
  }
#else
  (void)value_handle;
#endif
  return k_ra_ok;
}

ra_err_t ra_ble_gatt_write(uint16_t               conn_handle,
                           uint16_t               value_handle,
                           const uint8_t*         data,
                           uint16_t               len,
                           uint8_t                response,
                           ra_ble_gatt_write_fn_t cb,
                           void*                  ctx)
{
  if (data == NULL && len > 0U) {
    return k_ra_err_null_ptr;
  }
  if (len > (uint16_t)k_ra_ble_gatt_client_max_write_bytes) {
    return k_ra_err_invalid_arg;
  }
  if (response != 0U) {
    if (s_pending_write.in_use != 0U) {
      return k_ra_err_busy;
    }
    s_pending_write.in_use      = 1U;
    s_pending_write.conn_handle = conn_handle;
    s_pending_write.write_cb    = cb;
    s_pending_write.ctx         = ctx;
#ifdef RA_TARGET_BUILD
    int rc = ble_gattc_write_flat(conn_handle, value_handle, data, len,
                                  internal_write_trampoline, NULL);
    /* cppcheck-suppress knownConditionTrueFalse
     * Weak fallback returns 0; strong upstream returns BLE_HS_E* on failure. */
    if (rc != 0) {
      s_pending_write.in_use = 0U;
      return k_ra_err_invalid_arg;
    }
#endif
  } else {
#ifdef RA_TARGET_BUILD
    int rc = ble_gattc_write_no_rsp_flat(conn_handle, value_handle, data, len);
    /* cppcheck-suppress knownConditionTrueFalse
     * Weak fallback returns 0; strong upstream returns BLE_HS_E* on failure. */
    if (rc != 0) {
      return k_ra_err_invalid_arg;
    }
#endif
  }
#ifndef RA_TARGET_BUILD
  (void)conn_handle;
  (void)value_handle;
  (void)data;
  (void)cb;
  (void)ctx;
#endif
  return k_ra_ok;
}

ra_err_t ra_ble_gatt_subscribe(uint16_t                conn_handle,
                               uint16_t                cccd_handle,
                               uint8_t                 enable_notify,
                               uint8_t                 enable_indicate,
                               ra_ble_gatt_notify_fn_t notify_cb,
                               void*                   ctx)
{
  if (enable_notify == 0U && enable_indicate == 0U && notify_cb == NULL) {
    return k_ra_err_invalid_arg;
  }

  /* Find or allocate a subscription slot. */
  ra_ble_gatt_client_sub_t* slot = NULL;
  for (uint8_t i = 0U; i < (uint8_t)k_ra_gatt_client_max_subs; i++) {
    if (s_subs[i].in_use != 0U && s_subs[i].conn_handle == conn_handle &&
        s_subs[i].cccd_handle == cccd_handle) {
      slot = &s_subs[i];
      break;
    }
  }
  if (slot == NULL) {
    for (uint8_t i = 0U; i < (uint8_t)k_ra_gatt_client_max_subs; i++) {
      if (s_subs[i].in_use == 0U) {
        slot = &s_subs[i];
        break;
      }
    }
  }
  if (slot == NULL) {
    return k_ra_err_no_mem;
  }
  slot->in_use      = 1U;
  slot->conn_handle = conn_handle;
  slot->cccd_handle = cccd_handle;
  slot->notify_cb   = notify_cb;
  slot->ctx         = ctx;

  uint16_t cccd_value = 0U;
  if (enable_notify != 0U) {
    cccd_value |= 0x0001U;
  }
  if (enable_indicate != 0U) {
    cccd_value |= 0x0002U;
  }

  uint8_t cccd_bytes[2];
  cccd_bytes[0] = (uint8_t)(cccd_value & 0xFFU);
  cccd_bytes[1] = (uint8_t)((cccd_value >> 8) & 0xFFU);

  return ra_ble_gatt_write(conn_handle,
                           cccd_handle,
                           cccd_bytes,
                           sizeof(cccd_bytes),
                           1U,
                           NULL,
                           NULL);
}

#ifdef UNIT_TEST
/* Test-hook prototypes (external linkage). */
void     ra_ble_gatt_client_test_inject_notify(uint16_t       conn_handle,
                                               uint16_t       attr_handle,
                                               const uint8_t* data,
                                               uint16_t       len);
uint32_t ra_ble_gatt_client_test_pending_count(void);
void     ra_ble_gatt_client_test_reset(void);

/**
 * @brief Test hook -- inject an HVN/HVI on a registered subscription.
 *
 * @since 0.1.0
 */
void ra_ble_gatt_client_test_inject_notify(uint16_t       conn_handle,
                                           uint16_t       attr_handle,
                                           const uint8_t* data,
                                           uint16_t       len)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_gatt_client_max_subs; i++) {
    if (s_subs[i].in_use != 0U && s_subs[i].conn_handle == conn_handle &&
        s_subs[i].notify_cb != NULL) {
      s_subs[i].notify_cb(s_subs[i].ctx, attr_handle, data, len);
    }
  }
}

/**
 * @brief Test hook -- count of in-flight pending requests.
 *
 * @since 0.1.0
 */
uint32_t ra_ble_gatt_client_test_pending_count(void)
{
  uint32_t n = 0U;
  if (s_pending_disc.in_use != 0U) {
    n++;
  }
  if (s_pending_read.in_use != 0U) {
    n++;
  }
  if (s_pending_write.in_use != 0U) {
    n++;
  }
  return n;
}

/**
 * @brief Test hook -- reset internal state for fixture reset.
 *
 * @since 0.1.0
 */
void ra_ble_gatt_client_test_reset(void)
{
  memset(&s_pending_disc, 0, sizeof(s_pending_disc));
  memset(&s_pending_read, 0, sizeof(s_pending_read));
  memset(&s_pending_write, 0, sizeof(s_pending_write));
  memset(s_subs, 0, sizeof(s_subs));
}
#endif /* UNIT_TEST */

// NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity)
