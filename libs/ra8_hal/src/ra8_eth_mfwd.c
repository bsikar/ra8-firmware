/**
 * @file ra8_eth_mfwd.c
 * @brief Ethernet Message Forwarding Engine driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 MFWD block. The MFWD shares the
 * Ethernet Switch MSTP gate (ESWM) so the lifecycle calls below
 * piggyback on ``k_ra8_mstp_eswm`` rather than a per-block bit.
 * Every register access carries a HUM Ch 30 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_eth_mfwd.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_ether_regs.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"

static const char* s_tag = "ETHMFW";

static ra8_eth_mfwd_event_fn_t s_mfwd_fn;
static void*                   s_mfwd_ctx;

ra8_err_t ra8_eth_mfwd_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_eswm);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "mfwd_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_mfwd_regs_t* reg = ra8_mfwd();
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  reg->MFWD_CTRL = 0U;
  reg->MFWD_STS  = 0U;
  reg->MFWD_IE   = 0U;
  reg->MFWD_ICLR = 0U;
  ra8_log_info(s_tag, "mfwd_init");
  return k_ra8_ok;
}

ra8_err_t ra8_eth_mfwd_deinit(void)
{
  volatile r_mfwd_regs_t* reg = ra8_mfwd();
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  reg->MFWD_CTRL = 0U;
  reg->MFWD_IE   = 0U;
  s_mfwd_fn      = nullptr;
  s_mfwd_ctx     = nullptr;
  return ra8_mstp_disable(k_ra8_mstp_eswm);
}

ra8_err_t ra8_eth_mfwd_get_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  *out_mask = ra8_mfwd()->MFWD_STS;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_mfwd_clear_status(uint32_t mask)
{
  volatile r_mfwd_regs_t* reg = ra8_mfwd();
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  reg->MFWD_ICLR = mask;
  reg->MFWD_STS  = reg->MFWD_STS & ~mask;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_mfwd_attach_handler(ra8_eth_mfwd_event_fn_t fn, void* ctx)
{
  s_mfwd_fn  = fn;
  s_mfwd_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_eth_mfwd_dispatch(void)
{
  volatile r_mfwd_regs_t* reg = ra8_mfwd();
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  const uint32_t                mask = reg->MFWD_STS;
  const ra8_eth_mfwd_event_fn_t fn   = s_mfwd_fn;
  void* const                   ctx  = s_mfwd_ctx;
  reg->MFWD_ICLR                     = mask;
  reg->MFWD_STS                      = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra8_err_t ra8_eth_mfwd_enter_stop(void)
{
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  ra8_mfwd()->MFWD_CTRL = 0U;
  return ra8_mstp_disable(k_ra8_mstp_eswm);
}

ra8_err_t ra8_eth_mfwd_exit_stop(void)
{
  return ra8_mstp_enable(k_ra8_mstp_eswm);
}

/**
 * @enum ra8_eth_mfwd_internal_t
 * @brief MFWD register offsets used by the routing programming path.
 *
 * @details Per CMSIS R7KA8D2KF_core0.h: FWPBFCSDC00 sits at offset
 * 0x4A04 in the MFWD register window (port 0), with each subsequent
 * port advancing 0x10 bytes (so port 1 -> 0x4A14, port 2 -> 0x4A24).
 * PBCSD (Port Based Forwarding CSD) is the low 7 bits.
 */
typedef enum : uint32_t {
  k_ra8_mfwd_off_fwpbfc0_base    = 0x4A00UL, /**< FWPBFC0[0] offset.        */
  k_ra8_mfwd_off_fwpbfcsdc0_base = 0x4A04UL, /**< FWPBFCSDC00 offset.       */
  k_ra8_mfwd_fwpbfcsdc_stride    = 0x10UL,   /**< Per-port stride.          */
  k_ra8_mfwd_pbdv_mask           = 0x7FUL,   /**< FWPBFC0.PBDV mask.        */
  k_ra8_mfwd_pbcsd_mask          = 0x7FUL,   /**< PBCSD field mask.         */
  k_ra8_mfwd_port_count          = 3UL,      /**< Port[0] + Port[1] + host. */
  k_ra8_mfwd_max_port            = 1UL,      /**< Highest GMAC port index.  */
  k_ra8_mfwd_max_queue           = 31UL,     /**< Highest GWCA queue.       */
} ra8_eth_mfwd_internal_t;

/**
 * @brief Return a typed pointer to FWPBFCSDC0[port] inside MFWD.
 *
 * @details Helper that computes the register address from the MFWD
 * base + per-port offset so the caller's body stays readable. Per
 * project policy hardware register access is via inline accessors,
 * never preprocessor macros.
 *
 * @param[in] port Port index (already bounds-checked by the caller).
 * @return Volatile pointer to the per-port routing register.
 *
 * @pre port <= k_ra8_mfwd_max_port.
 * @pre MFWD MSTP gate is on.
 * @post Returned pointer maps to the MFWD register window.
 * @post Returned pointer is non-null.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* internal_mfwd_fwpbfcsdc(uint8_t port)
{
  const uintptr_t addr =
    (uintptr_t)k_ra8_mfwd_base_addr +
    (uintptr_t)(k_ra8_mfwd_off_fwpbfcsdc0_base + ((uint32_t)port * k_ra8_mfwd_fwpbfcsdc_stride));
  return (volatile uint32_t*)addr;
}

ra8_err_t ra8_eth_mfwd_set_forwarding_masks(const uint8_t port_masks[3])
{
  RA8_CHECK_NULL_PTR(port_masks, s_tag, "set_forwarding_masks: null arg");
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_mfwd_port_count; ++i) {
    /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
    volatile uint32_t* const reg =
      (volatile uint32_t*)(k_ra8_mfwd_base_addr + (uintptr_t)(k_ra8_mfwd_off_fwpbfc0_base +
                                                              (i * k_ra8_mfwd_fwpbfcsdc_stride)));
    const uint32_t cur = *reg & ~(uint32_t)k_ra8_mfwd_pbdv_mask;
    *reg               = cur | ((uint32_t)port_masks[i] & (uint32_t)k_ra8_mfwd_pbdv_mask);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_eth_mfwd_route_queue(uint8_t port, uint8_t queue_index)
{
  if ((port > (uint8_t)k_ra8_mfwd_max_port) || (queue_index > (uint8_t)k_ra8_mfwd_max_queue)) {
    ra8_log_error(s_tag, "mfwd_route_queue: invalid port or queue");
    return k_ra8_err_invalid_arg;
  }
  /* FWPBFCSDC0[port].PBCSD -- port-to-host queue routing. */
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  volatile uint32_t* reg = internal_mfwd_fwpbfcsdc(port);
  const uint32_t     val = (*reg & ~(uint32_t)k_ra8_mfwd_pbcsd_mask) |
                       ((uint32_t)queue_index & (uint32_t)k_ra8_mfwd_pbcsd_mask);
  *reg = val;
  return k_ra8_ok;
}
