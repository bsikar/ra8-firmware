/**
 * @file ra8_ble_gatt_client.c
 * @brief GATT client wrapper implementation.
 *
 * @par Tag
 * [Ring 5 / LIB] {World: NS}
 *
 * @details
 * Public API documented in ra8_ble_gatt_client.h.
 *
 * Two build modes:
 *   - Target build: ``ble_gattc_*`` from NimBLE handles the actual
 *     PDUs.
 *   - Host unit-test build (``RA8_SIMULATOR_MODE``): the wrapper keeps
 *     bookkeeping but never sends anything. Tests can drive the
 *     callbacks via the ``UNIT_TEST`` hooks at the bottom of the TU.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_ble_gatt_client.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

#ifdef RA8_TARGET_BUILD
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"

/*
 * Weak fallbacks to keep this TU linkable even before the NimBLE host
 * library has been brought into the per-app build. The strong upstream
 * symbols override these once the host stack is wired in.
 */
/**
 * @brief Weak fallback for NimBLE GATT-client "discover all services" call.
 *
 * @details No-op stub returning success; overridden by the real NimBLE
 *          implementation when the host stack is linked in.
 *
 * @param[in,out] conn_handle BLE connection handle (ignored).
 * @param[in,out] cb          Discovery callback (ignored).
 * @param[in,out] cb_arg      Opaque pointer forwarded to cb (ignored).
 *
 * @return 0 always (stub).
 * @retval 0 Stub success.
 *
 * @pre TU is being built without the real NimBLE host.
 * @pre Caller does not depend on actual discovery happening.
 * @post No state modified.
 * @post Returned value is 0.
 *
 * @note Weak symbol; superseded by real NimBLE host when present.
 *
 * @since 0.1.0
 */
[[gnu::weak]] int
ble_gattc_disc_all_svcs(uint16_t conn_handle, ble_gatt_disc_svc_fn* cb, void* cb_arg)
{
  (void)conn_handle;
  (void)cb;
  (void)cb_arg;
  return 0;
}

/**
 * @brief Weak fallback for NimBLE GATT-client "read attribute" call.
 *
 * @details No-op stub returning success; overridden by the real NimBLE
 *          implementation when the host stack is linked in.
 *
 * @param[in,out] conn_handle BLE connection handle (ignored).
 * @param[in,out] attr_handle Attribute handle to read (ignored).
 * @param[in,out] cb          Read-result callback (ignored).
 * @param[in,out] cb_arg      Opaque pointer forwarded to cb (ignored).
 *
 * @return 0 always (stub).
 * @retval 0 Stub success.
 *
 * @pre TU is being built without the real NimBLE host.
 * @pre Caller does not depend on actual read happening.
 * @post No state modified.
 * @post Returned value is 0.
 *
 * @note Weak symbol; superseded by real NimBLE host when present.
 *
 * @since 0.1.0
 */
[[gnu::weak]] int
ble_gattc_read(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_attr_fn* cb, void* cb_arg)
{
  (void)conn_handle;
  (void)attr_handle;
  (void)cb;
  (void)cb_arg;
  return 0;
}

[[gnu::weak]] int ble_gattc_write_flat(uint16_t          conn_handle,
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

[[gnu::weak]] int ble_gattc_write_no_rsp_flat(uint16_t    conn_handle,
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

/**
 * @brief Weak fallback for NimBLE "mbuf to flat buffer" copy.
 *
 * @details No-op stub returning success and writing zero bytes copied;
 *          overridden by real NimBLE host when present.
 *
 * @param[in,out] om           Source mbuf chain (ignored).
 * @param[in,out] flat         Destination flat buffer (ignored).
 * @param[in,out] max_len      Capacity of flat buffer (ignored).
 * @param[in,out] out_copy_len Output: bytes copied. Set to 0 when non-NULL.
 *
 * @return 0 always (stub).
 * @retval 0 Stub success.
 *
 * @pre TU is being built without the real NimBLE host.
 * @pre out_copy_len may be NULL; if non-NULL it is writable.
 * @post out_copy_len (if non-NULL) is set to 0.
 * @post No mbuf state modified.
 *
 * @note Weak symbol; superseded by real NimBLE host when present.
 *
 * @since 0.1.0
 */
[[gnu::weak]] int
ble_hs_mbuf_to_flat(const struct os_mbuf* om, void* flat, uint16_t max_len, uint16_t* out_copy_len)
{
  (void)om;
  (void)flat;
  (void)max_len;
  if (out_copy_len != nullptr) {
    *out_copy_len = 0U;
  }
  return 0;
}
#endif

/* ============================================================ */
/* Internal state */
/* ============================================================ */

/**
 * @struct ra8_ble_gatt_client_pending_t
 * @brief Per-request bookkeeping (one in flight at a time).
 */
typedef struct {
  uint8_t                 in_use;      /**< In use.      */
  uint16_t                conn_handle; /**< Conn handle. */
  ra8_ble_gatt_disc_fn_t  disc_cb;     /**< Disc cb.     */
  ra8_ble_gatt_read_fn_t  read_cb;     /**< Read cb.     */
  ra8_ble_gatt_write_fn_t write_cb;    /**< Write cb.    */
  void*                   ctx;         /**< Ctx.         */
} ra8_ble_gatt_client_pending_t;

/**
 * @struct ra8_ble_gatt_client_sub_t
 * @brief Tracked notification subscription.
 */
typedef struct {
  uint8_t                  in_use;      /**< In use.      */
  uint16_t                 conn_handle; /**< Conn handle. */
  uint16_t                 cccd_handle; /**< Cccd handle. */
  ra8_ble_gatt_notify_fn_t notify_cb;   /**< Notify cb.   */
  void*                    ctx;         /**< Ctx.         */
} ra8_ble_gatt_client_sub_t;

/** @brief Little-endian byte packing for 16/32-bit UUIDs and CCCD. */
typedef enum : uint32_t {
  k_ble_byte_mask   = 0xFFU, /**< Low-byte mask.                             */
  k_ble_shift_byte3 = 24U,   /**< Shift to byte 3 (8/16 are ignored values). */
} ble_gattc_pack_t;

typedef enum : uint8_t {
  k_ra8_gatt_client_max_subs = 4U, /**< RA8 GATT client maximum subs. */
} ra8_ble_gatt_client_internal_t;

/**
 * @var s_pending_disc
 * @brief Discovery operation in flight (singleton).
 */
static ra8_ble_gatt_client_pending_t s_pending_disc;

/**
 * @var s_pending_read
 * @brief Read operation in flight (singleton).
 */
static ra8_ble_gatt_client_pending_t s_pending_read;

/**
 * @var s_pending_write
 * @brief Write operation in flight (singleton).
 */
static ra8_ble_gatt_client_pending_t s_pending_write;

/**
 * @var s_subs
 * @brief Subscription table.
 */
static ra8_ble_gatt_client_sub_t s_subs[k_ra8_gatt_client_max_subs];

/* ============================================================ */
/* Helpers */
/* ============================================================ */

#ifdef RA8_TARGET_BUILD
/**
 * @brief Trampoline mapping NimBLE's discovery callback onto our API.
 *
 * @details Bluetooth Core 5.3 Vol 3 Part G 4.4 "Primary Service
 *          Discovery" -- this trampoline shapes NimBLE's per-row /
 *          completion callback into ra8_ble_gatt_disc_fn_t.
 *
 * @param[in] conn_handle ACL handle.
 * @param[in] error       NimBLE error struct (NULL on per-row).
 * @param[in] service     Discovered service row, or NULL on completion.
 * @param[in] arg         User-provided context (unused; we use s_pending_disc).
 *
 * @return int 0 to continue.
 * @retval 0  Always returned (NimBLE convention to keep walking).
 *
 * @pre s_pending_disc.in_use == 1.
 * @pre Caller is the NimBLE host task.
 * @post On final invocation s_pending_disc.in_use is cleared.
 * @post User callback (if registered) has been invoked.
 *
 * @note Not thread-safe; called only from the NimBLE host task.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_disc_trampoline(uint16_t                     conn_handle,
                                    const struct ble_gatt_error* error,
                                    const struct ble_gatt_svc*   service,
                                    void*                        arg)
{
  (void)conn_handle;
  (void)arg;
  ra8_ble_gatt_disc_fn_t cb     = s_pending_disc.disc_cb;
  void*                  ctx    = s_pending_disc.ctx;
  uint16_t               status = (error != nullptr) ? (uint16_t)error->status : (uint16_t)0U;
  if (service != nullptr && cb != nullptr) {
    ra8_ble_gatt_service_t row = {};
    row.start_handle           = service->start_handle;
    row.end_handle             = service->end_handle;
    /* Copy whichever UUID form NimBLE supplied, zero-padding to 128. */
    const ble_uuid_t* u = &service->uuid.u;
    if (u->type == BLE_UUID_TYPE_16) {
      uint16_t v      = ((const ble_uuid16_t*)u)->value;
      row.uuid_128[0] = (uint8_t)(v & k_ble_byte_mask);
      row.uuid_128[1] = (uint8_t)((v >> 8) & k_ble_byte_mask);
    } else if (u->type == BLE_UUID_TYPE_32) {
      uint32_t v      = ((const ble_uuid32_t*)u)->value;
      row.uuid_128[0] = (uint8_t)(v & k_ble_byte_mask);
      row.uuid_128[1] = (uint8_t)((v >> 8) & k_ble_byte_mask);
      row.uuid_128[2] = (uint8_t)((v >> 16) & k_ble_byte_mask);
      row.uuid_128[3] = (uint8_t)((v >> k_ble_shift_byte3) & k_ble_byte_mask);
    } else {
      memcpy(row.uuid_128, ((const ble_uuid128_t*)u)->value, sizeof(row.uuid_128));
    }
    cb(ctx, &row, status);
    return 0;
  }
  /* Final completion. */
  s_pending_disc.in_use = 0U;
  if (cb != nullptr) {
    cb(ctx, nullptr, status);
  }
  return 0;
}

/**
 * @brief Trampoline mapping NimBLE's read callback onto our API.
 *
 * @details Bluetooth Core 5.3 Vol 3 Part F 3.4.4.3 Read_Request
 *          completion path -- shapes NimBLE's mbuf into a flat byte
 *          buffer for the user callback.
 *
 * @param[in] conn_handle ACL handle (unused; tracked in s_pending_read).
 * @param[in] error       NimBLE error struct, NULL on success.
 * @param[in] attr        Attribute value mbuf carrier, may be NULL.
 * @param[in] arg         User context (unused).
 *
 * @return int 0 to continue (NimBLE convention).
 * @retval 0  Always returned.
 *
 * @pre s_pending_read.in_use == 1.
 * @pre Caller is the NimBLE host task.
 * @post s_pending_read.in_use is cleared.
 * @post User callback (if registered) has been invoked exactly once.
 *
 * @note Not thread-safe; called only from the NimBLE host task.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_read_trampoline(uint16_t                     conn_handle,
                                    const struct ble_gatt_error* error,
                                    struct ble_gatt_attr*        attr,
                                    void*                        arg)
{
  (void)conn_handle;
  (void)arg;
  ra8_ble_gatt_read_fn_t cb     = s_pending_read.read_cb;
  void*                  ctx    = s_pending_read.ctx;
  uint16_t               status = (error != nullptr) ? (uint16_t)error->status : (uint16_t)0U;
  s_pending_read.in_use         = 0U;
  if (cb != nullptr) {
    if (attr != nullptr && attr->om != nullptr) {
      uint16_t len = OS_MBUF_PKTLEN(attr->om);
      if (len > (uint16_t)k_ra8_ble_gatt_client_max_read_bytes) {
        len = (uint16_t)k_ra8_ble_gatt_client_max_read_bytes;
      }
      uint8_t  buf[k_ra8_ble_gatt_client_max_read_bytes];
      uint16_t out_len = 0U;
      int      mc      = ble_hs_mbuf_to_flat(attr->om, buf, len, &out_len);
      /* cppcheck-suppress knownConditionTrueFalse -- the in-tree weak fallback returns 0; the strong upstream NimBLE symbol returns non-zero on failure. */
      cb(ctx, buf, (mc == 0) ? out_len : 0U, status);
    } else {
      cb(ctx, nullptr, 0U, status);
    }
  }
  return 0;
}

/**
 * @brief Trampoline mapping NimBLE's write completion onto our API.
 *
 * @details Bluetooth Core 5.3 Vol 3 Part F 3.4.5.1 Write_Request
 *          completion path -- forwards the status code into
 *          ra8_ble_gatt_write_fn_t.
 *
 * @param[in] conn_handle ACL handle (unused; tracked in s_pending_write).
 * @param[in] error       NimBLE error struct, NULL on success.
 * @param[in] attr        Attribute mbuf (unused on write completion).
 * @param[in] arg         User context (unused).
 *
 * @return int 0 to continue (NimBLE convention).
 * @retval 0  Always returned.
 *
 * @pre s_pending_write.in_use == 1.
 * @pre Caller is the NimBLE host task.
 * @post s_pending_write.in_use is cleared.
 * @post User callback (if registered) has been invoked exactly once.
 *
 * @note Not thread-safe; called only from the NimBLE host task.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_write_trampoline(uint16_t                     conn_handle,
                                     const struct ble_gatt_error* error,
                                     struct ble_gatt_attr*        attr,
                                     void*                        arg)
{
  (void)conn_handle;
  (void)attr;
  (void)arg;
  ra8_ble_gatt_write_fn_t cb     = s_pending_write.write_cb;
  void*                   ctx    = s_pending_write.ctx;
  uint16_t                status = (error != nullptr) ? (uint16_t)error->status : (uint16_t)0U;
  s_pending_write.in_use         = 0U;
  if (cb != nullptr) {
    cb(ctx, status);
  }
  return 0;
}
#endif /* RA8_TARGET_BUILD */

/* ============================================================ */
/* Public API */
/* ============================================================ */

/**
 * @brief Issue a "Discover All Primary Services" GATT procedure.
 *
 * @details Bluetooth Core 5.3 Vol 3 Part G 4.4.1. Drives the upstream
 *          NimBLE ble_gattc_disc_all_svcs and forwards each row plus
 *          a final completion to the user callback. Only one
 *          discovery may be in flight at a time.
 *
 * @param[in] conn_handle ACL handle of the active connection.
 * @param[in] cb          User completion callback (must not be NULL).
 * @param[in] ctx         Opaque user pointer forwarded to cb.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Discovery launched.
 * @retval k_ra8_err_null_ptr     cb is NULL.
 * @retval k_ra8_err_busy         Another discovery is already in flight.
 * @retval k_ra8_err_invalid_arg  NimBLE rejected the request.
 *
 * @pre A live connection identified by conn_handle exists.
 * @pre s_pending_disc.in_use == 0.
 * @post On success s_pending_disc.in_use == 1 until completion.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_gatt_discover_services(uint16_t conn_handle, ra8_ble_gatt_disc_fn_t cb, void* ctx)
{
  if (cb == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (s_pending_disc.in_use != 0U) {
    return k_ra8_err_busy;
  }
  s_pending_disc.in_use      = 1U;
  s_pending_disc.conn_handle = conn_handle;
  s_pending_disc.disc_cb     = cb;
  s_pending_disc.ctx         = ctx;
#ifdef RA8_TARGET_BUILD
  int rc = ble_gattc_disc_all_svcs(conn_handle, internal_disc_trampoline, nullptr);
  /* cppcheck-suppress knownConditionTrueFalse -- the in-tree weak fallback returns 0; the strong upstream NimBLE symbol returns non-zero on failure. */
  if (rc != 0) {
    s_pending_disc.in_use = 0U;
    return k_ra8_err_invalid_arg;
  }
#endif
  return k_ra8_ok;
}

/**
 * @brief Issue a GATT Read Characteristic Value procedure.
 *
 * @details Bluetooth Core 5.3 Vol 3 Part G 4.8.1. Drives NimBLE's
 *          ble_gattc_read; the result is delivered asynchronously
 *          through the supplied callback. One read at a time per
 *          stack instance.
 *
 * @param[in] conn_handle  ACL handle.
 * @param[in] value_handle Attribute (value) handle to read.
 * @param[in] cb           User completion callback (must not be NULL).
 * @param[in] ctx          Opaque user pointer forwarded to cb.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Read launched.
 * @retval k_ra8_err_null_ptr     cb is NULL.
 * @retval k_ra8_err_busy         Another read is already in flight.
 * @retval k_ra8_err_invalid_arg  NimBLE rejected the request.
 *
 * @pre A live connection identified by conn_handle exists.
 * @pre s_pending_read.in_use == 0.
 * @post On success s_pending_read.in_use == 1 until completion.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
ra8_err_t
ra8_ble_gatt_read(uint16_t conn_handle, uint16_t value_handle, ra8_ble_gatt_read_fn_t cb, void* ctx)
{
  if (cb == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (s_pending_read.in_use != 0U) {
    return k_ra8_err_busy;
  }
  s_pending_read.in_use      = 1U;
  s_pending_read.conn_handle = conn_handle;
  s_pending_read.read_cb     = cb;
  s_pending_read.ctx         = ctx;
#ifdef RA8_TARGET_BUILD
  int rc = ble_gattc_read(conn_handle, value_handle, internal_read_trampoline, nullptr);
  /* cppcheck-suppress knownConditionTrueFalse -- the in-tree weak fallback returns 0; the strong upstream NimBLE symbol returns non-zero on failure. */
  if (rc != 0) {
    s_pending_read.in_use = 0U;
    return k_ra8_err_invalid_arg;
  }
#else
  (void)value_handle;
#endif
  return k_ra8_ok;
}

/**
 * @brief Issue a GATT Write Characteristic Value procedure.
 *
 * @details Bluetooth Core 5.3 Vol 3 Part G 4.9. Selects between
 *          Write_Request (response != 0, Vol 3 Part F 3.4.5.1) and
 *          Write_Command (response == 0, Vol 3 Part F 3.4.5.3) based
 *          on the response argument.
 *
 * @param[in] conn_handle  ACL handle.
 * @param[in] value_handle Attribute (value) handle to write.
 * @param[in] data         Bytes to write (may be NULL when len == 0).
 * @param[in] len          Number of bytes in data.
 * @param[in] response     0 to send Write Without Response, non-zero
 *                         to issue a Write Request.
 * @param[in] cb           Completion callback (only used when
 *                         response != 0; may be NULL).
 * @param[in] ctx          Opaque user pointer forwarded to cb.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Write launched.
 * @retval k_ra8_err_null_ptr     data is NULL while len > 0.
 * @retval k_ra8_err_invalid_arg  len exceeds the per-write cap or NimBLE rejected.
 * @retval k_ra8_err_busy         A pending Write_Request is already in flight.
 *
 * @pre A live connection identified by conn_handle exists.
 * @pre When response != 0, s_pending_write.in_use == 0.
 * @post On success the write has been queued; for Write_Request
 *       s_pending_write.in_use == 1 until completion.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_gatt_write(uint16_t                conn_handle,
                             uint16_t                value_handle,
                             const uint8_t*          data,
                             uint16_t                len,
                             uint8_t                 response,
                             ra8_ble_gatt_write_fn_t cb,
                             void*                   ctx)
{
  if (data == nullptr && len > 0U) {
    return k_ra8_err_null_ptr;
  }
  if (len > (uint16_t)k_ra8_ble_gatt_client_max_write_bytes) {
    return k_ra8_err_invalid_arg;
  }
  if (response != 0U) {
    if (s_pending_write.in_use != 0U) {
      return k_ra8_err_busy;
    }
    s_pending_write.in_use      = 1U;
    s_pending_write.conn_handle = conn_handle;
    s_pending_write.write_cb    = cb;
    s_pending_write.ctx         = ctx;
#ifdef RA8_TARGET_BUILD
    int rc = ble_gattc_write_flat(conn_handle,
                                  value_handle,
                                  data,
                                  len,
                                  internal_write_trampoline,
                                  nullptr);
    /* cppcheck-suppress knownConditionTrueFalse -- the in-tree weak fallback returns 0; the strong upstream NimBLE symbol returns non-zero on failure. */
    if (rc != 0) {
      s_pending_write.in_use = 0U;
      return k_ra8_err_invalid_arg;
    }
#endif
  } else {
#ifdef RA8_TARGET_BUILD
    int rc = ble_gattc_write_no_rsp_flat(conn_handle, value_handle, data, len);
    /* cppcheck-suppress knownConditionTrueFalse -- the in-tree weak fallback returns 0; the strong upstream NimBLE symbol returns non-zero on failure. */
    if (rc != 0) {
      return k_ra8_err_invalid_arg;
    }
#endif
  }
#ifndef RA8_TARGET_BUILD
  (void)conn_handle;
  (void)value_handle;
  (void)data;
  (void)cb;
  (void)ctx;
#endif
  return k_ra8_ok;
}

/**
 * @brief Find or allocate a subscription-table slot for (conn, cccd).
 *
 * @details First scans the table for a live slot already tracking the
 *          same (conn_handle, cccd_handle) pair so a re-subscribe
 *          reuses its row; otherwise falls back to the first free
 *          slot. Mirrors the CCCD-per-client bookkeeping model of
 *          Bluetooth Core 5.3 Vol 3 Part G 3.3.3.3.
 *
 * @param[in] conn_handle ACL handle the subscription belongs to.
 * @param[in] cccd_handle Attribute handle of the peer's CCCD.
 *
 * @return Pointer to the matching or freshly allocated slot.
 * @retval NULL  Table is full and no matching slot exists.
 * @retval !NULL Slot inside s_subs (existing match preferred).
 *
 * @pre Caller is the application thread (single-threaded access).
 * @pre s_subs is the singleton subscription table of this TU.
 * @post No slot fields are mutated (allocation is by the caller).
 * @post Returned pointer (if non-NULL) points into s_subs.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_ble_gatt_client_sub_t* internal_sub_slot(uint16_t conn_handle, uint16_t cccd_handle)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_gatt_client_max_subs; i++) {
    if (s_subs[i].in_use != 0U && s_subs[i].conn_handle == conn_handle &&
        s_subs[i].cccd_handle == cccd_handle) {
      return &s_subs[i];
    }
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_gatt_client_max_subs; i++) {
    if (s_subs[i].in_use == 0U) {
      return &s_subs[i];
    }
  }
  return nullptr;
}

/**
 * @brief Subscribe / unsubscribe to characteristic notifications and
 *        indications by writing the peer CCCD.
 *
 * @details Bluetooth Core 5.3 Vol 3 Part G 3.3.3.3 "Client
 *          Characteristic Configuration" -- bit 0 enables
 *          notifications, bit 1 enables indications. The CCCD value
 *          is written via Write_Request and the local subscription
 *          table is updated so subsequent HVN/HVI PDUs route to
 *          notify_cb.
 *
 * @param[in] conn_handle      ACL handle.
 * @param[in] cccd_handle      Attribute handle of the peer's CCCD.
 * @param[in] enable_notify    Non-zero to set the notify bit.
 * @param[in] enable_indicate  Non-zero to set the indicate bit.
 * @param[in] notify_cb        Inbound HVN/HVI callback (may be NULL
 *                             only when both enables are 0).
 * @param[in] ctx              Opaque user pointer forwarded to notify_cb.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Subscription updated.
 * @retval k_ra8_err_invalid_arg  Both enables are 0 with NULL callback.
 * @retval k_ra8_err_no_mem       Subscription table is full.
 * @retval other                 Error from the underlying CCCD write.
 *
 * @pre A live connection identified by conn_handle exists.
 * @pre cccd_handle was discovered via ra8_ble_gatt_discover_services.
 * @post On success the local subscription table holds the slot and
 *       a CCCD Write_Request has been queued.
 * @post On failure the subscription table may have an allocated slot
 *       that is reused on retry (no leak).
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_gatt_subscribe(uint16_t                 conn_handle,
                                 uint16_t                 cccd_handle,
                                 uint8_t                  enable_notify,
                                 uint8_t                  enable_indicate,
                                 ra8_ble_gatt_notify_fn_t notify_cb,
                                 void*                    ctx)
{
  if (enable_notify == 0U && enable_indicate == 0U && notify_cb == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  /* Find or allocate a subscription slot. */
  ra8_ble_gatt_client_sub_t* slot = internal_sub_slot(conn_handle, cccd_handle);
  if (slot == nullptr) {
    return k_ra8_err_no_mem;
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
  cccd_bytes[0] = (uint8_t)(cccd_value & k_ble_byte_mask);
  cccd_bytes[1] = (uint8_t)((cccd_value >> 8) & k_ble_byte_mask);

  return ra8_ble_gatt_write(conn_handle,
                            cccd_handle,
                            cccd_bytes,
                            sizeof(cccd_bytes),
                            1U,
                            nullptr,
                            nullptr);
}

#ifdef UNIT_TEST
/* Test-hook prototypes (external linkage). */
RA8_TEST_HELPER void     ra8_ble_gatt_client_test_inject_notify(uint16_t       conn_handle,
                                                                uint16_t       attr_handle,
                                                                const uint8_t* data,
                                                                uint16_t       len);
RA8_TEST_HELPER uint32_t ra8_ble_gatt_client_test_pending_count(void);
RA8_TEST_HELPER void     ra8_ble_gatt_client_test_reset(void);

/**
 * @brief Test hook -- inject an HVN/HVI on a registered subscription.
 *
 * @details Walks the subscription table and invokes the matching
 *          notify_cb directly, simulating an inbound Bluetooth Core
 *          5.3 Vol 3 Part F 3.4.7 Handle Value Notification /
 *          Indication.
 *
 * @param[in] conn_handle ACL handle to attribute the event to.
 * @param[in] attr_handle Value attribute handle that "fired".
 * @param[in] data        Notification payload bytes.
 * @param[in] len         Number of bytes in data.
 *
 * @pre A subscription was previously installed via
 *      ra8_ble_gatt_subscribe.
 * @pre Caller is the unit-test harness (single-threaded).
 * @post Matching notify callbacks have been invoked.
 * @post No internal table state is mutated.
 *
 * @note Not thread-safe; for unit-test harness use only.
 *
 * @since 0.1.0
 */
void ra8_ble_gatt_client_test_inject_notify(uint16_t       conn_handle,
                                            uint16_t       attr_handle,
                                            const uint8_t* data,
                                            uint16_t       len)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_gatt_client_max_subs; i++) {
    if (s_subs[i].in_use != 0U && s_subs[i].conn_handle == conn_handle &&
        s_subs[i].notify_cb != nullptr) {
      s_subs[i].notify_cb(s_subs[i].ctx, attr_handle, data, len);
    }
  }
}

/**
 * @brief Test hook -- count of in-flight pending requests.
 *
 * @details Returns the number of disc/read/write singletons currently
 *          marked in_use.
 *
 * @return uint32_t Number of in-flight operations (0..3).
 * @retval 0  No operations are pending.
 * @retval >0 At least one disc/read/write is pending.
 *
 * @pre Caller is the unit-test harness (single-threaded).
 * @pre None on the global state (function is read-only).
 * @post No state is mutated.
 * @post Return value reflects the snapshot at call time.
 *
 * @note Not thread-safe; for unit-test harness use only.
 *
 * @since 0.1.0
 */
uint32_t ra8_ble_gatt_client_test_pending_count(void)
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
 * @details Zeroes the disc/read/write pending singletons and the
 *          subscription table so subsequent test fixtures start from
 *          a clean slate.
 *
 * @pre Caller is the unit-test harness (single-threaded).
 * @pre No upstream NimBLE callbacks are racing.
 * @post All pending operations are cleared.
 * @post All subscriptions are cleared.
 *
 * @note Not thread-safe; for unit-test harness use only.
 *
 * @since 0.1.0
 */
void ra8_ble_gatt_client_test_reset(void)
{
  memset(&s_pending_disc, 0, sizeof(s_pending_disc));
  memset(&s_pending_read, 0, sizeof(s_pending_read));
  memset(&s_pending_write, 0, sizeof(s_pending_write));
  memset(s_subs, 0, sizeof(s_subs));
}
#endif /* UNIT_TEST */
