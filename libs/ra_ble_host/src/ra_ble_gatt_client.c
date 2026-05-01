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

/*
 * Direct NimBLE host calls would live behind ``ra_ble_gatt_client_target.c``
 * which wires this layer to ble_gattc_*. That target-only TU is not
 * present yet -- this file is portable and compiles into both the host
 * unit-test build and the cross-compiled target build with the same
 * source. Future work: split the trampolines into the target adapter.
 */

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

/*
 * The trampolines that translate between NimBLE's
 * ``ble_gatt_svc`` / ``ble_gatt_attr`` callback signatures and our
 * ra_err_t-style callbacks live in a separate target-only TU
 * (libs/ra_ble_host/src/ra_ble_gatt_client_target.c) which is added
 * by a future patch. For now both the host unit-test build and the
 * cross-compiled target build only exercise the bookkeeping surface
 * defined below; tests use the UNIT_TEST hooks at the bottom of the
 * file to drive completions synthetically.
 */

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
  (void)conn_handle;
  (void)value_handle;
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
  }
  (void)conn_handle;
  (void)value_handle;
  (void)data;
  (void)cb;
  (void)ctx;
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
