/**
 * @file ra8_net_pal.c
 * @brief Network PAL implementation -- ra8_eth wrapper
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * PAL over the Ring-3 ``ra8_eth`` driver. The PAL owns a
 * small in-memory TX/RX ring the stack drains with the send/recv
 * primitives. On real hardware the ring would be backed by the
 * GWCA descriptor engine; today the ring is a contiguous RAM
 * buffer large enough for Ethernet loopback tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_net_pal.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_eth.h"
#include "ra8_log.h"

/**
 * @brief Copy ``len`` bytes byte-by-byte (string.h-free implementation).
 *
 * @details
 * The codebase avoids ``string.h`` to dodge clang-tidy's
 * insecureAPI.DeprecatedOrUnsafeBufferHandling check; this loop is
 * the project-local replacement for ``memcpy`` on small fixed
 * buffers like the 6-byte MAC.
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
 * @brief Zero ``len`` bytes (project-local replacement for memset).
 *
 * @details
 * Same rationale as ::internal_copy_bytes -- string.h is excluded
 * by clang-tidy policy, so a hand-rolled loop fills small buffers.
 *
 * @param[out] dst Destination buffer; must hold at least ``len`` bytes.
 * @param[in]  len Number of bytes to clear.
 *
 * @pre ``dst`` is non-NULL.
 * @pre ``len`` accurately describes the writable extent of ``dst``.
 * @post ``dst[0..len-1] == 0``.
 * @post No other state is mutated.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_zero_bytes(uint8_t* dst, uint16_t len)
{
  for (uint16_t i = 0U; i < len; ++i) {
    dst[i] = 0U;
  }
}

static const char* s_tag = "NETPAL";

/* =============================================================================
 * Ring buffer sizing
 * =============================================================================
 */

typedef enum : uint16_t {
  k_ra8_net_pal_ring_slots = 4U, /**< Number of in-flight frames. */
} ra8_net_pal_ring_dim_t;

/* =============================================================================
 * Per-PAL state (single instance -- one ESWM per chip)
 * =============================================================================
 */

/**
 * @struct ra8_net_pal_slot_t
 * @brief One ring slot holding a single frame plus its byte length.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t len;                           /**< 0 == slot empty.  */
  uint8_t  data[k_ra8_net_pal_frame_max]; /**< Up to 1518 bytes. */
} ra8_net_pal_slot_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra8_net_pal_state_t
 * @brief Singleton PAL state.
 */
typedef struct {
  ra8_net_pal_mac_t        mac;                            /**< Stored MAC address.          */
  ra8_net_pal_link_state_t link_state;                     /**< Last observed link state.    */
  ra8_net_pal_event_fn_t   event_fn;                       /**< Async event callback.        */
  void*                    event_ctx;                      /**< Callback context.            */
  bool                     initialized;                    /**< True after ra8_net_pal_init. */
  ra8_net_pal_slot_t       ring[k_ra8_net_pal_ring_slots]; /**< RX/TX ring.                  */
  uint16_t                 head;                           /**< Next slot to pop (recv).     */
  uint16_t                 tail;                           /**< Next slot to push (send).    */
  uint16_t                 count;                          /**< In-flight frame count.       */
} ra8_net_pal_state_t;

static ra8_net_pal_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Reset the software ring to empty.
 *
 * @details
 * Clears the head/tail/count cursors and zeroes every slot's
 * ``len`` field so a subsequent ::ra8_net_pal_send_frame starts at
 * slot 0.
 *
 * @pre Caller holds the PAL single-thread lock (init/deinit context).
 * @pre ``s_state`` storage is mapped and writable.
 * @post ``s_state.head == s_state.tail == s_state.count == 0``.
 * @post Every ``s_state.ring[i].len == 0``.
 *
 * @note Not thread-safe; call only from init/deinit paths.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_ring_reset(void)
{
  s_state.head  = 0U;
  s_state.tail  = 0U;
  s_state.count = 0U;
  for (uint16_t i = 0U; i < k_ra8_net_pal_ring_slots; ++i) {
    s_state.ring[i].len = 0U;
  }
}

/**
 * @brief Translate ra8_eth status bits into PAL event bits.
 *
 * @details
 * Today the mapping is "any non-zero ra8_eth status bit becomes an
 * error event"; future waves will fan the bits out into the
 * link/RX/TX event taxonomy.
 *
 * @param[in] eth_mask Raw status mask published by ``ra8_eth``.
 *
 * @return PAL-side event mask suitable for the stack callback.
 * @retval k_ra8_net_pal_event_none  ``eth_mask`` was zero.
 * @retval k_ra8_net_pal_event_error ``eth_mask`` had any bit set.
 *
 * @pre ``eth_mask`` may take any uint32_t value.
 * @pre No global state is read.
 * @post No state is modified.
 * @post Return value reflects the translation only.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_translate_event(uint32_t eth_mask)
{
  if (eth_mask != 0U) {
    return k_ra8_net_pal_event_error;
  }
  return k_ra8_net_pal_event_none;
}

/**
 * @brief ra8_eth event handler -- translate + forward to the PAL callback.
 *
 * @details
 * Installed via ``ra8_eth_attach_handler`` during ::ra8_net_pal_init.
 * Drops events while the PAL is uninitialized, then translates the
 * raw status mask via ::internal_translate_event and forwards
 * non-zero results to the stack-installed callback.
 *
 * @param[in] ctx         Opaque context (unused -- PAL is a singleton).
 * @param[in] status_mask Raw ``ra8_eth`` status bits.
 *
 * @pre Invoked from ``ra8_eth`` ISR or task context.
 * @pre ``s_state`` storage is mapped and readable.
 * @post No PAL state is mutated.
 * @post Stack callback may have been invoked at most once per call.
 *
 * @note Reentrant only with respect to a different ra8_net_pal instance,
 *       which does not exist; treat as not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_eth_event(void* ctx, uint32_t status_mask)
{
  (void)ctx;
  if (!s_state.initialized) {
    return;
  }
  const uint32_t pal_mask = internal_translate_event(status_mask);
  // mcdc-deactivated: TU-local helper internal_eth_event dispatch gate; tests/test_ra8_net_pal_event_dispatch covers each branch outcome but the MC/DC vector that flips event_fn while holding pal_mask non-empty (or vice-versa) is not reachable through the public-API surface -- callbacks are registered/unregistered before any event mask can become non-zero.
  if ((s_state.event_fn != nullptr) && (pal_mask != k_ra8_net_pal_event_none)) {
    s_state.event_fn(s_state.event_ctx, pal_mask);
  }
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Bring up the network PAL singleton.
 *
 * @details
 * Powers up the underlying ``ra8_eth`` driver, programmes the supplied
 * MAC (or leaves it zero), resets the in-memory ring, and installs
 * the internal ra8_eth event handler so the stack callback can fire.
 *
 * @param[in] mac MAC descriptor to programme; may be NULL to keep
 *                the all-zero default.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                   PAL ready, link state = down.
 * @retval k_ra8_err_hw_init_failed   ``ra8_eth_init`` or the handler
 *                                   attach failed.
 *
 * @pre ``ra8_mstp_init`` and ``ra8_pwr_init`` have been called.
 * @pre IRQs masked or single-threaded init context.
 * @post On success, ``s_state.initialized == true`` and the ring
 *       is empty.
 * @post On failure, ``s_state.initialized == false`` and ra8_eth has
 *       been torn down.
 *
 * @note Not thread-safe; must run from boot init context.
 * @see ra8_net_pal_deinit
 * @since 0.1.0
 */
ra8_err_t ra8_net_pal_init(const ra8_net_pal_mac_t* mac)
{
  const ra8_err_t eth_err = ra8_eth_init();
  if (eth_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "ra8_eth_init failed", (uint32_t)eth_err);
    return k_ra8_err_hw_init_failed;
  }

  internal_zero_bytes(s_state.mac.bytes, k_ra8_net_pal_mac_addr_len);
  s_state.link_state  = k_ra8_net_pal_link_down;
  s_state.event_fn    = nullptr;
  s_state.event_ctx   = nullptr;
  s_state.initialized = true;
  internal_ring_reset();

  if (mac != nullptr) {
    internal_copy_bytes(s_state.mac.bytes, mac->bytes, k_ra8_net_pal_mac_addr_len);
  }

  const ra8_err_t att_err = ra8_eth_attach_handler(internal_eth_event, nullptr);
  if (att_err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    ra8_log_error_val(s_tag, "ra8_eth_attach_handler failed", (uint32_t)att_err);
    s_state.initialized = false;
    (void)ra8_eth_deinit();
    return k_ra8_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

  ra8_log_info(s_tag, "PAL ready");
  return k_ra8_ok;
}

/**
 * @brief Tear down the network PAL singleton.
 *
 * @details
 * Detaches the ra8_eth handler, releases the underlying driver,
 * clears the event callback, marks the link as down, and resets
 * the in-memory ring.
 *
 * @return ``ra8_err_t`` error code from ``ra8_eth_deinit``.
 * @retval k_ra8_ok                   Released cleanly.
 * @retval k_ra8_err_invalid_state    PAL was never initialized.
 *
 * @pre IRQs masked or single-threaded shutdown context.
 * @pre PAL was previously initialized (otherwise returns invalid_state).
 * @post ``s_state.initialized == false``.
 * @post Subsequent send/recv calls return ``k_ra8_err_invalid_state``.
 *
 * @note Not thread-safe.
 * @see ra8_net_pal_init
 * @since 0.1.0
 */
ra8_err_t ra8_net_pal_deinit(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  (void)ra8_eth_attach_handler(nullptr, nullptr);
  const ra8_err_t err = ra8_eth_deinit();
  s_state.initialized = false;
  s_state.event_fn    = nullptr;
  s_state.event_ctx   = nullptr;
  s_state.link_state  = k_ra8_net_pal_link_down;
  internal_ring_reset();
  return err;
}

/**
 * @brief Programme the PAL MAC address.
 *
 * @details
 * Updates the in-memory MAC. When ra8_eth gains MAC-write support
 * the same call will also update the ESWM hardware filter.
 *
 * @param[in] mac Non-NULL MAC descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  MAC stored.
 * @retval k_ra8_err_null_ptr        ``mac`` was NULL.
 * @retval k_ra8_err_invalid_state   PAL not initialized.
 *
 * @pre ``mac`` is non-NULL.
 * @pre PAL has been initialized.
 * @post ``s_state.mac`` mirrors the supplied descriptor.
 * @post No other PAL state is mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_net_pal_set_mac_addr(const ra8_net_pal_mac_t* mac)
{
  RA8_CHECK_NULL_PTR(mac, s_tag, "set_mac_addr: mac");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  internal_copy_bytes(s_state.mac.bytes, mac->bytes, k_ra8_net_pal_mac_addr_len);
  return k_ra8_ok;
}

/**
 * @brief Read the currently programmed MAC address.
 *
 * @details Copies ``s_state.mac`` into the caller buffer.
 *
 * @param[out] out_mac Receives the MAC descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  MAC copied.
 * @retval k_ra8_err_null_ptr        ``out_mac`` was NULL.
 * @retval k_ra8_err_invalid_state   PAL not initialized.
 *
 * @pre ``out_mac`` is non-NULL.
 * @pre PAL has been initialized.
 * @post ``out_mac`` holds the current MAC.
 * @post No PAL state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_net_pal_get_mac_addr(ra8_net_pal_mac_t* out_mac)
{
  RA8_CHECK_NULL_PTR(out_mac, s_tag, "get_mac_addr: out_mac");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  internal_copy_bytes(out_mac->bytes, s_state.mac.bytes, k_ra8_net_pal_mac_addr_len);
  return k_ra8_ok;
}

/**
 * @brief Hand a complete ethernet frame to the MAC for transmit.
 *
 * @details
 * Copies ``frame[0..len-1]`` into the next free TX ring slot. On
 * real hardware the slot would be a GWCA descriptor; in the host
 * build it is a plain RAM buffer the PAL also exposes through
 * ::ra8_net_pal_recv_frame for loopback tests. Fires the
 * ``k_ra8_net_pal_event_tx_done`` event after enqueue when an event
 * handler is installed.
 *
 * @param[in] frame Ethernet frame bytes (header + payload, no FCS).
 * @param[in] len   Frame length in bytes; non-zero, <= frame_max.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Frame queued.
 * @retval k_ra8_err_null_ptr        ``frame`` was NULL.
 * @retval k_ra8_err_invalid_arg     ``len`` zero or above ``k_ra8_net_pal_frame_max``.
 * @retval k_ra8_err_invalid_state   PAL not initialized.
 * @retval k_ra8_err_no_mem          TX ring is full; retry after drain.
 *
 * @pre ``frame`` is non-NULL.
 * @pre PAL has been initialized.
 * @post On success, ``s_state.count`` is incremented by one.
 * @post On error, no ring state is mutated.
 *
 * @note Not thread-safe.
 * @see ra8_net_pal_recv_frame
 * @since 0.1.0
 */
ra8_err_t ra8_net_pal_send_frame(const uint8_t* frame, uint16_t len)
{
  RA8_CHECK_NULL_PTR(frame, s_tag, "send_frame: frame");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if ((len == 0U) || (len > k_ra8_net_pal_frame_max)) {
    return k_ra8_err_invalid_arg;
  }
  if (s_state.count >= k_ra8_net_pal_ring_slots) {
    return k_ra8_err_no_mem;
  }
  ra8_net_pal_slot_t* slot = &s_state.ring[s_state.tail];
  internal_copy_bytes(slot->data, frame, len);
  slot->len    = len;
  s_state.tail = (uint16_t)((s_state.tail + 1U) % k_ra8_net_pal_ring_slots);
  ++s_state.count;
  if ((s_state.event_fn != nullptr)) {
    s_state.event_fn(s_state.event_ctx, k_ra8_net_pal_event_tx_done);
  }
  return k_ra8_ok;
}

/**
 * @brief Pull the next received ethernet frame, if any, into a buffer.
 *
 * @details
 * If a frame is available it is copied into ``out_buf`` and
 * ``*inout_len`` is set to the byte count actually written. When
 * no frame is queued the function returns ``k_ra8_err_no_data`` so
 * callers can poll without blocking.
 *
 * @param[out]    out_buf   Destination buffer, sized at least
 *                          ``k_ra8_net_pal_frame_max`` bytes.
 * @param[in,out] inout_len On entry: capacity of ``out_buf``.
 *                          On exit: bytes written.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Frame copied.
 * @retval k_ra8_err_no_data        No frame ready.
 * @retval k_ra8_err_null_ptr       ``out_buf`` or ``inout_len`` NULL.
 * @retval k_ra8_err_invalid_state  PAL not initialized.
 * @retval k_ra8_err_invalid_arg    ``*inout_len`` < ``k_ra8_net_pal_frame_max``.
 *
 * @pre ``out_buf`` and ``inout_len`` are non-NULL.
 * @pre ``*inout_len`` >= ``k_ra8_net_pal_frame_max``.
 * @post On success, the consumed slot is freed and ``s_state.count`` decremented.
 * @post On error, no ring state is mutated.
 *
 * @note Not thread-safe.
 * @see ra8_net_pal_send_frame
 * @since 0.1.0
 */
ra8_err_t ra8_net_pal_recv_frame(uint8_t* out_buf, uint16_t* inout_len)
{
  RA8_CHECK_NULL_PTR(out_buf, s_tag, "recv_frame: out_buf");
  RA8_CHECK_NULL_PTR(inout_len, s_tag, "recv_frame: inout_len");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (*inout_len < k_ra8_net_pal_frame_max) {
    return k_ra8_err_invalid_arg;
  }
  if (s_state.count == 0U) {
    return k_ra8_err_no_data;
  }
  ra8_net_pal_slot_t* slot = &s_state.ring[s_state.head];
  const uint16_t      n    = slot->len;
  internal_copy_bytes(out_buf, slot->data, n);
  *inout_len   = n;
  slot->len    = 0U;
  s_state.head = (uint16_t)((s_state.head + 1U) % k_ra8_net_pal_ring_slots);
  --s_state.count;
  return k_ra8_ok;
}

ra8_err_t ra8_net_pal_link_status(ra8_net_pal_link_state_t* out_state)
{
  RA8_CHECK_NULL_PTR(out_state, s_tag, "link_status: out_state");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  *out_state = s_state.link_state;
  return k_ra8_ok;
}

/**
 * @brief Install a single event handler for link / RX / TX events.
 *
 * @details
 * Replaces any previously installed callback. Pass ``fn == nullptr``
 * to detach. The callback fires from ra8_eth ISR/task context via
 * ::internal_eth_event and from the send/recv hot path.
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
ra8_err_t ra8_net_pal_set_event_handler(ra8_net_pal_event_fn_t fn, void* ctx)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  s_state.event_fn  = fn;
  s_state.event_ctx = ctx;
  return k_ra8_ok;
}
