/**
 * @file ra8_i3c_i2c_control.c
 * @brief IIC_B (I3C unified IP) control-plane + diagnostics implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Control-plane companion translation unit to ``ra8_i3c_i2c.c``. Holds the
 * non-data-path operations of the polling IIC_B driver:
 *
 * - ``ra8_i3c_i2c_abort``        cancel an in-flight transaction.
 *                                     latched bus-status decode + scrub.
 * - ``ra8_i3c_i2c_attach_handler`` register a completion / error
 *                                     callback and toggle the IIC_B IRQ
 *                                     enable bits as a group.
 * - ``ra8_i3c_i2c_dispatch_eri`` ERI service routine that decodes,
 *                                     clears, and forwards errors.
 *
 * These functions share the ``s_iic_b_state`` channel table and the promoted
 * START / STOP / clear-BST / send-address helpers with the transaction engine
 * in ``ra8_i3c_i2c.c`` via ``ra8_i3c_i2c_internal.h``. Each TU keeps its own
 * read-only ``s_tag`` log-tag copy.
 *
 * Owns its writes to the I3C register block. See HUM Ch 40
 * "I3C Bus Interface (I3C)", p 2445-2701.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_i3c_i2c.h"
#include "ra8_i3c_i2c_internal.h"
#include "ra8_i3c_i2c_regs.h"

/* =============================================================================
 * Abort -- cancel an in-flight transaction.
 * =============================================================================
 */

ra8_err_t ra8_i3c_i2c_abort(uint8_t channel)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* Mask interrupts before tearing down (mirrors FSP
   * controller abort-sequence helper).
   * HUM Ch 40.2.48 "BIE", p 2495 / Ch 40.2.52 "NTIE" p 2504. */
  reg->BIE  = 0U;
  reg->NTIE = 0U;

  priv_i3c_i2c_stop(reg);
  priv_i3c_i2c_clear_bst(reg);
  s_iic_b_state[channel].bus_held = false;
  return k_ra8_ok;
}

/* =============================================================================
 * Interrupt handler attach + ERI dispatch.
 * =============================================================================
 */

ra8_err_t ra8_i3c_i2c_attach_handler(uint8_t channel, ra8_i3c_i2c_complete_fn_t fn, void* ctx)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  s_iic_b_state[channel].cb  = fn;
  s_iic_b_state[channel].ctx = ctx;

  /* Toggle the interrupt sources used by the polling driver as a
   * single group when (de)attaching a handler. Per-bit tuning lands
   * when the first interrupt-mode consumer arrives. */
  if (fn != nullptr) {
    /* HUM Ch 40.2.48 "BIE : Bus Interrupt Enable Register" p 2495 */
    reg->BIE = k_ra8_i3c_i2c_msk_bie_nackdie | k_ra8_i3c_i2c_msk_bie_alie |
               k_ra8_i3c_i2c_msk_bie_todie | k_ra8_i3c_i2c_msk_bie_tendie;
    /* HUM Ch 40.2.52 "NTIE : Normal Transfer Interrupt Enable" p 2504 */
    reg->NTIE = k_ra8_i3c_i2c_msk_ntie_tdbeie0 | k_ra8_i3c_i2c_msk_ntie_rdbfie0;
  } else {
    /* HUM Ch 40.2.48 "BIE : Bus Interrupt Enable Register" p 2495 */
    reg->BIE = 0U;
    /* HUM Ch 40.2.52 "NTIE : Normal Transfer Interrupt Enable" p 2504 */
    reg->NTIE = 0U;
  }
  return k_ra8_ok;
}

void ra8_i3c_i2c_dispatch_eri(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_i3c_i2c_channel_count) {
    return;
  }
  uint8_t mask = 0U;
  (void)ra8_i3c_i2c_get_errors(channel, &mask);
  (void)ra8_i3c_i2c_clear_errors(channel);
  const ra8_i3c_i2c_complete_fn_t cb = s_iic_b_state[channel].cb;
  if (priv_i3c_i2c_should_dispatch(mask, (const void*)cb)) {
    cb(s_iic_b_state[channel].ctx, mask);
  }
}
