/**
 * @file ra_iic_b_slave.c
 * @brief IIC_B slave driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling-mode slave companion to ``ra_iic_b``. Mirrors FSP
 * ``r_iic_b_slave.c``:
 *
 *  - ``ra_iic_b_slave_open`` clears the channel (RSTCTL.RI3CRST),
 *    programmes MSDVAD with the 7-bit own address, switches the bus
 *    interface enable bit BCTL.BUSE on, and (optionally) enables the
 *    general-call response via SVCTL.GCAE.
 *  - ``_send`` pushes ``len`` bytes into NTDTBP0 once NTST.TDBEF0
 *    asserts.
 *  - ``_receive`` drains ``len`` bytes from NTDTBP0 once NTST.RDBFF0
 *    asserts.
 *  - ``_status`` snapshots NTST + BST and translates the latched bits
 *    into ``k_ra_iic_b_slave_status_*``.
 *
 * Citations: HUM Ch 40.2 "I3C / IIC_B Register Reference",
 * pages 2452..2491 (chapter map row 40).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_iic_b_slave.h"

#include <stdint.h>

#include "ra8d2_iic_b_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "IICBS";

/**
 * @enum ra_iic_b_slave_internal_t
 * @brief Internal tunables.
 */
typedef enum : uint32_t {
  k_ra_iic_b_slave_spin_budget = 50000U, /**< Bounded poll iterations. */
  k_ra_iic_b_slave_max_addr_7b = 0x7FU,  /**< 7-bit address ceiling.   */
} ra_iic_b_slave_internal_t;

/**
 * @enum ra_iic_b_slave_bits_t
 * @brief Local register-bit positions / masks used by this driver.
 */
typedef enum : uint32_t {
  k_ra_iic_b_slave_msk_ntst_tdbef0   = (uint32_t)(1U << 0U), /**< NTST.TDBEF0. */
  k_ra_iic_b_slave_msk_ntst_rdbff0   = (uint32_t)(1U << 1U), /**< NTST.RDBFF0. */
  k_ra_iic_b_slave_msk_svctl_gcae    = (uint32_t)(1U << 0U), /**< SVCTL.GCAE.  */
  k_ra_iic_b_slave_msdvad_shift_dvad = 1U,                   /**< Address shifted left 1.    */
  k_ra_iic_b_slave_byte_mask         = 0xFFU,                /**< Low-byte mask of NTDTBP0.  */
} ra_iic_b_slave_bits_t;

/**
 * @brief Bounded wait until ``mask`` bits set in NTST.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] mask See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_wait_ntst(volatile r_iic_b_regs_t* reg, uint32_t mask)
{
  for (uint32_t i = 0U; i < k_ra_iic_b_slave_spin_budget; ++i) {
    if ((reg->NTST & mask) == mask) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief ra_iic_b_slave_open -- see header for full description.
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] channel See header declaration for direction and constraints.
 * @param[in] cfg See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
ra_err_t ra_iic_b_slave_open(uint8_t channel, const ra_iic_b_slave_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "ra_iic_b_slave_open: cfg null");
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    ra_log_error(s_tag, "ra_iic_b_slave_open: channel out of range");
    return k_ra_err_invalid_arg;
  }
  if (cfg->slave_addr_7b > (uint8_t)k_ra_iic_b_slave_max_addr_7b) {
    return k_ra_err_invalid_arg;
  }

  reg->RSTCTL = 0U;
  reg->MSDVAD = (uint32_t)cfg->slave_addr_7b << k_ra_iic_b_slave_msdvad_shift_dvad;
  reg->SVCTL  = (cfg->general_call != 0U) ? k_ra_iic_b_slave_msk_svctl_gcae : 0U;
  reg->BCTL   = k_ra_iic_b_msk_bctl_buse;
  ra_log_info_val(s_tag, "ra_iic_b_slave_open ch", (uint32_t)channel);
  return k_ra_ok;
}

/**
 * @brief ra_iic_b_slave_close -- see header for full description.
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] channel See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
ra_err_t ra_iic_b_slave_close(uint8_t channel)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  reg->BCTL   = 0U;
  reg->MSDVAD = 0U;
  reg->SVCTL  = 0U;
  return k_ra_ok;
}

/**
 * @brief ra_iic_b_slave_send -- see header for full description.
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] channel See header declaration for direction and constraints.
 * @param[in] data See header declaration for direction and constraints.
 * @param[in] len See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
ra_err_t ra_iic_b_slave_send(uint8_t channel, const uint8_t* data, uint32_t len)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((len > 0U) && (data == nullptr)) {
    ra_log_error(s_tag, "ra_iic_b_slave_send: data null");
    return k_ra_err_null_ptr;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    RA_RETURN_ON_ERROR(internal_wait_ntst(reg, k_ra_iic_b_slave_msk_ntst_tdbef0),
                       s_tag,
                       "ra_iic_b_slave_send: TDBEF0 wait");
    reg->NTDTBP0 = (uint32_t)data[i];
  }
  return k_ra_ok;
}

/**
 * @brief ra_iic_b_slave_receive -- see header for full description.
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] channel See header declaration for direction and constraints.
 * @param[in] buf See header declaration for direction and constraints.
 * @param[in] len See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
ra_err_t ra_iic_b_slave_receive(uint8_t channel, uint8_t* buf, uint32_t len)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((len > 0U) && (buf == nullptr)) {
    ra_log_error(s_tag, "ra_iic_b_slave_receive: buf null");
    return k_ra_err_null_ptr;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    RA_RETURN_ON_ERROR(internal_wait_ntst(reg, k_ra_iic_b_slave_msk_ntst_rdbff0),
                       s_tag,
                       "ra_iic_b_slave_receive: RDBFF0 wait");
    buf[i] = (uint8_t)(reg->NTDTBP0 & k_ra_iic_b_slave_byte_mask);
  }
  return k_ra_ok;
}

/**
 * @brief ra_iic_b_slave_status -- see header for full description.
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] channel See header declaration for direction and constraints.
 * @param[in] out_mask See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
ra_err_t ra_iic_b_slave_status(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "ra_iic_b_slave_status: out_mask null");
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t ntst = reg->NTST;
  const uint32_t bst  = reg->BST;
  uint8_t        mask = 0U;
  if ((ntst & k_ra_iic_b_slave_msk_ntst_rdbff0) != 0U) {
    mask |= k_ra_iic_b_slave_status_rx_full;
  }
  if ((ntst & k_ra_iic_b_slave_msk_ntst_tdbef0) != 0U) {
    mask |= k_ra_iic_b_slave_status_tx_empty;
  }
  if ((bst & k_ra_iic_b_msk_bst_spcnddf) != 0U) {
    mask |= k_ra_iic_b_slave_status_stop;
  }
  if ((bst & k_ra_iic_b_msk_bst_nackdf) != 0U) {
    mask |= k_ra_iic_b_slave_status_nack;
  }
  if (reg->MSDVAD != 0U) {
    mask |= k_ra_iic_b_slave_status_aas;
  }
  *out_mask = mask;
  return k_ra_ok;
}
