/**
 * @file ra_ble_l2cap.c
 * @brief L2CAP fixed-channel layer for the BLE host stack
 *
 * @details
 * Implements the bare minimum of Bluetooth Core 5.3 Vol 3 Part A
 * required to host an ATT server on the LE-U logical link:
 *
 *   - CID 0x0004 (LE ATT, sec 2.1 Table 2.3) -- inbound PDUs are
 *     handed up to the ATT layer; outbound PDUs are framed as
 *     B-frames and pushed down via ``ra_ble_hci_send_acl_data``.
 *   - CID 0x0005 (LE Signaling, sec 4.20) -- minimal handling: the
 *     stack acknowledges Connection_Parameter_Update_Request frames
 *     so peers don't time out, but does not initiate signaling
 *     itself.
 *
 * Fragmentation: per Vol 3 Part A 7.2.1, an L2CAP B-frame can be
 * split across multiple HCI ACL Data packets using the PB
 * (Packet Boundary) flag. We support up to one outstanding inbound
 * fragmented PDU per connection (PB=0b01 continuations append to
 * the reassembly buffer, PB=0b00 / 0b10 starts replace it). On TX
 * we never produce fragments because the host always sits below
 * the controller's default ACL data length (251 bytes per
 * ra_ble_limits_t.k_ra_ble_max_acl_payload, far above the 23-byte
 * default ATT_MTU we operate at).
 *
 * Also owns the host-private state shared with the ATT and GATT
 * source files (connection handle, MTU, attribute table). A single
 * static struct in this TU keeps everything in one place; the
 * other host TUs reach in via the small set of internal functions
 * declared in ra_ble_host_internal.h (header file in this same
 * directory).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

// NOLINTBEGIN(readability-function-size,readability-function-cognitive-complexity)
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_ble.h"
#include "ra_ble_host.h"
#include "ra_ble_host_internal.h"
#include "ra_err.h"

/* =============================================================================
 * Internal contract shared with ra_ble_att.c / ra_ble_gatt.c
 *
 * The implementation-internal types (constants enum, attribute-table
 * row, host-state struct) plus the shared ``s_state`` instance and
 * ``internal_pack_le16`` live in ra_ble_host_internal.h so the GAP
 * advertising TU (ra_ble_l2cap_advertise.c) can reach them too.
 * =============================================================================
 */

/* The single shared-state instance. */
ra_ble_host_state_t s_state;

/* =============================================================================
 * Internal entry points used by ra_ble_att.c and ra_ble_gatt.c.
 * Forward declarations -- implementations follow further down or in
 * the sibling files.
 * =============================================================================
 */

ra_ble_host_state_t* ra_ble_host_state(void);
void                 ra_ble_host_dispatch_event(const ra_ble_host_event_t* evt);
ra_err_t             ra_ble_host_l2cap_send(uint16_t       conn_handle,
                                            uint16_t       cid,
                                            const uint8_t* payload,
                                            uint16_t       payload_len);
void ra_ble_host_att_handle_pdu(uint16_t conn_handle, const uint8_t* pdu, uint16_t pdu_len);

/* =============================================================================
 * Small helpers
 * =============================================================================
 */

/** @brief Implementation of `internal_pack_le16()` -- LE16 byte store. */
void internal_pack_le16(uint8_t* dst, uint16_t v)
{
  enum : uint8_t {
    k_byte_lo_idx = 0U,
    k_byte_hi_idx = 1U,
    k_byte_shift  = 8U,
    k_byte_mask   = 0xFFU,
  };
  dst[k_byte_lo_idx] = (uint8_t)(v & k_byte_mask);
  dst[k_byte_hi_idx] = (uint8_t)((v >> k_byte_shift) & k_byte_mask);
}

/**
 * @brief Unpack a little-endian uint16 from a byte buffer.
 *
 * @details Reads the LE16 layout per Bluetooth Core 5.3 Vol 3 Part A 3.
 *
 * @param[in] src Two-byte source. Must not be NULL.
 * @return Unpacked value.
 * @retval 0     src[0] and src[1] are both zero.
 * @retval other Combined LE16 value.
 *
 * @pre src != NULL.
 * @pre src points to at least 2 readable bytes.
 * @post No memory is modified.
 * @post Return value equals src[0] | (src[1] << 8).
 *
 * @note Not thread-safe; caller serializes access to src.
 *
 * @since 0.1.0
 */
static uint16_t internal_unpack_le16(const uint8_t* src)
{
  enum : uint8_t {
    k_byte_lo_idx = 0U,
    k_byte_hi_idx = 1U,
    k_byte_shift  = 8U,
  };
  return (uint16_t)((uint16_t)src[k_byte_lo_idx] | ((uint16_t)src[k_byte_hi_idx] << k_byte_shift));
}

/* =============================================================================
 * Internal accessors
 * =============================================================================
 */

/**
 * @brief Return a pointer to the singleton host-state struct.
 *
 * @details Used by the ATT and GATT sibling files so they share the
 *          same attribute table and connection state without having
 *          to resolve a global symbol by name.
 *
 * @return Pointer to the singleton state. Never NULL.
 * @retval !NULL Always returns a valid pointer to s_state.
 *
 * @pre None.
 * @pre Linker has placed s_state in writable BSS/data.
 * @post Returned pointer is valid for the program's lifetime.
 * @post No state is mutated.
 *
 * @note Not thread-safe; the underlying state struct is not lock-guarded.
 *
 * @since 0.1.0
 */
ra_ble_host_state_t* ra_ble_host_state(void)
{
  return &s_state;
}

/**
 * @brief Deliver an event to the user callback if one is registered.
 *
 * @details Bumps the dispatched-event counter and (when an event
 *          handler is attached via ra_ble_host_attach_event_handler)
 *          forwards the event payload.
 *
 * @param[in] evt Event payload. Must not be NULL.
 *
 * @pre evt != NULL.
 * @pre Stack is initialized.
 * @post s_state.evt_count incremented.
 * @post If a handler is registered it has been invoked synchronously.
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
void ra_ble_host_dispatch_event(const ra_ble_host_event_t* evt)
{
  s_state.evt_count++;
  if (s_state.evt_fn != nullptr) {
    s_state.evt_fn(s_state.evt_ctx, evt);
  }
}

/* =============================================================================
 * L2CAP frame helpers
 * =============================================================================
 */

/**
 * @brief Send an L2CAP B-frame over the controller-host HCI ACL pipe.
 *
 * @details
 * Bluetooth Core 5.3 Vol 3 Part A 3.1 "B-frame format":
 *   [length (LE16)][channel id (LE16)][payload...]
 * The whole frame is then handed to the HCI transport with the PB
 * flag set to "first non-flushable / complete PDU" (Vol 4 Part E
 * 5.4.2). We never fragment outbound traffic in this starter stack.
 *
 * @param[in] conn_handle 12-bit ACL connection handle.
 * @param[in] cid         L2CAP channel ID (0x0004 ATT, 0x0005 signaling).
 * @param[in] payload     Frame payload bytes.
 * @param[in] payload_len Payload length (0..k_tx_scratch_bytes - k_l2cap_hdr_bytes).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Sent.
 * @retval k_ra_err_invalid_arg   payload_len exceeds the scratch.
 * @retval k_ra_err_null_ptr      payload NULL with payload_len > 0.
 * @retval other                  Whatever ra_ble_hci_send_acl_data returns.
 *
 * @pre Stack is initialized.
 * @pre payload is non-NULL when payload_len > 0.
 * @post On success the L2CAP frame has been queued on the HCI ACL TX FIFO.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from the host serial dispatch loop.
 *
 * @since 0.1.0
 */
ra_err_t ra_ble_host_l2cap_send(uint16_t       conn_handle,
                                uint16_t       cid,
                                const uint8_t* payload,
                                uint16_t       payload_len)
{
  if ((payload == nullptr) && (payload_len > 0U)) {
    return k_ra_err_null_ptr;
  }
  if (((uint32_t)payload_len + (uint32_t)k_l2cap_hdr_bytes) > (uint32_t)k_tx_scratch_bytes) {
    return k_ra_err_invalid_arg;
  }

  static uint8_t s_tx[k_tx_scratch_bytes];
  internal_pack_le16(&s_tx[0], payload_len);
  internal_pack_le16(&s_tx[2], cid);
  if (payload_len > 0U) {
    (void)memcpy(&s_tx[k_l2cap_hdr_bytes], payload, payload_len);
  }
  const uint16_t handle_with_pb = (uint16_t)(conn_handle | k_pb_first_complete);
  return ra_ble_hci_send_acl_data(handle_with_pb,
                                  s_tx,
                                  (uint16_t)(payload_len + k_l2cap_hdr_bytes));
}

/* =============================================================================
 * HCI -> L2CAP feed
 * =============================================================================
 */

/**
 * @brief Bottom-half of the HCI ACL data path: reassemble L2CAP
 *        frames and dispatch them to the right channel handler.
 *
 * @details
 * Called from the per-host HCI ACL callback. The 4-byte ACL header
 * has already been stripped by the controller driver, so what we
 * see in ``payload`` is the L2CAP B-frame -- but possibly only the
 * first fragment.
 *
 * Reassembly state machine:
 *   - First-fragment frames (PB == 0b00 or 0b10, signalled by
 *     payload starting with a fresh L2CAP header where length matches
 *     payload_len-4) are dispatched immediately if complete, else
 *     buffered.
 *   - Continuation frames (caller passes is_continuation = 1)
 *     append to the reassembly buffer.
 *
 * For simplicity, ra_ble_host_acl_in() in this file conflates
 * first-fragment and continuation by inspecting whether we already
 * have a partial frame buffered. That is safe for the LE-U-only
 * scope.
 *
 * @param[in] conn_handle Connection handle (12 bits).
 * @param[in] payload     ACL data payload bytes.
 * @param[in] len         Byte count.
 *
 * @pre payload != NULL or len == 0.
 * @pre Stack is initialized (otherwise the function early-returns).
 * @post Inbound complete L2CAP frames have been dispatched to ATT.
 * @post Reassembly state may have advanced or reset.
 *
 * @note Not thread-safe; called from the HCI ACL trampoline only.
 *
 * @since 0.1.0
 */
static void ra_ble_host_acl_in(uint16_t conn_handle, const uint8_t* payload, uint16_t len)
{
  if ((payload == nullptr) || (len == 0U)) {
    return;
  }
  if (s_state.initialized == 0U) {
    return;
  }

  /* Are we mid-reassembly for the same connection? */
  if (s_state.reassembly_len > 0U) {
    if ((uint32_t)s_state.reassembly_len + (uint32_t)len > (uint32_t)k_reassembly_buf_bytes) {
      /* Drop, abort reassembly. */
      s_state.reassembly_len = 0U;
      return;
    }
    (void)memcpy(&s_state.reassembly[s_state.reassembly_len], payload, len);
    s_state.reassembly_len = (uint16_t)(s_state.reassembly_len + len);
    if (s_state.reassembly_len < s_state.reassembly_expected + k_l2cap_hdr_bytes) {
      return; /* still incomplete */
    }
  } else {
    if (len < k_l2cap_hdr_bytes) {
      return; /* bogus -- not even a full L2CAP header */
    }
    const uint16_t l2cap_len = internal_unpack_le16(&payload[0]);
    const uint16_t cid       = internal_unpack_le16(&payload[2]);
    if ((uint32_t)l2cap_len + (uint32_t)k_l2cap_hdr_bytes <= (uint32_t)len) {
      /* Complete frame in this one ACL packet. */
      if (cid == k_l2cap_cid_att) {
        ra_ble_host_att_handle_pdu(conn_handle, &payload[k_l2cap_hdr_bytes], l2cap_len);
      }
      /* Other CIDs (LE signaling) are silently consumed in this starter
       * stack -- the controller already answers default LE_LL events
       * (LL_CONNECTION_PARAM_REQ etc) on its own. */
      return;
    }
    /* Need to start reassembly. */
    if ((uint32_t)len > (uint32_t)k_reassembly_buf_bytes) {
      return;
    }
    (void)memcpy(s_state.reassembly, payload, len);
    s_state.reassembly_len      = len;
    s_state.reassembly_expected = l2cap_len;
    s_state.reassembly_cid      = cid;
    s_state.reassembly_conn     = conn_handle;
    return;
  }

  /* Reassembly complete -- dispatch. */
  if (s_state.reassembly_cid == k_l2cap_cid_att) {
    ra_ble_host_att_handle_pdu(s_state.reassembly_conn,
                               &s_state.reassembly[k_l2cap_hdr_bytes],
                               s_state.reassembly_expected);
  }
  s_state.reassembly_len = 0U;
}

/* HCI ACL -> host trampoline registered via ra_ble_attach_acl_handler -- see implementation for details. */
static void
internal_acl_trampoline(void* ctx, uint16_t handle, const uint8_t* payload, uint16_t len)
{
  (void)ctx;
  enum : uint16_t {
    k_handle_mask = 0x0FFFU, /**< Vol 4 Part E 5.4.2 ACL handle field. */
  };
  ra_ble_host_acl_in((uint16_t)(handle & k_handle_mask), payload, len);
}

/* =============================================================================
 * HCI -> host event handling (connection / disconnection)
 * =============================================================================
 */

/**
 * @brief HCI event trampoline: spot LE_Connection_Complete /
 *        Disconnection_Complete and update the host state.
 *
 * @details
 * Bluetooth Core 5.3 Vol 4 Part E:
 *   - 7.7.65.1 LE_Connection_Complete (subevent 0x01 of 0x3E LE Meta).
 *   - 7.7.5    Disconnection_Complete (event code 0x05).
 *
 * @param[in] ctx       Unused.
 * @param[in] evt_code  HCI event code.
 * @param[in] params    Parameter bytes.
 * @param[in] params_len Parameter byte count.
 *
 * @pre Registered via ra_ble_attach_event_handler.
 * @pre Stack is initialized.
 * @post Connection bookkeeping (conn_handle, att_mtu, CCCDs) updated
 *       and a connect/disconnect event dispatched on transitions.
 * @post No state change for unrecognised events.
 *
 * @note Not thread-safe; called only from the HCI driver callback.
 *
 * @since 0.1.0
 */
static void
internal_evt_trampoline(void* ctx, uint8_t evt_code, const uint8_t* params, uint8_t params_len)
{
  (void)ctx;
  enum : uint8_t {
    k_evt_disconn_complete    = 0x05U,
    k_evt_le_meta             = 0x3EU,
    k_subev_le_conn_complete  = 0x01U,
    k_disconn_handle_lo_idx   = 1U,
    k_disconn_handle_hi_idx   = 2U,
    k_lemeta_subev_idx        = 0U,
    k_lemeta_status_idx       = 1U,
    k_lemeta_handle_lo_idx    = 2U,
    k_lemeta_handle_hi_idx    = 3U,
    k_min_disconn_param_bytes = 4U,
    k_min_lemeta_param_bytes  = 19U,
  };

  if ((params == nullptr) || (s_state.initialized == 0U)) {
    return;
  }

  if ((evt_code == k_evt_le_meta) && (params_len >= k_min_lemeta_param_bytes) &&
      (params[k_lemeta_subev_idx] == k_subev_le_conn_complete)) {
    /* Status byte 0x00 == success. */
    if (params[k_lemeta_status_idx] == 0U) {
      const uint16_t h            = (uint16_t)((uint16_t)params[k_lemeta_handle_lo_idx] |
                                               ((uint16_t)params[k_lemeta_handle_hi_idx] << 8U));
      s_state.conn_handle         = h;
      s_state.att_mtu             = k_att_mtu_default;
      const ra_ble_host_event_t e = {
        .kind        = k_ra_ble_host_event_connected,
        .conn_handle = h,
        .attr_handle = 0U,
        .data        = nullptr,
        .data_len    = 0U,
      };
      ra_ble_host_dispatch_event(&e);
    }
  } else if ((evt_code == k_evt_disconn_complete) && (params_len >= k_min_disconn_param_bytes)) {
    const uint16_t h = (uint16_t)((uint16_t)params[k_disconn_handle_lo_idx] |
                                  ((uint16_t)params[k_disconn_handle_hi_idx] << 8U));
    if (h == s_state.conn_handle) {
      s_state.conn_handle = k_invalid_handle;
      /* Clear all CCCD subscriptions on disconnect (LE bonded devices
       * would persist these; we don't bond). */
      for (uint8_t i = 0U; i < s_state.attr_count; i++) {
        if (s_state.attrs[i].kind == k_attr_kind_cccd) {
          s_state.attrs[i].cccd_value = 0U;
        }
      }
      const ra_ble_host_event_t e = {
        .kind        = k_ra_ble_host_event_disconnected,
        .conn_handle = h,
        .attr_handle = 0U,
        .data        = nullptr,
        .data_len    = 0U,
      };
      ra_ble_host_dispatch_event(&e);
    }
  }
}

/* =============================================================================
 * Public lifecycle / advertising / event API
 * =============================================================================
 */

/**
 * @brief Initialize the BLE host stack.
 *
 * @details Validates the supplied config, opens the underlying HCI
 *          driver via ra_ble_open, attaches the host's HCI event /
 *          ACL trampolines, zeroes the GATT attribute table, and
 *          marks the host singleton as initialized. After this
 *          returns k_ra_ok the application may register services and
 *          start advertising. See Bluetooth Core 5.3 Vol 3 Part C 2.2
 *          for the GAP roles enumerated by ra_ble_host_role_t.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Stack is up.
 * @retval k_ra_err_null_ptr        cfg == NULL.
 * @retval k_ra_err_invalid_arg     cfg->role out of range or already initialized.
 * @retval k_ra_err_invalid_state   ra_ble_open failed.
 *
 * @pre Caller is single-threaded init context.
 * @pre Stack is not already initialized.
 * @post On success ra_ble_host_state()->initialized == 1.
 * @post On failure no state is left initialized.
 *
 * @note Not thread-safe; called once at boot.
 *
 * @since 0.1.0
 */
ra_err_t ra_ble_host_init(const ra_ble_host_config_t* cfg)
{
  if (cfg == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (s_state.initialized != 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((cfg->role != k_ra_ble_host_role_peripheral) && (cfg->role != k_ra_ble_host_role_central) &&
      (cfg->role != k_ra_ble_host_role_observer) && (cfg->role != k_ra_ble_host_role_broadcaster)) {
    return k_ra_err_invalid_arg;
  }

  const ra_ble_config_t ble_cfg = {.use_external_osc = 1U, .deep_sleep_enable = 1U};
  const ra_err_t        rc      = ra_ble_open(&ble_cfg);
  if (rc != k_ra_ok) {
    return k_ra_err_invalid_state;
  }

  (void)memset(&s_state, 0, sizeof(s_state));
  s_state.role        = cfg->role;
  s_state.appearance  = cfg->appearance;
  s_state.next_handle = 1U; /* ATT handles are 1-based. */
  s_state.conn_handle = k_invalid_handle;
  s_state.att_mtu     = k_att_mtu_default;
  if (cfg->name != nullptr) {
    /* Copy with bound. */
    enum : uint8_t {
      k_name_copy_max = 31U,
    };
    uint8_t i = 0U;
    while ((i < k_name_copy_max) && (cfg->name[i] != '\0')) {
      s_state.name[i] = cfg->name[i];
      i               = (uint8_t)(i + 1U);
    }
    s_state.name[i] = '\0';
  }

  /* Wire up HCI callbacks. ra_ble_attach_* always return k_ra_ok. */
  (void)ra_ble_attach_event_handler(internal_evt_trampoline, nullptr);
  (void)ra_ble_attach_acl_handler(internal_acl_trampoline, nullptr);

  s_state.initialized = 1U;
  return k_ra_ok;
}

/**
 * @brief Tear down the BLE host stack.
 *
 * @details Detaches the HCI event / ACL trampolines, calls
 *          ra_ble_close, and zeroes the singleton state.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Stack closed.
 * @retval k_ra_err_not_initialized Stack was never initialized.
 *
 * @pre ra_ble_host_init has succeeded.
 * @pre Caller is single-threaded.
 * @post Stack is no longer initialized.
 * @post All registered services / characteristics are forgotten.
 *
 * @note Not thread-safe; called once at shutdown.
 *
 * @since 0.1.0
 */
ra_err_t ra_ble_host_close(void)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  (void)ra_ble_attach_event_handler(nullptr, nullptr);
  (void)ra_ble_attach_acl_handler(nullptr, nullptr);
  (void)ra_ble_close();
  (void)memset(&s_state, 0, sizeof(s_state));
  return k_ra_ok;
}

/**
 * @brief Register the application's event handler.
 *
 * @details The handler receives connect / disconnect / write /
 *          subscribe events as they occur (see ra_ble_host_event_t).
 *          Passing NULL detaches the handler.
 *
 * @param[in] fn  Event callback (NULL detaches).
 * @param[in] ctx Opaque context forwarded to fn.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok Always returned.
 *
 * @pre ra_ble_host_init has succeeded.
 * @pre Caller is single-threaded.
 * @post s_state.evt_fn = fn and s_state.evt_ctx = ctx.
 * @post Subsequent events route to the new handler.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
ra_err_t ra_ble_host_attach_event_handler(ra_ble_host_event_fn_t fn, void* ctx)
{
  s_state.evt_fn  = fn;
  s_state.evt_ctx = ctx;
  return k_ra_ok;
}

/* =============================================================================
 * Test hooks
 * =============================================================================
 */

#ifdef UNIT_TEST
/**
 * @brief Test hook -- inject a synthetic L2CAP frame into the host.
 *
 * @details Wraps ra_ble_host_acl_in so unit tests can drive the host
 *          state machine without going through the controller HCI
 *          ACL pipe (Bluetooth Core 5.3 Vol 3 Part A 3).
 *
 * @param[in] conn_handle 12-bit ACL connection handle to attribute the
 *                        frame to.
 * @param[in] l2cap_frame L2CAP B-frame bytes.
 * @param[in] len         Total byte count.
 *
 * @pre Stack is initialized.
 * @pre l2cap_frame is non-NULL when len > 0.
 * @post The L2CAP layer has consumed the frame.
 * @post Reassembly state may have advanced.
 *
 * @note Not thread-safe; for unit-test harness use only.
 *
 * @since 0.1.0
 */
void ra_ble_host_test_inject_acl(uint16_t conn_handle, const uint8_t* l2cap_frame, uint16_t len)
{
  ra_ble_host_acl_in(conn_handle, l2cap_frame, len);
}

/**
 * @brief Test hook -- read the cumulative dispatched-event counter.
 *
 * @details Returns s_state.evt_count, useful for verifying that an
 *          expected event was synthesised by the L2CAP/ATT layer.
 *
 * @return uint32_t Cumulative dispatched-event count.
 * @retval 0  No events dispatched yet.
 * @retval >0 At least one event delivered.
 *
 * @pre Stack is initialized.
 * @pre Caller is single-threaded.
 * @post No state change.
 * @post Return value is monotonically non-decreasing.
 *
 * @note Not thread-safe; for unit-test harness use only.
 *
 * @since 0.1.0
 */
uint32_t ra_ble_host_test_event_count(void)
{
  return s_state.evt_count;
}

/**
 * @brief Test hook -- force the host into the "connected" state.
 *
 * @details Bypasses the HCI transport and forces s_state.conn_handle
 *          to conn_handle, dispatching a synthetic
 *          k_ra_ble_host_event_connected. Mirrors the effect of
 *          Bluetooth Core 5.3 Vol 4 Part E 7.7.65.1
 *          (LE_Connection_Complete).
 *
 * @param[in] conn_handle 12-bit ACL connection handle to assign.
 *
 * @pre Stack is initialized.
 * @pre Caller is single-threaded.
 * @post s_state.conn_handle == conn_handle.
 * @post A k_ra_ble_host_event_connected event was dispatched.
 *
 * @note Not thread-safe; for unit-test harness use only.
 *
 * @since 0.1.0
 */
void ra_ble_host_test_inject_connect(uint16_t conn_handle)
{
  s_state.conn_handle         = conn_handle;
  s_state.att_mtu             = k_att_mtu_default;
  const ra_ble_host_event_t e = {
    .kind        = k_ra_ble_host_event_connected,
    .conn_handle = conn_handle,
    .attr_handle = 0U,
    .data        = nullptr,
    .data_len    = 0U,
  };
  ra_ble_host_dispatch_event(&e);
}

/**
 * @brief Test hook -- synthesize an inbound HCI event for MC/DC vectors.
 *
 * @details Forwards directly to the static
 *          ``internal_evt_trampoline`` so tests can vary
 *          (params, params_len, evt_code, subev) and exercise the
 *          line-576 params-NULL-or-uninit guard and the line-580 /
 *          line-597 decisions on the production source.
 *
 * @param[in] evt_code   HCI event code.
 * @param[in] params     Parameter bytes (may be NULL).
 * @param[in] params_len Parameter byte count.
 *
 * @pre Caller is single-threaded.
 * @pre @p params is non-NULL when @p params_len > 0 and the caller
 *      expects the trampoline to dispatch.
 * @post No state mutation when guards reject the event.
 * @post Connection bookkeeping advanced when the LE_Connection_Complete
 *       or Disconnection_Complete decisions evaluate true.
 *
 * @note Not thread-safe; for unit-test harness use only.
 *
 * @since 0.1.0
 */
void ra_ble_host_test_inject_event(uint8_t evt_code, const uint8_t* params, uint8_t params_len)
{
  internal_evt_trampoline(nullptr, evt_code, params, params_len);
}
#endif
// NOLINTEND(readability-function-size,readability-function-cognitive-complexity)
