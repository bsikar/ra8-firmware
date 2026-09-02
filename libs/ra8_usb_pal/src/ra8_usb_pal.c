/**
 * @file ra8_usb_pal.c
 * @brief USB PAL implementation -- ra8_usb wrapper
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * PAL over the Ring-3 ``ra8_usb`` driver. Endpoints are
 * backed by a small in-memory queue so the PAL is usable in host
 * tests (and any future software-only transport) before the real
 * ra8_usb pipe primitives land. On hardware the queue is backed
 * by the controller's pipe FIFOs; the stack-facing contract is
 * identical in either case.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_usb_pal.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_usb.h"
#include "ra8_usb_pal_internal.h"

/**
 * @brief Pure dispatch-event predicate -- see header for full contract.
 * @details Promoted helper so the line-218 AND can be driven under MC/DC.
 * @param[in] event_fn   Application callback.
 * @param[in] mask       Translated event-mask bits.
 * @param[in] none_value Numeric value of @c k_ra8_usb_pal_event_none.
 * @return Boolean predicate.
 * @retval true  Caller invokes event_fn.
 * @retval false Skip the callback.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool priv_usb_pal_should_dispatch_event(const void* event_fn, uint16_t mask, uint16_t none_value)
{
  return (event_fn != nullptr) && (mask != none_value);
}

/**
 * @brief Pure ep-out-of-range predicate -- see header for full contract.
 * @details Promoted helper so the line-559 OR can be driven under MC/DC.
 * @param[in] ep_addr Endpoint number.
 * @param[in] ep_max  Maximum permitted endpoint number.
 * @return Boolean reject predicate.
 * @retval true  Caller returns invalid-arg.
 * @retval false Endpoint is in range.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool priv_usb_pal_ep_out_of_range(uint8_t ep_addr, uint8_t ep_max)
{
  return (ep_addr == 0U) || (ep_addr > ep_max);
}

static const char* s_tag = "USBPAL";

/* =============================================================================
 * Ring sizing
 * =============================================================================
 */

typedef enum : uint16_t {
  k_ra8_usb_pal_ep_table_len = 11U,   /**< Index 0..ep_max, 1-based EPs. */
  k_ra8_usb_pal_ring_slots   = 4U,    /**< Per-EP queue depth.           */
  k_ra8_usb_pal_pkt_max      = 1024U, /**< Per-packet capacity.          */
} ra8_usb_pal_ring_dim_t;

/* =============================================================================
 * Per-EP + per-PAL state
 * =============================================================================
 */

/**
 * @struct ra8_usb_pal_packet_t
 * @brief One packet on a per-endpoint ring.
 */
typedef struct {
  uint16_t len;                         /**< 0 == empty slot.      */
  uint8_t  data[k_ra8_usb_pal_pkt_max]; /**< Packet payload bytes. */
} ra8_usb_pal_packet_t;

/**
 * @struct ra8_usb_pal_ep_slot_t
 * @brief Configuration + queue for one endpoint.
 */
typedef struct {
  bool                  opened;                         /**< True once ra8_usb_pal_ep_open fired. */
  ra8_usb_pal_ep_dir_t  dir;                            /**< Stored direction.                    */
  ra8_usb_pal_ep_type_t type;                           /**< Stored transfer type.                */
  uint16_t              max_packet;                     /**< Stored max packet size.              */
  uint16_t              head;                           /**< Next slot to pop.                    */
  uint16_t              tail;                           /**< Next slot to push.                   */
  uint16_t              count;                          /**< In-flight packet count.              */
  ra8_usb_pal_packet_t  ring[k_ra8_usb_pal_ring_slots]; /**< Per-EP queue.                        */
} ra8_usb_pal_ep_slot_t;

/**
 * @struct ra8_usb_pal_state_inner_t
 * @brief Singleton PAL state.
 */
typedef struct {
  ra8_usb_speed_t        speed;                           /**< FS or HS, set at init.       */
  ra8_usb_pal_state_t    state;                           /**< Detached/attached/...        */
  ra8_usb_pal_event_fn_t event_fn;                        /**< Stack-installed callback.    */
  void*                  event_ctx;                       /**< Callback context.            */
  bool                   initialized;                     /**< True after ra8_usb_pal_init. */
  ra8_usb_pal_ep_slot_t  eps[k_ra8_usb_pal_ep_table_len]; /**< Per-EP state.                */
} ra8_usb_pal_state_inner_t;

static ra8_usb_pal_state_inner_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Copy ``len`` bytes (string.h-free local memcpy substitute).
 *
 * @details
 * The codebase avoids ``string.h`` to keep clang-tidy's
 * insecureAPI.DeprecatedOrUnsafeBufferHandling check quiet; this
 * loop fills the same role for small fixed buffers.
 *
 * @param[out] dst Destination buffer; must hold at least ``len`` bytes.
 * @param[in]  src Source buffer; must hold at least ``len`` bytes.
 * @param[in]  len Number of bytes to copy.
 *
 * @pre ``dst`` and ``src`` are non-NULL.
 * @pre ``dst`` and ``src`` do not overlap.
 * @post ``dst[0..len-1] == src[0..len-1]``.
 * @post No other state is mutated.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_copy_bytes(uint8_t* dst, const uint8_t* src, uint16_t len)
{
  for (uint16_t i = 0U; i < len; ++i) {
    dst[i] = src[i];
  }
}

/**
 * @brief Reset every endpoint slot to empty / unopened.
 *
 * @details
 * Walks the per-endpoint table, marks each slot ``opened == false``,
 * resets its OUT/control defaults, and zeroes the per-EP ring
 * cursors and packet lengths.
 *
 * @pre Caller holds the PAL single-thread lock (init/deinit context).
 * @pre ``s_state`` storage is mapped and writable.
 * @post Every ``s_state.eps[i].opened == false``.
 * @post Every per-EP ring is cursor-reset and length-zeroed.
 *
 * @note Not thread-safe; call only from init/deinit paths.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_reset_eps(void)
{
  for (uint16_t i = 0U; i < k_ra8_usb_pal_ep_table_len; ++i) {
    ra8_usb_pal_ep_slot_t* slot = &s_state.eps[i];
    slot->opened                = false;
    slot->dir                   = k_ra8_usb_pal_ep_dir_out;
    slot->type                  = k_ra8_usb_pal_ep_type_control;
    slot->max_packet            = 0U;
    slot->head                  = 0U;
    slot->tail                  = 0U;
    slot->count                 = 0U;
    for (uint16_t j = 0U; j < k_ra8_usb_pal_ring_slots; ++j) {
      slot->ring[j].len = 0U;
    }
  }
}

/**
 * @brief Translate ra8_usb status mask -> PAL event mask.
 *
 * @details
 * Today the mapping is "any non-zero ra8_usb status bit becomes an
 * error event"; later waves will fan the bits out into per-EP and
 * bus-event masks.
 *
 * @param[in] usb_mask Raw status mask published by ``ra8_usb``.
 *
 * @return PAL-side event mask suitable for the stack callback.
 * @retval k_ra8_usb_pal_event_none  ``usb_mask`` was zero.
 * @retval k_ra8_usb_pal_event_error ``usb_mask`` had any bit set.
 *
 * @pre ``usb_mask`` may take any uint16_t value.
 * @pre No global state is read.
 * @post No state is modified.
 * @post Return value reflects the translation only.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_translate(uint16_t usb_mask)
{
  if (usb_mask != 0U) {
    return k_ra8_usb_pal_event_error;
  }
  return k_ra8_usb_pal_event_none;
}

/**
 * @brief ra8_usb event handler -- translate + forward to the PAL callback.
 *
 * @details
 * Installed via ``ra8_usb_attach_handler`` during ::ra8_usb_pal_init.
 * Drops events while the PAL is uninitialized or arriving from a
 * different speed than the one negotiated, then translates the raw
 * status mask via ::internal_translate and forwards non-zero
 * results to the stack callback.
 *
 * @param[in] ctx         Opaque context (unused -- PAL is a singleton).
 * @param[in] speed       Speed reported by the ra8_usb driver.
 * @param[in] status_mask Raw ra8_usb status bits.
 *
 * @pre Invoked from ``ra8_usb`` ISR or task context.
 * @pre ``s_state`` storage is mapped and readable.
 * @post No PAL state is mutated.
 * @post Stack callback is invoked at most once per call.
 *
 * @note Treat as not thread-safe; do not call back into the PAL
 *       from inside the callback.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_usb_event(void* ctx, ra8_usb_speed_t speed, uint16_t status_mask)
{
  (void)ctx;
  if (!s_state.initialized) {
    return;
  }
  if (speed != s_state.speed) {
    return;
  }
  const uint16_t pal_mask = internal_translate(status_mask);
  if (priv_usb_pal_should_dispatch_event((const void*)s_state.event_fn,
                                         pal_mask,
                                         (uint16_t)k_ra8_usb_pal_event_none)) {
    s_state.event_fn(s_state.event_ctx, speed, pal_mask);
  }
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Bring up the USB PAL singleton at the requested speed.
 *
 * @details
 * Validates the speed, powers up the ``ra8_usb`` device-mode driver,
 * resets every endpoint slot, and installs the internal event
 * handler so the stack callback can fire.
 *
 * @param[in] speed FS or HS speed selector.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  PAL ready, state = detached.
 * @retval k_ra8_err_invalid_arg     ``speed`` not FS or HS.
 * @retval k_ra8_err_hw_init_failed  ``ra8_usb_device_init`` failed.
 *
 * @pre ``ra8_mstp_init`` and ``ra8_pwr_init`` have been called.
 * @pre IRQs masked or single-threaded init context.
 * @post On success, ``s_state.initialized == true`` and every EP
 *       slot is in the unopened state.
 * @post On failure, ``s_state.initialized == false`` and the
 *       underlying ra8_usb driver has been torn down.
 *
 * @note Not thread-safe; must run from boot init context.
 * @see ra8_usb_pal_deinit
 * @since 0.1.0
 */
ra8_err_t ra8_usb_pal_init(ra8_usb_speed_t speed)
{
  if ((speed != k_ra8_usb_speed_fs) && (speed != k_ra8_usb_speed_hs)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t usb_err = ra8_usb_device_init(speed);
  if (usb_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "ra8_usb_device_init failed", (uint32_t)usb_err);
    return k_ra8_err_hw_init_failed;
  }

  s_state.speed       = speed;
  s_state.state       = k_ra8_usb_pal_state_detached;
  s_state.event_fn    = nullptr;
  s_state.event_ctx   = nullptr;
  s_state.initialized = true;
  internal_reset_eps();

  ra8_usb_attach_handler(speed, internal_usb_event, nullptr);

  ra8_log_info(s_tag, "PAL ready");
  return k_ra8_ok;
}

/**
 * @brief Tear down the USB PAL singleton.
 *
 * @details
 * Detaches the device, removes the ra8_usb event handler, deinits
 * the underlying driver, clears stack callback state, marks the
 * state as detached, and resets every endpoint slot.
 *
 * @return ``ra8_err_t`` error code from ``ra8_usb_device_deinit``.
 * @retval k_ra8_ok                  Released cleanly.
 * @retval k_ra8_err_invalid_state   PAL was never initialized.
 *
 * @pre IRQs masked or single-threaded shutdown context.
 * @pre PAL was previously initialized (otherwise returns invalid_state).
 * @post ``s_state.initialized == false``.
 * @post Subsequent EP calls return ``k_ra8_err_invalid_state``.
 *
 * @note Not thread-safe.
 * @see ra8_usb_pal_init
 * @since 0.1.0
 */
ra8_err_t ra8_usb_pal_deinit(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  (void)ra8_usb_device_attach(s_state.speed, false);
  ra8_usb_attach_handler(s_state.speed, nullptr, nullptr);
  const ra8_err_t err = ra8_usb_device_deinit(s_state.speed);
  s_state.initialized = false;
  s_state.event_fn    = nullptr;
  s_state.event_ctx   = nullptr;
  s_state.state       = k_ra8_usb_pal_state_detached;
  internal_reset_eps();
  return err;
}

/**
 * @brief Drive the D+ pull-up to attach or detach the device.
 *
 * @details
 * Wraps ``ra8_usb_device_attach`` and updates the cached PAL state
 * so subsequent ::ra8_usb_pal_get_state calls reflect the request.
 *
 * @param[in] attached true to assert attach, false to detach.
 *
 * @return ``ra8_err_t`` error code from the underlying driver.
 * @retval k_ra8_ok                  Attach state updated.
 * @retval k_ra8_err_invalid_state   PAL not initialized.
 *
 * @pre PAL has been initialized.
 * @pre Bus is in a stable enumeration state (host connected if attaching).
 * @post On success, ``s_state.state`` reflects ``attached``.
 * @post On error, no PAL state is mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_pal_attach(bool attached)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  /* `ra8_usb_device_attach` rejects only an invalid controller selector;
   * successful initialization stored a validated selector in `s_state`. */
  (void)ra8_usb_device_attach(s_state.speed, attached);
  if (attached) {
    s_state.state = k_ra8_usb_pal_state_attached;
  } else {
    s_state.state = k_ra8_usb_pal_state_detached;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_usb_pal_get_state(ra8_usb_pal_state_t* out_state)
{
  RA8_CHECK_NULL_PTR(out_state, s_tag, "get_state: out_state");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  *out_state = s_state.state;
  return k_ra8_ok;
}

/**
 * @brief Open one endpoint and reserve a per-EP packet ring.
 *
 * @details
 * Records direction, transfer type, and max packet size in the
 * per-EP slot; resets the slot's queue cursors so subsequent
 * ::ra8_usb_pal_ep_send / ::ra8_usb_pal_ep_recv start at slot 0.
 *
 * @param[in] ep_addr    Endpoint address (1..k_ra8_usb_pal_ep_max).
 * @param[in] dir        OUT or IN direction.
 * @param[in] type       Control / bulk / iso / intr transfer type.
 * @param[in] max_packet Maximum packet size; 1..k_ra8_usb_pal_xfer_max.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Endpoint opened.
 * @retval k_ra8_err_invalid_state   PAL not initialized.
 * @retval k_ra8_err_invalid_arg     Address, direction, type, or max_packet
 *                                  out of range.
 *
 * @pre PAL has been initialized.
 * @pre Endpoint is not currently opened (re-opens are allowed and reset state).
 * @post On success, the slot is opened and the per-EP ring is empty.
 * @post On error, no slot state is mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_pal_ep_open(uint8_t               ep_addr,
                              ra8_usb_pal_ep_dir_t  dir,
                              ra8_usb_pal_ep_type_t type,
                              uint16_t              max_packet)
{
  /* USB descriptor bEndpointAddress encodes direction in bit 7
   * (0x80 = IN). Strip it so callers can pass either the descriptor
   * form (e.g. 0x83) or the bare endpoint number (e.g. 3). */
  ep_addr = (uint8_t)(ep_addr & (uint8_t)k_ra8_usb_pal_ep_addr_mask);
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (priv_usb_pal_ep_out_of_range(ep_addr, (uint8_t)k_ra8_usb_pal_ep_max)) {
    return k_ra8_err_invalid_arg;
  }
  if ((dir != k_ra8_usb_pal_ep_dir_out) && (dir != k_ra8_usb_pal_ep_dir_in)) {
    return k_ra8_err_invalid_arg;
  }
  if ((type > k_ra8_usb_pal_ep_type_intr) || (max_packet == 0U) ||
      (max_packet > k_ra8_usb_pal_xfer_max)) {
    return k_ra8_err_invalid_arg;
  }
  ra8_usb_pal_ep_slot_t* slot = &s_state.eps[ep_addr];
  slot->opened                = true;
  slot->dir                   = dir;
  slot->type                  = type;
  slot->max_packet            = max_packet;
  slot->head                  = 0U;
  slot->tail                  = 0U;
  slot->count                 = 0U;
  for (uint16_t j = 0U; j < k_ra8_usb_pal_ring_slots; ++j) {
    slot->ring[j].len = 0U;
  }
  return k_ra8_ok;
}

/**
 * @brief Queue a packet for transmit on an open IN endpoint.
 *
 * @details
 * Copies ``data[0..len-1]`` into the next free slot of the per-EP
 * ring. Fires ``k_ra8_usb_pal_event_ep_in`` when an event handler
 * is installed. ``len == 0`` is allowed (zero-length packet); when
 * ``len == 0`` the ``data`` pointer may be NULL.
 *
 * @param[in] ep_addr Endpoint address (1..k_ra8_usb_pal_ep_max).
 * @param[in] data    Packet bytes; non-NULL when ``len > 0``.
 * @param[in] len     Packet length in bytes (<= max_packet).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Packet queued.
 * @retval k_ra8_err_invalid_state   PAL not initialized or EP not opened.
 * @retval k_ra8_err_null_ptr        ``data`` NULL with ``len > 0``.
 * @retval k_ra8_err_invalid_arg     Address out of range or length above limit.
 * @retval k_ra8_err_no_mem          Per-EP ring full; drain and retry.
 *
 * @pre PAL has been initialized.
 * @pre Endpoint was opened via ::ra8_usb_pal_ep_open.
 * @post On success, the per-EP ring depth is incremented by one.
 * @post On error, no slot state is mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_pal_ep_send(uint8_t ep_addr, const uint8_t* data, uint16_t len)
{
  /* Accept descriptor-shaped (0x80 | ep) and bare-number ep_addr. */
  ep_addr = (uint8_t)(ep_addr & (uint8_t)k_ra8_usb_pal_ep_addr_mask);
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (priv_usb_pal_ep_out_of_range(ep_addr, (uint8_t)k_ra8_usb_pal_ep_max)) {
    return k_ra8_err_invalid_arg;
  }
  if ((len > k_ra8_usb_pal_xfer_max) || ((data == nullptr) && (len != 0U))) {
    return (data == nullptr) ? k_ra8_err_null_ptr : k_ra8_err_invalid_arg;
  }
  ra8_usb_pal_ep_slot_t* slot = &s_state.eps[ep_addr];
  if (!slot->opened) {
    return k_ra8_err_invalid_state;
  }
  if (len > slot->max_packet) {
    return k_ra8_err_invalid_arg;
  }
  if (slot->count >= k_ra8_usb_pal_ring_slots) {
    return k_ra8_err_no_mem;
  }
  ra8_usb_pal_packet_t* pkt = &slot->ring[slot->tail];
  if (len > 0U) {
    internal_copy_bytes(pkt->data, data, len);
  }
  pkt->len   = len;
  slot->tail = (uint16_t)((slot->tail + 1U) % k_ra8_usb_pal_ring_slots);
  ++slot->count;
  if (s_state.event_fn != nullptr) {
    s_state.event_fn(s_state.event_ctx, s_state.speed, k_ra8_usb_pal_event_ep_in);
  }
  return k_ra8_ok;
}

/**
 * @brief Pull the next packet, if any, from an open OUT endpoint.
 *
 * @details
 * Copies up to ``*inout_len`` bytes from the head of the per-EP ring
 * into ``out_buf`` and updates ``*inout_len`` with the byte count
 * actually written. When the ring is empty the call returns
 * ``k_ra8_err_no_data`` so the stack can poll without blocking.
 *
 * @param[in]     ep_addr   Endpoint address (1..k_ra8_usb_pal_ep_max).
 * @param[out]    out_buf   Destination buffer.
 * @param[in,out] inout_len On entry: capacity of ``out_buf``.
 *                          On exit: bytes written.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Packet copied.
 * @retval k_ra8_err_no_data         Ring empty.
 * @retval k_ra8_err_null_ptr        ``out_buf`` or ``inout_len`` NULL.
 * @retval k_ra8_err_invalid_state   PAL not initialized or EP not opened.
 * @retval k_ra8_err_invalid_arg     Address out of range or capacity zero.
 *
 * @pre PAL has been initialized.
 * @pre Endpoint was opened via ::ra8_usb_pal_ep_open.
 * @post On success, the per-EP ring depth is decremented by one.
 * @post On error, no slot state is mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_pal_ep_recv(uint8_t ep_addr, uint8_t* out_buf, uint16_t* inout_len)
{
  RA8_CHECK_NULL_PTR(out_buf, s_tag, "ep_recv: out_buf");
  RA8_CHECK_NULL_PTR(inout_len, s_tag, "ep_recv: inout_len");
  /* Accept descriptor-shaped (0x80 | ep) and bare-number ep_addr. */
  ep_addr = (uint8_t)(ep_addr & (uint8_t)k_ra8_usb_pal_ep_addr_mask);
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (priv_usb_pal_ep_out_of_range(ep_addr, (uint8_t)k_ra8_usb_pal_ep_max)) {
    return k_ra8_err_invalid_arg;
  }
  if (*inout_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  ra8_usb_pal_ep_slot_t* slot = &s_state.eps[ep_addr];
  if (!slot->opened) {
    return k_ra8_err_invalid_state;
  }
  if (slot->count == 0U) {
    return k_ra8_err_no_data;
  }
  ra8_usb_pal_packet_t* pkt = &slot->ring[slot->head];
  const uint16_t        n   = (pkt->len < *inout_len) ? pkt->len : *inout_len;
  if (n > 0U) {
    internal_copy_bytes(out_buf, pkt->data, n);
  }
  *inout_len = n;
  pkt->len   = 0U;
  slot->head = (uint16_t)((slot->head + 1U) % k_ra8_usb_pal_ring_slots);
  --slot->count;
  return k_ra8_ok;
}

/**
 * @brief Install a single event handler for bus and per-EP events.
 *
 * @details
 * Replaces any previously installed callback. Pass ``fn == nullptr``
 * to detach. The callback is invoked from ra8_usb ISR/task context
 * via ::internal_usb_event and from the per-EP send/recv hot path.
 *
 * @param[in] fn  Event callback, or NULL to detach.
 * @param[in] ctx Opaque context handed back to ``fn``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Handler installed/cleared.
 * @retval k_ra8_err_invalid_state   PAL not initialized.
 *
 * @pre PAL has been initialized.
 * @pre ``fn`` is callable from ISR context if it is non-NULL.
 * @post ``s_state.event_fn == fn`` and ``s_state.event_ctx == ctx``.
 * @post No other PAL state is mutated.
 *
 * @note Not thread-safe with respect to a concurrent event delivery.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_pal_set_event_handler(ra8_usb_pal_event_fn_t fn, void* ctx)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  s_state.event_fn  = fn;
  s_state.event_ctx = ctx;
  return k_ra8_ok;
}
