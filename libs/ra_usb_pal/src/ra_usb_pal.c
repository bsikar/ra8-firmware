/**
 * @file ra_usb_pal.c
 * @brief USB PAL implementation -- ra_usb wrapper
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * PAL over the Ring-3 ``ra_usb`` driver. Endpoints are
 * backed by a small in-memory queue so the PAL is usable in host
 * tests (and any future software-only transport) before the real
 * ra_usb pipe primitives land. On hardware the queue is backed
 * by the controller's pipe FIFOs; the stack-facing contract is
 * identical in either case.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_usb_pal.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_usb.h"

static const char* s_tag = "USBPAL";

/* =============================================================================
 * Ring sizing
 * =============================================================================
 */

typedef enum : uint16_t {
  k_ra_usb_pal_ep_table_len = 11U,   /**< Index 0..ep_max, 1-based EPs. */
  k_ra_usb_pal_ring_slots   = 4U,    /**< Per-EP queue depth. */
  k_ra_usb_pal_pkt_max      = 1024U, /**< Per-packet capacity. */
} ra_usb_pal_ring_dim_t;

/* =============================================================================
 * Per-EP + per-PAL state
 * =============================================================================
 */

/**
 * @struct ra_usb_pal_packet_t
 * @brief One packet on a per-endpoint ring.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t len;                        /**< 0 == empty slot. */
  uint8_t  data[k_ra_usb_pal_pkt_max]; /**< Packet payload bytes. */
} ra_usb_pal_packet_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_usb_pal_ep_slot_t
 * @brief Configuration + queue for one endpoint.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  bool                 opened;                        /**< True once ra_usb_pal_ep_open fired. */
  ra_usb_pal_ep_dir_t  dir;                           /**< Stored direction. */
  ra_usb_pal_ep_type_t type;                          /**< Stored transfer type. */
  uint16_t             max_packet;                    /**< Stored max packet size. */
  uint16_t             head;                          /**< Next slot to pop. */
  uint16_t             tail;                          /**< Next slot to push. */
  uint16_t             count;                         /**< In-flight packet count. */
  ra_usb_pal_packet_t  ring[k_ra_usb_pal_ring_slots]; /**< Per-EP queue. */
} ra_usb_pal_ep_slot_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_usb_pal_state_inner_t
 * @brief Singleton PAL state.
 */
typedef struct {
  ra_usb_speed_t        speed;                          /**< FS or HS, set at init. */
  ra_usb_pal_state_t    state;                          /**< Detached/attached/... */
  ra_usb_pal_event_fn_t event_fn;                       /**< Stack-installed callback. */
  void*                 event_ctx;                      /**< Callback context. */
  bool                  initialised;                    /**< True after ra_usb_pal_init. */
  ra_usb_pal_ep_slot_t  eps[k_ra_usb_pal_ep_table_len]; /**< Per-EP state. */
} ra_usb_pal_state_inner_t;

static ra_usb_pal_state_inner_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

static void internal_copy_bytes(uint8_t* dst, const uint8_t* src, uint16_t len)
{
  for (uint16_t i = 0U; i < len; ++i) {
    dst[i] = src[i];
  }
}

/**
 * @brief Reset every endpoint slot to empty / unopened.
 */
static void internal_reset_eps(void)
{
  for (uint16_t i = 0U; i < (uint16_t)k_ra_usb_pal_ep_table_len; ++i) {
    ra_usb_pal_ep_slot_t* slot = &s_state.eps[i];
    slot->opened               = false;
    slot->dir                  = k_ra_usb_pal_ep_dir_out;
    slot->type                 = k_ra_usb_pal_ep_type_control;
    slot->max_packet           = 0U;
    slot->head                 = 0U;
    slot->tail                 = 0U;
    slot->count                = 0U;
    for (uint16_t j = 0U; j < (uint16_t)k_ra_usb_pal_ring_slots; ++j) {
      slot->ring[j].len = 0U;
    }
  }
}

/**
 * @brief Translate ra_usb status mask -> PAL event mask.
 */
static uint16_t internal_translate(uint16_t usb_mask)
{
  if (usb_mask != 0U) {
    return (uint16_t)k_ra_usb_pal_event_error;
  }
  return (uint16_t)k_ra_usb_pal_event_none;
}

static void internal_usb_event(void* ctx, ra_usb_speed_t speed, uint16_t status_mask)
{
  (void)ctx;
  if (!s_state.initialised) {
    return;
  }
  if (speed != s_state.speed) {
    return;
  }
  const uint16_t pal_mask = internal_translate(status_mask);
  if ((s_state.event_fn != nullptr) && (pal_mask != (uint16_t)k_ra_usb_pal_event_none)) {
    s_state.event_fn(s_state.event_ctx, speed, pal_mask);
  }
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra_err_t ra_usb_pal_init(ra_usb_speed_t speed)
{
  if ((speed != k_ra_usb_speed_fs) && (speed != k_ra_usb_speed_hs)) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t usb_err = ra_usb_device_init(speed);
  if (usb_err != k_ra_ok) {
    ra_log_error_val(s_tag, "ra_usb_device_init failed", (uint32_t)usb_err);
    return k_ra_err_hw_init_failed;
  }

  s_state.speed       = speed;
  s_state.state       = k_ra_usb_pal_state_detached;
  s_state.event_fn    = nullptr;
  s_state.event_ctx   = nullptr;
  s_state.initialised = true;
  internal_reset_eps();

  const ra_err_t att_err = ra_usb_attach_handler(internal_usb_event, nullptr);
  if (att_err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    ra_log_error_val(s_tag, "ra_usb_attach_handler failed", (uint32_t)att_err);
    s_state.initialised = false;
    (void)ra_usb_device_deinit(speed);
    return k_ra_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

  ra_log_info(s_tag, "PAL ready");
  return k_ra_ok;
}

ra_err_t ra_usb_pal_deinit(void)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  (void)ra_usb_device_attach(s_state.speed, false);
  (void)ra_usb_attach_handler(nullptr, nullptr);
  const ra_err_t err  = ra_usb_device_deinit(s_state.speed);
  s_state.initialised = false;
  s_state.event_fn    = nullptr;
  s_state.event_ctx   = nullptr;
  s_state.state       = k_ra_usb_pal_state_detached;
  internal_reset_eps();
  return err;
}

ra_err_t ra_usb_pal_attach(bool attached)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  const ra_err_t err = ra_usb_device_attach(s_state.speed, attached);
  if (err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    return err;
    /* GCOVR_EXCL_STOP */
  }
  if (attached) {
    s_state.state = k_ra_usb_pal_state_attached;
  } else {
    s_state.state = k_ra_usb_pal_state_detached;
  }
  return k_ra_ok;
}

ra_err_t ra_usb_pal_get_state(ra_usb_pal_state_t* out_state)
{
  RA_CHECK_NULL_PTR(out_state, s_tag, "get_state: out_state");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  *out_state = s_state.state;
  return k_ra_ok;
}

ra_err_t ra_usb_pal_ep_open(uint8_t              ep_addr,
                            ra_usb_pal_ep_dir_t  dir,
                            ra_usb_pal_ep_type_t type,
                            uint16_t             max_packet)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((ep_addr == 0U) || (ep_addr > (uint8_t)k_ra_usb_pal_ep_max)) {
    return k_ra_err_invalid_arg;
  }
  if ((dir != k_ra_usb_pal_ep_dir_out) && (dir != k_ra_usb_pal_ep_dir_in)) {
    return k_ra_err_invalid_arg;
  }
  if ((type > k_ra_usb_pal_ep_type_intr) || (max_packet == 0U) ||
      (max_packet > (uint16_t)k_ra_usb_pal_xfer_max)) {
    return k_ra_err_invalid_arg;
  }
  ra_usb_pal_ep_slot_t* slot = &s_state.eps[ep_addr];
  slot->opened               = true;
  slot->dir                  = dir;
  slot->type                 = type;
  slot->max_packet           = max_packet;
  slot->head                 = 0U;
  slot->tail                 = 0U;
  slot->count                = 0U;
  for (uint16_t j = 0U; j < (uint16_t)k_ra_usb_pal_ring_slots; ++j) {
    slot->ring[j].len = 0U;
  }
  return k_ra_ok;
}

ra_err_t ra_usb_pal_ep_send(uint8_t ep_addr, const uint8_t* data, uint16_t len)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((ep_addr == 0U) || (ep_addr > (uint8_t)k_ra_usb_pal_ep_max)) {
    return k_ra_err_invalid_arg;
  }
  if ((len > (uint16_t)k_ra_usb_pal_xfer_max) || ((data == nullptr) && (len != 0U))) {
    return (data == nullptr) ? k_ra_err_null_ptr : k_ra_err_invalid_arg;
  }
  ra_usb_pal_ep_slot_t* slot = &s_state.eps[ep_addr];
  if (!slot->opened) {
    return k_ra_err_invalid_state;
  }
  if (len > slot->max_packet) {
    return k_ra_err_invalid_arg;
  }
  if (slot->count >= (uint16_t)k_ra_usb_pal_ring_slots) {
    return k_ra_err_no_mem;
  }
  ra_usb_pal_packet_t* pkt = &slot->ring[slot->tail];
  if (len > 0U) {
    internal_copy_bytes(pkt->data, data, len);
  }
  pkt->len   = len;
  slot->tail = (uint16_t)((slot->tail + 1U) % (uint16_t)k_ra_usb_pal_ring_slots);
  ++slot->count;
  if (s_state.event_fn != nullptr) {
    s_state.event_fn(s_state.event_ctx, s_state.speed, (uint16_t)k_ra_usb_pal_event_ep_in);
  }
  return k_ra_ok;
}

ra_err_t ra_usb_pal_ep_recv(uint8_t ep_addr, uint8_t* out_buf, uint16_t* inout_len)
{
  RA_CHECK_NULL_PTR(out_buf, s_tag, "ep_recv: out_buf");
  RA_CHECK_NULL_PTR(inout_len, s_tag, "ep_recv: inout_len");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((ep_addr == 0U) || (ep_addr > (uint8_t)k_ra_usb_pal_ep_max)) {
    return k_ra_err_invalid_arg;
  }
  if (*inout_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  ra_usb_pal_ep_slot_t* slot = &s_state.eps[ep_addr];
  if (!slot->opened) {
    return k_ra_err_invalid_state;
  }
  if (slot->count == 0U) {
    return k_ra_err_no_data;
  }
  ra_usb_pal_packet_t* pkt = &slot->ring[slot->head];
  const uint16_t       n   = (pkt->len < *inout_len) ? pkt->len : *inout_len;
  if (n > 0U) {
    internal_copy_bytes(out_buf, pkt->data, n);
  }
  *inout_len = n;
  pkt->len   = 0U;
  slot->head = (uint16_t)((slot->head + 1U) % (uint16_t)k_ra_usb_pal_ring_slots);
  --slot->count;
  return k_ra_ok;
}

ra_err_t ra_usb_pal_set_event_handler(ra_usb_pal_event_fn_t fn, void* ctx)
{
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  s_state.event_fn  = fn;
  s_state.event_ctx = ctx;
  return k_ra_ok;
}
