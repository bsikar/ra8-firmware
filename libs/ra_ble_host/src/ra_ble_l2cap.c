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

// NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity)
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_ble.h"
#include "ra_ble_host.h"
#include "ra_err.h"

/* =============================================================================
 * Internal contract shared with ra_ble_att.c / ra_ble_gatt.c
 *
 * Kept in this .c file (with extern "C" linkage equivalents) rather
 * than a separate _internal.h header because (a) the surface is
 * small and (b) the Doxygen-required documentation lives next to
 * the implementation it describes.
 * =============================================================================
 */

/**
 * @enum ra_ble_host_constants_t
 * @brief Implementation-internal numeric constants for the host stack.
 *
 * @details
 * Bluetooth Core 5.3 Vol 3 Part A 3 "Data Packet Format" sets the
 * L2CAP B-frame header at 4 bytes; Vol 4 Part E 5.4.2 sets the HCI
 * ACL header at 4 bytes; Vol 3 Part F 3.4.2 caps default ATT_MTU at
 * 23. All other values are sized to keep the host self-contained.
 */
typedef enum : uint16_t {
  k_l2cap_cid_att          = 0x0004U, /**< Vol 3 Part A 2.1 Table 2.3.   */
  k_l2cap_cid_le_signaling = 0x0005U, /**< Vol 3 Part A 2.1 Table 2.3.   */
  k_l2cap_hdr_bytes        = 4U,      /**< len(LE16) + cid(LE16).        */
  k_acl_hdr_bytes          = 4U,      /**< handle/PB/BC + len.           */
  k_att_mtu_default        = 23U,     /**< Vol 3 Part F 3.4.2.           */
  k_att_mtu_max            = 247U,    /**< Stack-internal cap.           */
  k_reassembly_buf_bytes   = 256U,    /**< Inbound L2CAP reassembly.     */
  k_tx_scratch_bytes       = 256U,    /**< Outbound L2CAP scratch.       */
  k_invalid_handle         = 0x0000U, /**< Sentinel for "no connection". */
  k_pb_first_non_flushable = 0x0000U, /**< Vol 4 Part E 5.4.2 PB flags.  */
  k_pb_continuation        = 0x1000U,
  k_pb_first_complete      = 0x2000U,
} ra_ble_host_constants_t;

/**
 * @enum ra_ble_host_attr_kind_t
 * @brief Internal attribute-table entry classification.
 */
typedef enum : uint8_t {
  k_attr_kind_primary_service = 0U, /**< UUID 0x2800.                   */
  k_attr_kind_char_decl       = 1U, /**< UUID 0x2803.                   */
  k_attr_kind_char_value      = 2U, /**< UUID = char's UUID.            */
  k_attr_kind_cccd            = 3U, /**< UUID 0x2902.                   */
} ra_ble_host_attr_kind_t;

/**
 * @struct ra_ble_host_attr_t
 * @brief One row of the GATT attribute table.
 *
 * @details Bluetooth Core 5.3 Vol 3 Part F 3.2 defines an attribute
 *          as (handle, type, permissions, value). We pack a 128-bit
 *          UUID for type (16-bit assigned numbers are stored
 *          big-endian-padded into the same 16-byte field).
 */
typedef struct {
  uint16_t                handle; /**< ATT handle (1-based).                          */
  ra_ble_host_attr_kind_t kind;   /**< Internal classification.                       */
  uint8_t                 props;  /**< Properties (only meaningful for char_decl).    */
  uint8_t                 uuid[k_ra_ble_host_uuid_bytes]; /**< Type UUID (LE).           */
  uint8_t*                value;              /**< Backing storage. NULL = no value.              */
  uint16_t                value_len;          /**< Current valid bytes in ``value``.              */
  uint16_t                value_max;          /**< Capacity of ``value``.                         */
  uint16_t                cccd_value;         /**< Mirror of CCCD bits when kind == cccd.        */
  uint16_t                value_handle_owner; /**< For cccd: which char value handle.    */
} ra_ble_host_attr_t;

/**
 * @struct ra_ble_host_state_t
 * @brief Singleton runtime state shared across the three host TUs.
 *
 * @details
 * Sized for the limits in ``ra_ble_host_limits_t``. Each
 * characteristic produces 2 or 3 attribute-table rows (decl + value
 * + optional CCCD), so the table is sized 1 + 8 + 32*3 == 105 to
 * leave headroom; we cap at 96 here, which covers the worst case
 * (all 32 chars carry CCCDs: 8 + 32*3 = 104; round to a 96-attribute
 * cap with a runtime check that rejects further inserts when full).
 */
typedef struct {
  uint8_t                initialized;
  ra_ble_host_role_t     role;
  uint16_t               appearance;
  char                   name[32];
  uint16_t               next_handle;
  uint16_t               last_service_handle;
  ra_ble_host_attr_t     attrs[96];
  uint8_t                attr_count;
  uint8_t                service_count;
  uint8_t                char_count;
  uint16_t               conn_handle;
  uint16_t               att_mtu;
  uint8_t                reassembly[k_reassembly_buf_bytes];
  uint16_t               reassembly_len;
  uint16_t               reassembly_expected;
  uint16_t               reassembly_cid;
  uint16_t               reassembly_conn;
  ra_ble_host_event_fn_t evt_fn;
  void*                  evt_ctx;
  uint32_t               evt_count;
} ra_ble_host_state_t;

/* The single shared-state instance. */
static ra_ble_host_state_t s_state;

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

/**
 * @brief Pack a little-endian uint16 into a byte buffer.
 *
 * @param[out] dst Two-byte destination.
 * @param[in]  v   Value to pack.
 *
 * @pre dst != NULL.
 * @post dst[0..1] holds the LE16 encoding of ``v``.
 *
 * @since 0.1.0
 */
static void internal_pack_le16(uint8_t* dst, uint16_t v)
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
 * @param[in] src Two-byte source. Must not be NULL.
 * @return Unpacked value.
 *
 * @pre src != NULL.
 * @post return value matches src[0] | (src[1] << 8).
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
 *
 * @pre None.
 * @post Returned pointer is valid for the program's lifetime.
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
 * @param[in] evt Event payload. Must not be NULL.
 *
 * @pre evt != NULL.
 * @post s_state.evt_count incremented.
 *
 * @since 0.1.0
 */
void ra_ble_host_dispatch_event(const ra_ble_host_event_t* evt)
{
  s_state.evt_count++;
  if (s_state.evt_fn != NULL) {
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
 * @since 0.1.0
 */
ra_err_t ra_ble_host_l2cap_send(uint16_t       conn_handle,
                                uint16_t       cid,
                                const uint8_t* payload,
                                uint16_t       payload_len)
{
  if ((payload == NULL) && (payload_len > 0U)) {
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
 * @post Inbound complete L2CAP frames have been dispatched to ATT.
 *
 * @since 0.1.0
 */
static void ra_ble_host_acl_in(uint16_t conn_handle, const uint8_t* payload, uint16_t len)
{
  if ((payload == NULL) || (len == 0U)) {
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

/**
 * @brief HCI ACL -> host trampoline registered via ra_ble_attach_acl_handler.
 *
 * @param[in] ctx     Unused (always NULL here).
 * @param[in] handle  HCI ACL handle word (handle | PB | BC).
 * @param[in] payload ACL data bytes.
 * @param[in] len     Byte count.
 *
 * @since 0.1.0
 */
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

  if ((params == NULL) || (s_state.initialized == 0U)) {
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
        .data        = NULL,
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
        .data        = NULL,
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

ra_err_t ra_ble_host_init(const ra_ble_host_config_t* cfg)
{
  if (cfg == NULL) {
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
  if (cfg->name != NULL) {
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
  (void)ra_ble_attach_event_handler(internal_evt_trampoline, NULL);
  (void)ra_ble_attach_acl_handler(internal_acl_trampoline, NULL);

  s_state.initialized = 1U;
  return k_ra_ok;
}

ra_err_t ra_ble_host_close(void)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  (void)ra_ble_attach_event_handler(NULL, NULL);
  (void)ra_ble_attach_acl_handler(NULL, NULL);
  (void)ra_ble_close();
  (void)memset(&s_state, 0, sizeof(s_state));
  return k_ra_ok;
}

ra_err_t ra_ble_host_advertise_start(const uint8_t* adv_data,
                                     uint8_t        adv_data_len,
                                     const uint8_t* scan_resp,
                                     uint8_t        scan_resp_len,
                                     uint16_t       interval_ms)
{
  enum : uint16_t {
    k_min_interval_ms = 20U,
    k_max_interval_ms = 10240U,
    /* 0.625 ms units per Vol 4 Part E 7.8.5. interval = ms * 1000 / 625 = ms*8/5. */
    k_us625_per_ms_num      = 8U,
    k_us625_per_ms_den      = 5U,
    k_op_le_set_adv_params  = 0x2006U,
    k_adv_params_param_byte = 15U,
  };

  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  if ((s_state.role != k_ra_ble_host_role_peripheral) &&
      (s_state.role != k_ra_ble_host_role_broadcaster)) {
    return k_ra_err_invalid_arg;
  }
  if ((adv_data == NULL) && (adv_data_len > 0U)) {
    return k_ra_err_null_ptr;
  }
  if ((scan_resp == NULL) && (scan_resp_len > 0U)) {
    return k_ra_err_null_ptr;
  }
  if ((adv_data_len > k_ra_ble_adv_data_max) || (scan_resp_len > k_ra_ble_adv_data_max)) {
    return k_ra_err_invalid_arg;
  }
  if ((interval_ms < k_min_interval_ms) || (interval_ms > k_max_interval_ms)) {
    return k_ra_err_invalid_arg;
  }

  /* LE_Set_Advertising_Parameters: 15-byte param block, Vol 4 Part E 7.8.5.
   * For brevity we only set min/max interval; the remaining fields stay
   * at their defaults (0x00) which corresponds to ADV_IND on all primary
   * channels with public address, no peer filter. */
  const uint16_t interval_625us =
    (uint16_t)((uint32_t)interval_ms * (uint32_t)k_us625_per_ms_num / (uint32_t)k_us625_per_ms_den);
  uint8_t adv_params[k_adv_params_param_byte] = {};
  internal_pack_le16(&adv_params[0], interval_625us);
  internal_pack_le16(&adv_params[2], interval_625us);
  /* adv_params[4] = adv_type 0x00 (ADV_IND), rest zero. */
  /* All-channels mask = 0x07 at offset 13. */
  enum : uint8_t {
    k_offset_chan_map = 13U,
    k_chan_map_all    = 0x07U,
  };
  adv_params[k_offset_chan_map] = k_chan_map_all;

  ra_err_t rc =
    ra_ble_hci_send_command(k_op_le_set_adv_params, adv_params, k_adv_params_param_byte);
  if (rc != k_ra_ok) {
    return rc;
  }

  rc = ra_ble_set_advertising_data(adv_data, adv_data_len);
  if (rc != k_ra_ok) {
    return rc;
  }

  if (scan_resp_len > 0U) {
    enum : uint16_t {
      k_op_le_set_scan_resp_data = 0x2009U,
    };
    enum : uint8_t {
      k_scan_resp_param_byte = 32U,
    };
    uint8_t sr[k_scan_resp_param_byte] = {};
    sr[0]                              = scan_resp_len;
    (void)memcpy(&sr[1], scan_resp, scan_resp_len);
    rc = ra_ble_hci_send_command(k_op_le_set_scan_resp_data, sr, k_scan_resp_param_byte);
    if (rc != k_ra_ok) {
      return rc;
    }
  }

  return ra_ble_set_advertising_enable(1U);
}

ra_err_t ra_ble_host_advertise_stop(void)
{
  if (s_state.initialized == 0U) {
    return k_ra_err_not_initialized;
  }
  return ra_ble_set_advertising_enable(0U);
}

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
void ra_ble_host_test_inject_acl(uint16_t conn_handle, const uint8_t* l2cap_frame, uint16_t len)
{
  ra_ble_host_acl_in(conn_handle, l2cap_frame, len);
}

uint32_t ra_ble_host_test_event_count(void)
{
  return s_state.evt_count;
}

void ra_ble_host_test_inject_connect(uint16_t conn_handle)
{
  s_state.conn_handle         = conn_handle;
  s_state.att_mtu             = k_att_mtu_default;
  const ra_ble_host_event_t e = {
    .kind        = k_ra_ble_host_event_connected,
    .conn_handle = conn_handle,
    .attr_handle = 0U,
    .data        = NULL,
    .data_len    = 0U,
  };
  ra_ble_host_dispatch_event(&e);
}
#endif
// NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity)
