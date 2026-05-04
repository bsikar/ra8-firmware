/**
 * @file ble_hci_ra_ble.c
 * @brief Apache NimBLE HCI transport adapter against the ra_ble HCI ring
 *
 * @par Tag
 * [Ring 4 / PORT] {World: S}
 *
 * @details
 * Implementation of the LL-side HCI transport for Apache NimBLE,
 * routed onto our ``ra_ble`` driver. NimBLE's host stack runs in the
 * "host" partition and emits HCI commands / ACL frames through
 * ``ble_transport_to_ll_cmd`` / ``ble_transport_to_ll_acl`` -- both
 * resolve to our ``*_impl`` symbols below thanks to the upstream
 * weak-binding scheme.
 *
 * Inbound HCI events / ACL frames originate from the radio block.
 * ``ra_ble_dispatch`` (called from the kernel main loop or BLE IRQ)
 * fires our ``priv_event_cb`` / ``priv_acl_cb`` callbacks. We copy
 * the bytes into NimBLE-allocated buffers and hand them off to
 * ``ble_transport_to_hs_evt`` / ``ble_transport_to_hs_acl`` so they
 * land in NimBLE's host-side input queue.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ble_hci_ra_ble.h"

#include <stdint.h>
#include <string.h>

#include "ra_ble.h"
#include "ra_ble_patch.h"
#include "ra_err.h"

/* =============================================================================
 * Forward decls of NimBLE transport API
 * =============================================================================
 *
 * We avoid pulling in ``nimble/transport.h`` directly because that
 * header transitively requires the upstream syscfg / mbuf machinery
 * which the curated build does not enumerate. The contracts below
 * match upstream verbatim and are resolved at link time against the
 * `nimble` interface library.
 */

struct os_mbuf;

/**
 * @brief Inbound HCI event submission to the host side.
 *
 * @details See implementation for details.
 * @param[in,out] buf See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
extern int ble_transport_to_hs_evt(void* buf);

/**
 * @brief Inbound HCI ACL submission to the host side.
 *
 * @details See implementation for details.
 * @param[in,out] om See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
extern int ble_transport_to_hs_acl(struct os_mbuf* om);

/** @brief Allocate a non-discardable HCI event buffer (host-side). */
extern void* ble_transport_alloc_evt(int discardable);

/** @brief Allocate a host-side ACL mbuf travelling LL -> HS. */
extern struct os_mbuf* ble_transport_alloc_acl_from_ll(void);

/**
 * @brief Append `len` bytes of `data` to the end of the mbuf chain.
 *
 * @details See implementation for details.
 * @param[in,out] data See function signature.
 * @param[in,out] len See function signature.
 * @param[in,out] om See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
extern int os_mbuf_append(struct os_mbuf* om, const void* data, uint16_t len);

/**
 * @brief Free an mbuf chain.
 *
 * @details See implementation for details.
 * @param[in,out] om See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
extern int os_mbuf_free_chain(struct os_mbuf* om);

/* =============================================================================
 * Adapter-private state
 * =============================================================================
 */

/**
 * @enum ble_hci_ra_ble_state_t
 * @brief Adapter lifecycle states.
 */
typedef enum : uint8_t {
  k_ble_hci_ra_ble_state_idle    = 0U, /**< Constructed, not yet attached.  */
  k_ble_hci_ra_ble_state_running = 1U, /**< Callbacks attached to ra_ble.   */
} ble_hci_ra_ble_state_t;

/**
 * @enum ble_hci_ra_ble_evt_layout_t
 * @brief Layout offsets used when packing/unpacking HCI event buffers.
 *
 * @details
 * Bluetooth Core 5.3 Vol 4 Part E 5.4.4 "HCI Event Packet": the host
 * receives ``[evt_code][param_len][params...]`` as a flat byte array.
 * The ``ra_ble`` driver hands us ``evt_code`` + ``params`` separately;
 * we re-pack them into NimBLE's evt-buffer format.
 */
typedef enum : uint8_t {
  k_ble_hci_evt_off_code   = 0U, /**< Offset of the event-code byte.       */
  k_ble_hci_evt_off_len    = 1U, /**< Offset of the parameter-length byte. */
  k_ble_hci_evt_off_params = 2U, /**< Offset of the parameter payload.     */
  k_ble_hci_evt_header_len = 2U, /**< Total event-header byte count.       */
} ble_hci_ra_ble_evt_layout_t;

/**
 * @enum ble_hci_ra_ble_acl_layout_t
 * @brief Layout offsets used when packing/unpacking HCI ACL data.
 *
 * @details
 * Bluetooth Core 5.3 Vol 4 Part E 5.4.2 "HCI ACL Data Packet": flat
 * byte layout ``[handle_lo][handle_hi][len_lo][len_hi][payload...]``.
 */
typedef enum : uint8_t {
  k_ble_hci_acl_off_handle_lo = 0U, /**< Handle low byte.            */
  k_ble_hci_acl_off_handle_hi = 1U, /**< Handle high byte + flags.   */
  k_ble_hci_acl_off_len_lo    = 2U, /**< Length low byte.            */
  k_ble_hci_acl_off_len_hi    = 3U, /**< Length high byte.           */
  k_ble_hci_acl_off_payload   = 4U, /**< Payload start offset.       */
  k_ble_hci_acl_header_len    = 4U, /**< Total ACL-header byte count.*/
} ble_hci_ra_ble_acl_layout_t;

/**
 * @enum ble_hci_ra_ble_bits_t
 * @brief Shared bit-shift constants.
 */
typedef enum : uint8_t {
  k_ble_hci_shift_byte = 8U, /**< Byte shift for LE16 fold/unfold. */
} ble_hci_ra_ble_bits_t;

/**
 * @enum ble_hci_ra_ble_masks_t
 * @brief Shared bit-mask constants.
 */
typedef enum : uint32_t {
  k_ble_hci_mask_byte = 0xFFU, /**< Low-byte mask for 16-bit splits. */
} ble_hci_ra_ble_masks_t;

/** @brief Single-instance state for the adapter. */
static ble_hci_ra_ble_state_t s_state = k_ble_hci_ra_ble_state_idle;

/* =============================================================================
 * Private callbacks: ra_ble -> NimBLE host
 * =============================================================================
 */

/**
 * @brief Inbound HCI event callback (controller -> host).
 *
 * @details
 * Allocates a NimBLE event buffer, copies ``[evt_code][param_len]
 * [params...]`` into it, and hands it to ``ble_transport_to_hs_evt``.
 * Drops the event silently if the allocator returns NULL (the host
 * stack has back-pressure of its own).
 *
 * @param[in] ctx        Unused; registered as ``nullptr``.
 * @param[in] evt_code   HCI event code (Bluetooth Core 5.3 Vol 4 Part E 7.7).
 * @param[in] params     Parameter byte pointer (driver scratch region).
 * @param[in] params_len Parameter byte count.
 *
 * @pre ``ble_hci_ra_ble_init`` returned ``k_ra_ok``.
 * @pre params != nullptr || params_len == 0.
 * @post Either the event landed in NimBLE's host queue, or it was
 *       dropped due to a transient OOM.
 *
 * @note Invoked from ``ra_ble_dispatch``; thread context depends on
 *       where the caller schedules dispatch (kernel loop or ISR).
 *
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 */
static void priv_event_cb(void* ctx, uint8_t evt_code, const uint8_t* params, uint8_t params_len)
{
  (void)ctx;
  if (s_state != k_ble_hci_ra_ble_state_running) {
    return;
  }
  if (params == NULL && params_len > 0U) {
    return;
  }

  uint8_t* hci_evt = (uint8_t*)ble_transport_alloc_evt(0);
  if (hci_evt == NULL) {
    /* Transient OOM -- the controller will retry. */
    return;
  }

  hci_evt[k_ble_hci_evt_off_code] = evt_code;
  hci_evt[k_ble_hci_evt_off_len]  = params_len;
  if (params_len > 0U) {
    (void)memcpy(&hci_evt[k_ble_hci_evt_off_params], params, params_len);
  }

  (void)ble_transport_to_hs_evt(hci_evt);
}

/**
 * @brief Inbound ACL data callback (controller -> host).
 *
 * @details
 * Allocates a NimBLE ACL mbuf, appends ``[handle_lo][handle_hi]
 * [len_lo][len_hi][payload]`` into it, and hands it to
 * ``ble_transport_to_hs_acl``. Drops on allocator failure.
 *
 * @param[in] ctx     Unused; registered as ``nullptr``.
 * @param[in] handle  Connection handle + PB/BC flags (LE16).
 * @param[in] payload Payload byte pointer (driver scratch region).
 * @param[in] len     Payload byte count.
 *
 * @pre ``ble_hci_ra_ble_init`` returned ``k_ra_ok``.
 * @pre payload != nullptr || len == 0.
 * @post Either the frame landed in NimBLE's host queue, or it was
 *       dropped due to a transient OOM.
 *
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
static void priv_acl_cb(void* ctx, uint16_t handle, const uint8_t* payload, uint16_t len)
{
  (void)ctx;
  if (s_state != k_ble_hci_ra_ble_state_running) {
    return;
  }
  if (payload == NULL && len > 0U) {
    return;
  }

  struct os_mbuf* om = ble_transport_alloc_acl_from_ll();
  if (om == NULL) {
    return;
  }

  uint8_t hdr[k_ble_hci_acl_header_len];
  hdr[k_ble_hci_acl_off_handle_lo] = (uint8_t)(handle & k_ble_hci_mask_byte);
  hdr[k_ble_hci_acl_off_handle_hi] =
    (uint8_t)((handle >> k_ble_hci_shift_byte) & k_ble_hci_mask_byte);
  hdr[k_ble_hci_acl_off_len_lo] = (uint8_t)(len & k_ble_hci_mask_byte);
  hdr[k_ble_hci_acl_off_len_hi] = (uint8_t)((len >> k_ble_hci_shift_byte) & k_ble_hci_mask_byte);

  if (os_mbuf_append(om, hdr, (uint16_t)k_ble_hci_acl_header_len) != 0) {
    (void)os_mbuf_free_chain(om);
    return;
  }
  if (len > 0U && os_mbuf_append(om, payload, len) != 0) {
    (void)os_mbuf_free_chain(om);
    return;
  }

  (void)ble_transport_to_hs_acl(om);
}

/* =============================================================================
 * Public lifecycle
 * =============================================================================
 */

/**
 * @brief Ble hci ra ble init.
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
ra_err_t ble_hci_ra_ble_init(void)
{
  /*
   * Probe the BLE controller patch loader. The stub returns
   * ``k_ra_err_not_supported`` and logs a one-shot warning when no
   * Renesas-supplied patch image is configured; we proceed anyway so
   * unit-test builds and dry-run smoke tests still come up.
   */
  (void)ra_ble_patch_load(NULL, 0U);

  if (ra_ble_attach_event_handler(priv_event_cb, NULL) != k_ra_ok) {
    return k_ra_err_not_initialized;
  }
  if (ra_ble_attach_acl_handler(priv_acl_cb, NULL) != k_ra_ok) {
    return k_ra_err_not_initialized;
  }
  s_state = k_ble_hci_ra_ble_state_running;
  return k_ra_ok;
}

/**
 * @brief Ble hci ra ble deinit.
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
ra_err_t ble_hci_ra_ble_deinit(void)
{
  s_state = k_ble_hci_ra_ble_state_idle;
  (void)ra_ble_attach_event_handler(NULL, NULL);
  (void)ra_ble_attach_acl_handler(NULL, NULL);
  return k_ra_ok;
}

/* =============================================================================
 * NimBLE LL-side transport hooks (host -> controller)
 * =============================================================================
 *
 * The NimBLE transport core calls these *_impl() entry points -- the
 * outer ``ble_transport_to_ll_cmd`` / ``ble_transport_to_ll_acl``
 * wrappers in libs/third_party/nimble/nimble/transport/ resolve here.
 * ``ble_transport_ll_init`` is invoked from the upstream
 * ``ble_transport_init`` so we use it to hot-attach our callbacks
 * even when the app skipped the explicit ``ble_hci_ra_ble_init``.
 */

/**
 * @brief LL-side transport init hook (called by ``ble_transport_init``).
 *
 * @details
 * Idempotent: if the app already called ``ble_hci_ra_ble_init`` we
 * leave the running state alone. Otherwise we attach the inbound
 * callbacks here so the very first HCI command issued by the host
 * has a return path ready.
 *
 * @pre ``ra_ble_init`` returned ``k_ra_ok``.
 * @post Adapter is in the ``running`` state.
 *
 * @since 0.1.0
 *
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
void ble_transport_ll_init(void)
{
  if (s_state != k_ble_hci_ra_ble_state_running) {
    (void)ble_hci_ra_ble_init();
  }
}

/**
 * @brief Forward a host-side HCI command to the controller.
 *
 * @details
 * Bluetooth Core 5.3 Vol 4 Part E 5.4.1 "HCI Command Packet":
 * ``[opcode_lo][opcode_hi][param_len][params...]``. NimBLE hands us
 * the buffer; we split out the opcode and forward the params to
 * ``ra_ble_hci_send_command``. We always free the buffer on the way
 * out (success or fail) -- the buffer is owned by us once received.
 *
 * @param[in] buf NimBLE-allocated command buffer.
 *
 * @pre ``ble_hci_ra_ble_init`` returned ``k_ra_ok``.
 * @pre buf != NULL.
 * @post buf has been freed via ``ble_transport_free``.
 *
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
extern void ble_transport_free(void* buf);

/**
 * @brief Ble transport to ll cmd impl.
 *
 * @details See implementation for details.
 *
 * @param[in,out] buf See function signature for type and usage.
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
int ble_transport_to_ll_cmd_impl(void* buf)
{
  if (buf == NULL) {
    return -1;
  }
  const uint8_t* bytes = (const uint8_t*)buf;
  const uint16_t opcode =
    (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << k_ble_hci_shift_byte));
  const uint8_t  params_len = bytes[2];
  const uint8_t* params     = (params_len == 0U) ? NULL : &bytes[3];

  const ra_err_t err = ra_ble_hci_send_command(opcode, params, params_len);
  ble_transport_free(buf);
  return (err == k_ra_ok) ? 0 : -1;
}

/**
 * @brief Forward a host-side HCI ACL frame to the controller.
 *
 * @details
 * Bluetooth Core 5.3 Vol 4 Part E 5.4.2 "HCI ACL Data Packet": NimBLE
 * holds the framed bytes inside an mbuf chain. We linearise the chain
 * into a stack buffer, peel off the 4-byte HCI header, and push the
 * payload through ``ra_ble_hci_send_acl_data``. Mbuf is freed on the
 * way out regardless of success.
 *
 * @param[in] om NimBLE-allocated ACL mbuf chain.
 *
 * @return 0 on success, negative on failure.
 *
 * @pre ``ble_hci_ra_ble_init`` returned ``k_ra_ok``.
 * @pre om != NULL.
 * @post om has been freed via ``os_mbuf_free_chain``.
 *
 * @since 0.1.0
 *
 * @retval 0 Success or default value.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
extern uint16_t os_mbuf_len(const struct os_mbuf* om);
/**
 * @brief Os mbuf copydata.
 *
 * @details See implementation for details.
 *
 * @param[in,out] om See function signature for type and usage.
 * @param[in,out] off See function signature for type and usage.
 * @param[in,out] len See function signature for type and usage.
 * @param[in,out] dst See function signature for type and usage.
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
extern int      os_mbuf_copydata(const struct os_mbuf* om, int off, int len, void* dst);

/**
 * @brief Ble transport to ll acl impl.
 *
 * @details See implementation for details.
 *
 * @param[in,out] om See function signature for type and usage.
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
int ble_transport_to_ll_acl_impl(struct os_mbuf* om)
{
  if (om == NULL) {
    return -1;
  }
  const uint16_t total = os_mbuf_len(om);
  if (total < (uint16_t)k_ble_hci_acl_header_len) {
    (void)os_mbuf_free_chain(om);
    return -1;
  }
  if (total > (uint16_t)k_ble_hci_ra_ble_acl_buf_max) {
    (void)os_mbuf_free_chain(om);
    return -1;
  }

  uint8_t flat[k_ble_hci_ra_ble_acl_buf_max];
  if (os_mbuf_copydata(om, 0, (int)total, flat) != 0) {
    (void)os_mbuf_free_chain(om);
    return -1;
  }
  (void)os_mbuf_free_chain(om);

  const uint16_t handle =
    (uint16_t)((uint16_t)flat[k_ble_hci_acl_off_handle_lo] |
               ((uint16_t)flat[k_ble_hci_acl_off_handle_hi] << k_ble_hci_shift_byte));
  const uint16_t len =
    (uint16_t)((uint16_t)flat[k_ble_hci_acl_off_len_lo] |
               ((uint16_t)flat[k_ble_hci_acl_off_len_hi] << k_ble_hci_shift_byte));
  if ((uint16_t)(len + (uint16_t)k_ble_hci_acl_header_len) > total) {
    return -1;
  }

  const uint8_t* payload = (len == 0U) ? NULL : &flat[k_ble_hci_acl_off_payload];
  const ra_err_t err     = ra_ble_hci_send_acl_data(handle, payload, len);
  return (err == k_ra_ok) ? 0 : -1;
}

/**
 * @brief Stub LL-side ISO transport hook.
 *
 * @details
 * Bluetooth LE Audio (Core 5.2+) ISO data is not exposed through
 * ``ra_ble`` yet. We accept the call to keep the link satisfied,
 * free the mbuf, and return success.
 *
 * @param[in] om NimBLE-allocated ISO mbuf chain.
 *
 * @return 0 on success.
 *
 * @pre om may be NULL.
 * @post om has been freed (if non-NULL).
 *
 * @since 0.1.0
 *
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
int ble_transport_to_ll_iso_impl(struct os_mbuf* om)
{
  if (om != NULL) {
    (void)os_mbuf_free_chain(om);
  }
  return 0;
}
