/**
 * @file ra_net_pal.c
 * @brief Network PAL implementation -- ra_eth wrapper
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * PAL over the Ring-3 ``ra_eth`` driver. The PAL owns a
 * small in-memory TX/RX ring the stack drains with the send/recv
 * primitives. On real hardware the ring would be backed by the
 * GWCA descriptor engine; today the ring is a contiguous RAM
 * buffer large enough for Ethernet loopback tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_net_pal.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_eth.h"
#include "ra_log.h"

/**
 * @brief Copy ``len`` bytes byte-by-byte (string.h-free implementation).
 *
 * @details
 * The codebase avoids ``string.h`` to dodge clang-tidy's
 * insecureAPI.DeprecatedOrUnsafeBufferHandling check; this loop is
 * the project-local replacement for ``memcpy`` on small fixed
 * buffers like the 6-byte MAC.
 */
static void internal_copy_bytes(uint8_t* dst, const uint8_t* src, uint16_t len)
{
  for (uint16_t i = 0U; i < len; ++i) {
    dst[i] = src[i];
  }
}

/**
 * @brief Zero ``len`` bytes (project-local replacement for memset).
 */
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
  k_ra_net_pal_ring_slots = 4U, /**< Number of in-flight frames. */
} ra_net_pal_ring_dim_t;

/* =============================================================================
 * Per-PAL state (single instance -- one ESWM per chip)
 * =============================================================================
 */

/**
 * @struct ra_net_pal_slot_t
 * @brief One ring slot holding a single frame plus its byte length.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t len;                          /**< 0 == slot empty. */
  uint8_t  data[k_ra_net_pal_frame_max]; /**< Up to 1518 bytes. */
} ra_net_pal_slot_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_net_pal_state_t
 * @brief Singleton PAL state.
 */
typedef struct {
  ra_net_pal_mac_t        mac;                           /**< Stored MAC address. */
  ra_net_pal_link_state_t link_state;                    /**< Last observed link state. */
  ra_net_pal_event_fn_t   event_fn;                      /**< Async event callback. */
  void*                   event_ctx;                     /**< Callback context. */
  bool                    initialised;                   /**< True after ra_net_pal_init. */
  ra_net_pal_slot_t       ring[k_ra_net_pal_ring_slots]; /**< RX/TX ring. */
  uint16_t                head;                          /**< Next slot to pop (recv). */
  uint16_t                tail;                          /**< Next slot to push (send). */
  uint16_t                count;                         /**< In-flight frame count. */
} ra_net_pal_state_t;

static ra_net_pal_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Reset the software ring to empty.
 */
static void internal_ring_reset(void)
{
  s_state.head  = 0U;
  s_state.tail  = 0U;
  s_state.count = 0U;
  for (uint16_t i = 0U; i < k_ra_net_pal_ring_slots; ++i) {
    s_state.ring[i].len = 0U;
  }
}

/**
 * @brief Translate ra_eth status bits into PAL event bits.
 */
static uint32_t internal_translate_event(uint32_t eth_mask)
{
  if (eth_mask != 0U) {
    return k_ra_net_pal_event_error;
  }
  return k_ra_net_pal_event_none;
}

/**
 * @brief ra_eth event handler -- translate + forward to the PAL callback.
 */
static void internal_eth_event(void* ctx, uint32_t status_mask)
{
  (void)ctx;
  if (!s_state.initialised) {
    return;
  }
  const uint32_t pal_mask = internal_translate_event(status_mask);
  if ((s_state.event_fn != nullptr) && (pal_mask != k_ra_net_pal_event_none)) {
    s_state.event_fn(s_state.event_ctx, pal_mask);
  }
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra_err_t ra_net_pal_init(const ra_net_pal_mac_t* mac)
{
  const ra_err_t eth_err = ra_eth_init();
  if (eth_err != k_ra_ok) {
    ra_log_error_val(s_tag, "ra_eth_init failed", (uint32_t)eth_err);
    return k_ra_err_hw_init_failed;
  }

  internal_zero_bytes(s_state.mac.bytes, k_ra_net_pal_mac_addr_len);
  s_state.link_state  = k_ra_net_pal_link_down;
  s_state.event_fn    = nullptr;
  s_state.event_ctx   = nullptr;
  s_state.initialised = true;
  internal_ring_reset();

  if (mac != nullptr) {
    internal_copy_bytes(s_state.mac.bytes, mac->bytes, k_ra_net_pal_mac_addr_len);
  }

  const ra_err_t att_err = ra_eth_attach_handler(internal_eth_event, nullptr);
  if (att_err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    ra_log_error_val(s_tag, "ra_eth_attach_handler failed", (uint32_t)att_err);
    s_state.initialised = false;
    (void)ra_eth_deinit();
    return k_ra_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

  ra_log_info(s_tag, "PAL ready");
  return k_ra_ok;
}

ra_err_t ra_net_pal_deinit(void)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  (void)ra_eth_attach_handler(nullptr, nullptr);
  const ra_err_t err  = ra_eth_deinit();
  s_state.initialised = false;
  s_state.event_fn    = nullptr;
  s_state.event_ctx   = nullptr;
  s_state.link_state  = k_ra_net_pal_link_down;
  internal_ring_reset();
  return err;
}

ra_err_t ra_net_pal_set_mac_addr(const ra_net_pal_mac_t* mac)
{
  RA_CHECK_NULL_PTR(mac, s_tag, "set_mac_addr: mac");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  internal_copy_bytes(s_state.mac.bytes, mac->bytes, k_ra_net_pal_mac_addr_len);
  return k_ra_ok;
}

ra_err_t ra_net_pal_get_mac_addr(ra_net_pal_mac_t* out_mac)
{
  RA_CHECK_NULL_PTR(out_mac, s_tag, "get_mac_addr: out_mac");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  internal_copy_bytes(out_mac->bytes, s_state.mac.bytes, k_ra_net_pal_mac_addr_len);
  return k_ra_ok;
}

ra_err_t ra_net_pal_send_frame(const uint8_t* frame, uint16_t len)
{
  RA_CHECK_NULL_PTR(frame, s_tag, "send_frame: frame");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((len == 0U) || (len > k_ra_net_pal_frame_max)) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.count >= k_ra_net_pal_ring_slots) {
    return k_ra_err_no_mem;
  }
  ra_net_pal_slot_t* slot = &s_state.ring[s_state.tail];
  internal_copy_bytes(slot->data, frame, len);
  slot->len    = len;
  s_state.tail = (uint16_t)((s_state.tail + 1U) % k_ra_net_pal_ring_slots);
  ++s_state.count;
  if ((s_state.event_fn != nullptr)) {
    s_state.event_fn(s_state.event_ctx, k_ra_net_pal_event_tx_done);
  }
  return k_ra_ok;
}

ra_err_t ra_net_pal_recv_frame(uint8_t* out_buf, uint16_t* inout_len)
{
  RA_CHECK_NULL_PTR(out_buf, s_tag, "recv_frame: out_buf");
  RA_CHECK_NULL_PTR(inout_len, s_tag, "recv_frame: inout_len");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if (*inout_len < k_ra_net_pal_frame_max) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.count == 0U) {
    return k_ra_err_no_data;
  }
  ra_net_pal_slot_t* slot = &s_state.ring[s_state.head];
  const uint16_t     n    = slot->len;
  internal_copy_bytes(out_buf, slot->data, n);
  *inout_len   = n;
  slot->len    = 0U;
  s_state.head = (uint16_t)((s_state.head + 1U) % k_ra_net_pal_ring_slots);
  --s_state.count;
  return k_ra_ok;
}

ra_err_t ra_net_pal_link_status(ra_net_pal_link_state_t* out_state)
{
  RA_CHECK_NULL_PTR(out_state, s_tag, "link_status: out_state");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  *out_state = s_state.link_state;
  return k_ra_ok;
}

ra_err_t ra_net_pal_set_event_handler(ra_net_pal_event_fn_t fn, void* ctx)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  s_state.event_fn  = fn;
  s_state.event_ctx = ctx;
  return k_ra_ok;
}
