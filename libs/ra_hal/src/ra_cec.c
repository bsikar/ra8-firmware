/**
 * @file ra_cec.c
 * @brief HDMI CEC driver -- placeholder implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Host-testable stand-in for FSP `r_cec`. The RA8D2 silicon does not
 * carry a CEC block, so the driver only mirrors API shape: state
 * transitions, payload bounds, and a single-slot RX queue that
 * unit tests can prime via `ra_cec_inject_rx()`. No real bytes are
 * shifted onto a CEC bus.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/* NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */

#include "ra_cec.h"

#include <stdint.h>
#include <string.h>

#include "ra8d2_cec_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/** @brief Module log tag. */
static const char* s_tag = "CEC";

/**
 * @struct ra_cec_internal_t
 * @brief Driver runtime state, file-scope.
 */
typedef struct {
  bool                  opened;
  bool                  rx_pending;
  ra_cec_logical_addr_t local_address;
  uint16_t              clock_div;
  uint16_t              error_flags;
  uint32_t              tx_count;
  uint32_t              rx_count;
  ra_cec_message_t      rx_slot;
  ra_cec_rx_handler_t   handler;
  void*                 handler_ctx;
} ra_cec_internal_t;

static ra_cec_internal_t s_state = {};

typedef enum : uint8_t {
  k_ra_cec_addr_max     = 15U, /**< Largest valid 4-bit logical addr. */
  k_ra_cec_ctrl0_enable = 0x80U,
} ra_cec_priv_t;

/* --------------------------------------------------------------------- */
/* Helpers                                                               */
/* --------------------------------------------------------------------- */

static bool internal_addr_in_range(ra_cec_logical_addr_t a)
{
  return (uint8_t)a <= k_ra_cec_addr_max;
}

/* --------------------------------------------------------------------- */
/* Public API                                                            */
/* --------------------------------------------------------------------- */

ra_err_t ra_cec_open(const ra_cec_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (!internal_addr_in_range(cfg->local_address)) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->clock_div == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.opened) {
    return k_ra_err_exists;
  }
  s_state.opened        = true;
  s_state.rx_pending    = false;
  s_state.local_address = cfg->local_address;
  s_state.clock_div     = cfg->clock_div;
  s_state.error_flags   = 0U;
  s_state.tx_count      = 0U;
  s_state.rx_count      = 0U;
  s_state.handler       = nullptr;
  s_state.handler_ctx   = nullptr;
  ra_log_info_val(s_tag, "cec open addr", (uint32_t)cfg->local_address);
  return k_ra_ok;
}

ra_err_t ra_cec_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  s_state.opened      = false;
  s_state.rx_pending  = false;
  s_state.handler     = nullptr;
  s_state.handler_ctx = nullptr;
  return k_ra_ok;
}

ra_err_t ra_cec_send_message(const ra_cec_message_t* msg)
{
  RA_CHECK_NULL_PTR(msg, s_tag, "msg must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  if (!internal_addr_in_range(msg->source) || !internal_addr_in_range(msg->destination)) {
    return k_ra_err_invalid_arg;
  }
  if (msg->payload_len > k_ra_cec_payload_max) {
    return k_ra_err_invalid_arg;
  }
  s_state.tx_count = s_state.tx_count + 1U;
  return k_ra_ok;
}

ra_err_t ra_cec_receive_message(ra_cec_message_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  if (!s_state.rx_pending) {
    return k_ra_err_no_data;
  }
  *out               = s_state.rx_slot;
  s_state.rx_pending = false;
  return k_ra_ok;
}

ra_err_t ra_cec_attach_handler(ra_cec_rx_handler_t handler, void* context)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  s_state.handler     = handler;
  s_state.handler_ctx = context;
  return k_ra_ok;
}

ra_err_t ra_cec_status_get(ra_cec_status_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  out->state         = s_state.rx_pending ? k_ra_cec_state_rx_active : k_ra_cec_state_ready;
  out->local_address = s_state.local_address;
  out->error_flags   = s_state.error_flags;
  out->tx_count      = s_state.tx_count;
  out->rx_count      = s_state.rx_count;
  return k_ra_ok;
}

ra_err_t ra_cec_inject_rx(const ra_cec_message_t* msg)
{
  RA_CHECK_NULL_PTR(msg, s_tag, "msg must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  if (msg->payload_len > k_ra_cec_payload_max) {
    return k_ra_err_invalid_arg;
  }
  s_state.rx_slot    = *msg;
  s_state.rx_pending = true;
  s_state.rx_count   = s_state.rx_count + 1U;
  if (s_state.handler != nullptr) {
    s_state.handler(&s_state.rx_slot, s_state.handler_ctx);
  }
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */
