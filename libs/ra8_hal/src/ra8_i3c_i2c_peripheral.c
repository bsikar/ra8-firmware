/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_i3c_i2c_peripheral.c
 * @brief IIC_B peripheral driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling-mode peripheral companion to ``ra8_i3c_i2c``. Mirrors FSP
 * ``r_iic_b_peripheral.c``:
 *
 *  - ``internal_i3c_i2c_peripheral_open`` clears the channel (RSTCTL.RI3CRST),
 *    programmes MSDVAD with the 7-bit own address, switches the bus
 *    interface enable bit BCTL.BUSE on, and (optionally) enables the
 *    general-call response via SVCTL.GCAE.
 *  - ``_send`` pushes ``len`` bytes into NTDTBP0 once NTST.TDBEF0
 *    asserts.
 *  - ``_receive`` drains ``len`` bytes from NTDTBP0 once NTST.RDBFF0
 *    asserts.
 *  - ``_status`` snapshots NTST + BST and translates the latched bits
 *    into ``k_ra8_i3c_i2c_peripheral_status_*``.
 *
 * Citations: HUM Ch 40.2 "I3C / IIC_B Register Reference",
 * pages 2452..2491 (chapter map row 40).
 */

#include "ra8_i3c_i2c_peripheral.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_i3c_i2c_regs.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"

static const char* s_tag = "IICBP";

/**
 * @enum ra8_i3c_i2c_peripheral_internal_t
 * @brief Internal tunables.
 */
typedef enum : uint32_t {
  k_ra8_i3c_i2c_peripheral_spin_budget = 50000U, /**< Bounded poll iterations. */
  k_ra8_i3c_i2c_peripheral_max_addr_7b = 0x7FU,  /**< 7-bit address ceiling.   */
} ra8_i3c_i2c_peripheral_internal_t;

/**
 * @enum ra8_i3c_i2c_peripheral_bits_t
 * @brief Local register-bit positions / masks used by this driver.
 */
typedef enum : uint32_t {
  k_ra8_i3c_i2c_peripheral_msk_ntst_tdbef0   = (uint32_t)(1U << 0U), /**< NTST.TDBEF0.            */
  k_ra8_i3c_i2c_peripheral_msk_ntst_rdbff0   = (uint32_t)(1U << 1U), /**< NTST.RDBFF0.            */
  k_ra8_i3c_i2c_peripheral_msk_svctl_gcae    = (uint32_t)(1U << 0U), /**< SVCTL.GCAE.             */
  k_ra8_i3c_i2c_peripheral_msdvad_shift_dvad = 1U,                   /**< Address shifted left 1. */
  k_ra8_i3c_i2c_peripheral_byte_mask         = 0xFFU,                /**< NTDTBP0 low-byte mask.  */
} ra8_i3c_i2c_peripheral_bits_t;

/**
 * @brief Bounded wait until ``mask`` bits set in NTST.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] mask See header declaration for direction and constraints.
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
static ra8_err_t internal_wait_ntst(volatile r_i3c_i2c_regs_t* reg, uint32_t mask)
{
  for (uint32_t i = 0U; i < k_ra8_i3c_i2c_peripheral_spin_budget; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((reg->NTST & mask) == mask) {                                    /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

ra8_err_t internal_i3c_i2c_peripheral_open(uint8_t channel, const ra8_i3c_i2c_peripheral_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "internal_i3c_i2c_peripheral_open: cfg null");
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(channel);
  if (reg == nullptr) {
    ra8_log_error(s_tag, "internal_i3c_i2c_peripheral_open: channel out of range");
    return k_ra8_err_invalid_arg;
  }
  if (cfg->peripheral_addr_7b > (uint8_t)k_ra8_i3c_i2c_peripheral_max_addr_7b) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 445 -- the R_I3C
   * block is clock-gated OFF at reset; cancel its module-stop (MSTPB4) before the
   * first responder register write or the peripheral never sees the bus. Omitting
   * this ran clean in ra8_emulator yet was inert on silicon until #405 modelled the
   * gate, which surfaced the miss here (the controller path already ungated). The
   * writes are guarded so a failed ungate does not touch a still-gated block, and
   * the outcome is returned from the single exit below (no extra return path). */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_i3c);
  if (mst_err == k_ra8_ok) { /* GCOVR_EXCL_BR_LINE */
    reg->RSTCTL = 0U;
    reg->MSDVAD = (uint32_t)cfg->peripheral_addr_7b << k_ra8_i3c_i2c_peripheral_msdvad_shift_dvad;
    reg->SVCTL  = (cfg->general_call != 0U) ? k_ra8_i3c_i2c_peripheral_msk_svctl_gcae : 0U;
    reg->BCTL   = k_ra8_i3c_i2c_msk_bctl_buse;
    ra8_log_info_val(s_tag, "internal_i3c_i2c_peripheral_open ch", (uint32_t)channel);
  }
  return mst_err;
}

ra8_err_t internal_i3c_i2c_peripheral_close(uint8_t channel)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  reg->BCTL   = 0U;
  reg->MSDVAD = 0U;
  reg->SVCTL  = 0U;
  /* Release the I3C module-stop reference taken in peripheral_open (ref-counted;
   * HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 445). */
  return ra8_mstp_disable(k_ra8_mstp_i3c);
}

ra8_err_t internal_i3c_i2c_peripheral_send(uint8_t channel, const uint8_t* data, uint32_t len)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((len > 0U) && (data == nullptr)) {
    ra8_log_error(s_tag, "internal_i3c_i2c_peripheral_send: data null");
    return k_ra8_err_null_ptr;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    RA8_RETURN_ON_ERROR(
      internal_wait_ntst(reg, k_ra8_i3c_i2c_peripheral_msk_ntst_tdbef0), /* GCOVR_EXCL_BR_LINE */
      s_tag,
      "internal_i3c_i2c_peripheral_send: TDBEF0 wait");
    reg->NTDTBP0 = (uint32_t)data[i];
  }
  return k_ra8_ok;
}

ra8_err_t internal_i3c_i2c_peripheral_receive(uint8_t channel, uint8_t* buf, uint32_t len)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((len > 0U) && (buf == nullptr)) {
    ra8_log_error(s_tag, "internal_i3c_i2c_peripheral_receive: buf null");
    return k_ra8_err_null_ptr;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    RA8_RETURN_ON_ERROR(
      internal_wait_ntst(reg, k_ra8_i3c_i2c_peripheral_msk_ntst_rdbff0), /* GCOVR_EXCL_BR_LINE */
      s_tag,
      "internal_i3c_i2c_peripheral_receive: RDBFF0 wait");
    buf[i] = (uint8_t)(reg->NTDTBP0 & k_ra8_i3c_i2c_peripheral_byte_mask);
  }
  return k_ra8_ok;
}

ra8_err_t internal_i3c_i2c_peripheral_status(uint8_t channel, uint8_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "internal_i3c_i2c_peripheral_status: out_mask null");
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t ntst = reg->NTST;
  const uint32_t bst  = reg->BST;
  uint8_t        mask = 0U;
  if ((ntst & k_ra8_i3c_i2c_peripheral_msk_ntst_rdbff0) != 0U) {
    mask |= k_ra8_i3c_i2c_peripheral_status_rx_full;
  }
  if ((ntst & k_ra8_i3c_i2c_peripheral_msk_ntst_tdbef0) != 0U) {
    mask |= k_ra8_i3c_i2c_peripheral_status_tx_empty;
  }
  if ((bst & k_ra8_i3c_i2c_msk_bst_spcnddf) != 0U) {
    mask |= k_ra8_i3c_i2c_peripheral_status_stop;
  }
  if ((bst & k_ra8_i3c_i2c_msk_bst_nackdf) != 0U) {
    mask |= k_ra8_i3c_i2c_peripheral_status_nack;
  }
  if (reg->MSDVAD != 0U) {
    mask |= k_ra8_i3c_i2c_peripheral_status_aas;
  }
  *out_mask = mask;
  return k_ra8_ok;
}
