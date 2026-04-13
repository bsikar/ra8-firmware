/**
 * @file ra_net_pal.c
 * @brief Network PAL implementation -- ra_eth wrapper
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Wave 7.1 scaffold. Implements the ``ra_net_pal_*`` API by
 * wrapping the Ring-3 ``ra_eth`` driver. Frame I/O bottoms out in
 * ``k_ra_err_not_supported`` until ra_eth gains a TX/RX descriptor
 * ring (Wave 7.1b).
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
 * Per-PAL state (single instance -- one ESWM per chip)
 * =============================================================================
 */

/**
 * @struct ra_net_pal_state_t
 * @brief Singleton PAL state.
 */
typedef struct {
  ra_net_pal_mac_t        mac;         /**< Stored MAC address.        */
  ra_net_pal_link_state_t link_state;  /**< Last observed link state.  */
  ra_net_pal_event_fn_t   event_fn;    /**< Async event callback.       */
  void*                   event_ctx;   /**< Callback context.            */
  bool                    initialised; /**< True after ra_net_pal_init. */
} ra_net_pal_state_t;

static ra_net_pal_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Translate ra_eth status bits into PAL event bits.
 */
static uint32_t internal_translate_event(uint32_t eth_mask)
{
  /* Wave 7.1: ra_eth only exposes a generic status mask; the
   * concrete bit assignments come with the descriptor-ring work
   * in 7.1b. Until then translate "any non-zero" into a generic
   * "error" so the PAL has a defined contract. */
  if (eth_mask != 0U) {
    return (uint32_t)k_ra_net_pal_event_error;
  }
  return (uint32_t)k_ra_net_pal_event_none;
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
  if ((s_state.event_fn != nullptr) && (pal_mask != (uint32_t)k_ra_net_pal_event_none)) {
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

  /* Reset state. */
  internal_zero_bytes(s_state.mac.bytes, (uint16_t)k_ra_net_pal_mac_addr_len);
  s_state.link_state  = k_ra_net_pal_link_down;
  s_state.event_fn    = nullptr;
  s_state.event_ctx   = nullptr;
  s_state.initialised = true;

  if (mac != nullptr) {
    internal_copy_bytes(s_state.mac.bytes, mac->bytes, (uint16_t)k_ra_net_pal_mac_addr_len);
  }

  /* Wire ra_eth dispatch into the PAL translator. */
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
  /* Best-effort tear-down; ignore handler-detach failure. */
  (void)ra_eth_attach_handler(nullptr, nullptr);
  const ra_err_t err  = ra_eth_deinit();
  s_state.initialised = false;
  s_state.event_fn    = nullptr;
  s_state.event_ctx   = nullptr;
  s_state.link_state  = k_ra_net_pal_link_down;
  return err;
}

ra_err_t ra_net_pal_set_mac_addr(const ra_net_pal_mac_t* mac)
{
  RA_CHECK_NULL_PTR((void*)mac, s_tag, "set_mac_addr: mac");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  internal_copy_bytes(s_state.mac.bytes, mac->bytes, (uint16_t)k_ra_net_pal_mac_addr_len);
  return k_ra_ok;
}

ra_err_t ra_net_pal_get_mac_addr(ra_net_pal_mac_t* out_mac)
{
  RA_CHECK_NULL_PTR(out_mac, s_tag, "get_mac_addr: out_mac");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  internal_copy_bytes(out_mac->bytes, s_state.mac.bytes, (uint16_t)k_ra_net_pal_mac_addr_len);
  return k_ra_ok;
}

ra_err_t ra_net_pal_send_frame(const uint8_t* frame, uint16_t len)
{
  RA_CHECK_NULL_PTR((void*)frame, s_tag, "send_frame: frame");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((len == 0U) || (len > (uint16_t)k_ra_net_pal_frame_max)) {
    return k_ra_err_invalid_arg;
  }
  /* Wave 7.1 stub. ra_eth_tx_submit lands in 7.1b. */
  return k_ra_err_not_supported;
}

/* out_buf and inout_len are written by the future ra_eth descriptor-
 * ring (out_buf gets frame bytes, inout_len gets actual length);
 * clang-tidy cannot see that yet so both look like read-only params. */
ra_err_t ra_net_pal_recv_frame(uint8_t*  out_buf,   // NOLINT(readability-non-const-parameter)
                               uint16_t* inout_len) // NOLINT(readability-non-const-parameter)
{
  RA_CHECK_NULL_PTR(out_buf, s_tag, "recv_frame: out_buf");
  RA_CHECK_NULL_PTR(inout_len, s_tag, "recv_frame: inout_len");
  if (!s_state.initialised) {
    return k_ra_err_invalid_state;
  }
  if (*inout_len < (uint16_t)k_ra_net_pal_frame_max) {
    return k_ra_err_invalid_arg;
  }
  /* Wave 7.1 stub. */
  return k_ra_err_not_supported;
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
