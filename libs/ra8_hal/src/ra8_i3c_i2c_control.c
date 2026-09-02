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
 * - ``ra8_i3c_i2c_scan``         single-byte address-only bus probe.
 * - ``ra8_i3c_i2c_get_errors`` / ``ra8_i3c_i2c_clear_errors``
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

/** @brief Log tag for this driver's control-plane TU. */
static const char* s_tag = "IIC_B";

/**
 * @enum internal_i3c_i2c_control_t
 * @brief Implementation constants for the control-plane TU.
 */
typedef enum : uint32_t {
  /** Generic spin budget for status-flag polls. ~200k iters keeps the
   * worst-case stall under ~5ms at 250MHz with the load/branch pair
   * the compiler emits for the wait helpers. */
  k_ra8_i3c_i2c_ctrl_poll_limit = 200000U,
  /** Shift count to convert a 7-bit address into the on-the-wire byte. */
  k_ra8_i3c_i2c_ctrl_addr_shift = 1U,
  /** R/W bit value for a write transaction (0 in LSB). */
  k_ra8_i3c_i2c_ctrl_addr_rw_write = 0U,
} internal_i3c_i2c_control_t;

/**
 * @brief Decode latched BST error bits into a ``k_ra8_i3c_i2c_err_*`` mask.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] bst See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_i3c_i2c_decode_errors(uint32_t bst)
{
  uint8_t mask = k_ra8_i3c_i2c_err_none;
  if ((bst & k_ra8_i3c_i2c_msk_bst_alf) != 0U) {
    mask |= k_ra8_i3c_i2c_err_arb_lost;
  }
  if ((bst & k_ra8_i3c_i2c_msk_bst_nackdf) != 0U) {
    mask |= k_ra8_i3c_i2c_err_nack;
  }
  if ((bst & k_ra8_i3c_i2c_msk_bst_todf) != 0U) {
    mask |= k_ra8_i3c_i2c_err_timeout;
  }
  return mask;
}

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
 * Bus probe -- single-byte address-only transaction.
 * =============================================================================
 */

ra8_err_t ra8_i3c_i2c_scan(uint8_t channel, uint8_t target_7b, bool* out_acked)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "iic_b_scan: channel");
  RA8_CHECK_NULL_PTR(out_acked, s_tag, "iic_b_scan: out_acked");

  *out_acked = false;
  priv_i3c_i2c_clear_bst(reg);
  priv_i3c_i2c_start(reg);

  const uint8_t address_byte = (uint8_t)(((uint32_t)target_7b << k_ra8_i3c_i2c_ctrl_addr_shift) |
                                         k_ra8_i3c_i2c_ctrl_addr_rw_write);
  ra8_err_t     err          = priv_i3c_i2c_send_address(reg, address_byte);
  if (err != k_ra8_ok) {
    priv_i3c_i2c_stop(reg);
    return err;
  }

  /* Wait for either TENDF (peripheral ACKed the address) or NACKDF. */
  err = k_ra8_err_hw_timeout;
  for (uint32_t i = 0U; i < k_ra8_i3c_i2c_ctrl_poll_limit; i++) {
    const uint32_t bst = reg->BST;
    const bool     transfer_done =
      (bst & (k_ra8_i3c_i2c_msk_bst_tendf | k_ra8_i3c_i2c_msk_bst_nackdf)) != 0U;
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    /* HUM Ch 40.2.46 "BST : Bus Status Register" p 2490 */
    const bool observed = ra8_fake_mmio_poll(&reg->BST, i, transfer_done);
#else
    const bool observed = transfer_done;
#endif
    if (observed) {
      *out_acked = (bst & k_ra8_i3c_i2c_msk_bst_nackdf) == 0U;
      err        = k_ra8_ok;
      break;
    }
  }

  /* Stop is best-effort -- record the outcome of the probe regardless. */
  priv_i3c_i2c_stop(reg);
  priv_i3c_i2c_clear_bst(reg);
  return err;
}

/* =============================================================================
 * Status helpers.
 * =============================================================================
 */

ra8_err_t ra8_i3c_i2c_get_errors(uint8_t channel, uint8_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "iic_b_get_errors: out_mask");
  volatile const r_i3c_i2c_regs_t* reg = i3c_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_mask = internal_i3c_i2c_decode_errors(reg->BST);
  return k_ra8_ok;
}

ra8_err_t ra8_i3c_i2c_clear_errors(uint8_t channel)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 40.2.46 "BST : Bus Status Register" p 2490 */
  enum : uint32_t {
    k_ra8_i3c_i2c_err_clear_mask = k_ra8_i3c_i2c_msk_bst_alf | k_ra8_i3c_i2c_msk_bst_nackdf |
                                   k_ra8_i3c_i2c_msk_bst_todf, /**< RA8 I3C I2C error clear mask. */
  };
  reg->BST = reg->BST & ~k_ra8_i3c_i2c_err_clear_mask;
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
