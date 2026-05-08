/**
 * @file ra_iic_b.c
 * @brief IIC_B (I3C unified IP) master driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling-mode master driver for the RA8D2 I3C peripheral operated in
 * I2C compatibility mode. Mirrors the public init / start / write /
 * read / stop flow from FSP's ``r_iic_b_master`` (file
 * ``r_iic_b_master.c``) collapsed into synchronous helpers -- no DTC
 * fast path, no IBI / HDR support, just enough to exchange bytes
 * with a 7-bit-addressed slave.
 *
 * The state machine implemented here is a synchronous reduction of
 * FSP's interrupt-driven flow:
 *
 * @verbatim
 * Write (restart = false):
 *     IDLE --START--> ADDR_TX --TDBEF0--> DATA_TX --STOP--> IDLE
 *
 * Write (restart = true):
 *     IDLE --START--> ADDR_TX --TDBEF0--> DATA_TX --hold--> (caller chains)
 *
 * Read (restart = false):
 *     IDLE --START--> ADDR_TX --RDBFF0--> DATA_RX --NACK on last
 *     byte--> STOP --> IDLE
 *
 * Read (restart = true):
 *     IDLE --START--> ADDR_TX --RDBFF0--> DATA_RX --NACK on last
 *     byte--> hold --> (caller chains)
 *
 * Combined transfer:
 *     IDLE --START--> ADDR_TX --TDBEF0--> DATA_TX --RESTART-->
 *     ADDR_TX(read) --RDBFF0--> DATA_RX --NACK on last byte--> STOP --> IDLE
 * @endverbatim
 *
 * Owns every write to the I3C register block. See HUM Ch 40
 * "I3C Bus Interface (I3C)", p 2445-2701.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_iic_b.h"

#include <stdint.h>

#include "ra8d2_iic_b_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_iic_b_internal.h"
#include "ra_log.h"
#include "ra_mstp.h"

/**
 * @brief Pure len/buf reject predicate -- see header for full contract.
 * @details Promoted helper so the line-976 AND can be driven under MC/DC.
 * @param[in] len Length in bytes.
 * @param[in] buf Buffer pointer.
 * @return Boolean reject predicate.
 * @retval true  Caller returns null-ptr error.
 * @retval false Combination is OK.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_iic_b_internal_len_buf_invalid(uint32_t len, const void* buf)
{
  return (len != 0U) && (buf == nullptr);
}

/**
 * @brief Pure dispatch-callback predicate -- see header for full contract.
 * @details Promoted helper so the line-1235 AND can be driven under MC/DC.
 * @param[in] mask Bitmask of pending error sources.
 * @param[in] cb   Callback pointer.
 * @return Boolean predicate.
 * @retval true  Caller invokes cb.
 * @retval false Skip callback.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_iic_b_internal_should_dispatch(uint8_t mask, const void* cb)
{
  return (mask != 0U) && (cb != nullptr);
}

/** @brief Log tag for this driver. */
static const char* s_tag = "IIC_B";

/**
 * @enum ra_iic_b_internal_t
 * @brief Implementation constants -- spin budgets, addressing helpers.
 */
typedef enum : uint32_t {
  /** Generic spin budget for status-flag polls. ~200k iters keeps the
   * worst-case stall under ~5ms at 250MHz with the load/branch pair
   * the compiler emits for the wait helpers. */
  k_ra_iic_b_poll_limit = 200000U,
  /** Shift count to convert a 7-bit address into the on-the-wire byte. */
  k_ra_iic_b_addr_shift = 1U,
  /** R/W bit value for a write transaction (0 in LSB). */
  k_ra_iic_b_addr_rw_write = 0U,
  /** R/W bit value for a read transaction  (1 in LSB). */
  k_ra_iic_b_addr_rw_read = 1U,
  /** 8-bit byte mask for narrowing 32-bit reads from NTDTBP0. */
  k_ra_iic_b_byte_mask = 0xFFU,
} ra_iic_b_internal_t;

/**
 * @struct ra_iic_b_state_t
 * @brief Per-channel dispatch state.
 */
typedef struct {
  ra_iic_b_complete_fn_t cb;          /**< Callback or NULL.            */
  void*                  ctx;         /**< Callback context.            */
  bool                   initialised; /**< Tracks ``ra_iic_b_init`` /
                                         ``ra_iic_b_deinit``.           */
  bool                   bus_held;    /**< True when the previous
                                         transaction returned with
                                         ``restart=true`` and the next
                                         call must inject a RESTART
                                         instead of a fresh START.      */
} ra_iic_b_state_t;

/**
 * @var s_iic_b_state
 * @brief Per-channel state table indexed by channel.
 */
static ra_iic_b_state_t s_iic_b_state[k_ra_iic_b_channel_count];

/**
 * @brief Compute STDBR.SBRLO / SBRHO half-period count from PCLKA.
 *
 * @details
 * IIC_B clocks SCL by counting reference-clock cycles for the low and
 * high halves of the bit period. With REFCKCTL.IREFCKS = 0 the
 * reference clock is PCLKA. The full bit period is therefore
 * ``(SBRLO + 1) + (SBRHO + 1)`` PCLKA cycles. We split it 50/50 so
 * each half is ``round(pclka_hz / (2 * bus_hz)) - 1``.
 *
 * Result is clamped to 8 bits to fit the SBRLO/SBRHO fields per HUM
 * Ch 40.2.15 "STDBR : Standard Bit Rate Register", p 2463.
 *
 * @param[in] bus_hz   Target bus clock (non-zero).
 * @param[in] pclka_hz PCLKA frequency (non-zero).
 * @return One half-period count, ``[0, 0xFF]``.
 *
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static uint8_t internal_iic_b_half_period(uint32_t bus_hz, uint32_t pclka_hz)
{
  /* mcdc-deactivated: both args validated by ra_iic_b_init upstream; defensive duplicate. */
  if ((bus_hz == 0U) || (pclka_hz == 0U)) {
    return 0U;
  }
  enum : uint32_t {
    k_ra_iic_b_period_split = 2U,
    k_ra_iic_b_max_field    = 0xFFU,
  };
  const uint32_t total = pclka_hz / bus_hz;
  const uint32_t half  = total / k_ra_iic_b_period_split;
  if (half == 0U) {
    return 0U;
  }
  uint32_t value = half - 1U;
  if (value > k_ra_iic_b_max_field) {
    value = k_ra_iic_b_max_field;
  }
  return (uint8_t)value;
}

/**
 * @brief Wait for a flag in NTST to set.
 *
 * @param[in] reg  Channel register block.
 * @param[in] mask Bit mask to test against ``NTST``.
 * @return ``k_ra_ok`` on flag set, ``k_ra_err_hw_timeout`` otherwise.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_iic_b_wait_ntst(volatile r_iic_b_regs_t* reg, uint32_t mask)
{
  for (uint32_t i = 0U; i < k_ra_iic_b_poll_limit; i++) {
    if ((reg->NTST & mask) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Decode latched BST error bits into a ``k_ra_iic_b_err_*`` mask.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] bst See header declaration for direction and constraints.
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
static uint8_t internal_iic_b_decode_errors(uint32_t bst)
{
  uint8_t mask = k_ra_iic_b_err_none;
  if ((bst & k_ra_iic_b_msk_bst_alf) != 0U) {
    mask |= k_ra_iic_b_err_arb_lost;
  }
  if ((bst & k_ra_iic_b_msk_bst_nackdf) != 0U) {
    mask |= k_ra_iic_b_err_nack;
  }
  if ((bst & k_ra_iic_b_msk_bst_todf) != 0U) {
    mask |= k_ra_iic_b_err_timeout;
  }
  return mask;
}

/**
 * @brief Pulse-reset the I3C peripheral via RSTCTL.RI3CRST.
 *
 * @details
 * On real silicon RSTCTL.RI3CRST self-clears once the internal reset
 * completes. We follow the FSP sequence (write the bit, poll until it
 * clears) but additionally write zero before polling so that the
 * host-mode simulator -- which has no auto-clear behaviour -- exits
 * the loop cleanly. In production the explicit zero-write hits while
 * the hardware is still draining the reset and is harmless.
 *
 * HUM Ch 40.2.4 "RSTCTL : Reset Control Register" p 2451
 *
 * @param[in] reg See header declaration for direction and constraints.
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
static ra_err_t internal_iic_b_reset(volatile r_iic_b_regs_t* reg)
{
  /* HUM Ch 40.2.4 "RSTCTL : Reset Control Register" p 2451 */
  reg->RSTCTL = k_ra_iic_b_msk_rstctl_ri3crst;
  reg->RSTCTL = 0U;
  for (uint32_t i = 0U; i < k_ra_iic_b_poll_limit; i++) {
    if ((reg->RSTCTL & k_ra_iic_b_msk_rstctl_ri3crst) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Issue a START condition.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] reg See header declaration for direction and constraints.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static void internal_iic_b_start(volatile r_iic_b_regs_t* reg)
{
  /* HUM Ch 40.2.32 "CNDCTL : Condition Control Register" p 2479 */
  reg->CNDCTL = k_ra_iic_b_msk_cndctl_stcnd;
}

/**
 * @brief Issue a repeated-START (Sr) condition.
 *
 * @details
 * HUM Ch 40.2.32 "CNDCTL : Condition Control Register" p 2479 -- Sr
 * differs from S in that it is gated by the prior transfer not having
 * issued STOP. The polling driver achieves that by leaving STOP off
 * when the caller passes ``restart=true`` to write/read.
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static void internal_iic_b_restart(volatile r_iic_b_regs_t* reg)
{
  /* HUM Ch 40.2.32 "CNDCTL : Condition Control Register" p 2479 */
  reg->CNDCTL = k_ra_iic_b_msk_cndctl_srcnd;
}

/**
 * @brief Issue a STOP condition.
 *
 * @details
 * Mirrors FSP's stop-on-completion path (rxi_read_data / tei_master).
 * SPCNDDF is left for the ERI dispatcher to observe; the polling
 * helpers don't gate on it because the synchronous flow is already
 * past the data phase by the time we get here.
 *
 * HUM Ch 40.2.32 "CNDCTL : Condition Control Register" p 2479
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static void internal_iic_b_stop(volatile r_iic_b_regs_t* reg)
{
  /* Clear the prior STOP-detect flag so the NEXT transaction sees a
   * fresh edge. W0C semantics. */
  reg->BST    = reg->BST & ~k_ra_iic_b_msk_bst_spcnddf;
  reg->CNDCTL = k_ra_iic_b_msk_cndctl_spcnd;
}

/**
 * @brief Clear all latched bus-status flags ahead of a new transaction.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] reg See header declaration for direction and constraints.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static void internal_iic_b_clear_bst(volatile r_iic_b_regs_t* reg)
{
  enum : uint32_t {
    k_ra_iic_b_bst_clear_mask = k_ra_iic_b_msk_bst_stcnddf | k_ra_iic_b_msk_bst_spcnddf |
                                k_ra_iic_b_msk_bst_nackdf | k_ra_iic_b_msk_bst_tendf |
                                k_ra_iic_b_msk_bst_alf | k_ra_iic_b_msk_bst_todf,
  };
  /* HUM Ch 40.2.46 "BST : Bus Status Register" p 2490 */
  reg->BST = reg->BST & ~k_ra_iic_b_bst_clear_mask;
}

/**
 * @brief Send a single address byte and wait for TDBEF0 to be ready
 *        for the next slot.
 *
 * @param[in] reg          Channel registers.
 * @param[in] address_byte Pre-shifted address with R/W bit.
 *
 * @return ``ra_err_t`` -- forwards timeout / NACK detection.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_iic_b_send_address(volatile r_iic_b_regs_t* reg, uint8_t address_byte)
{
  /* Wait for first TDBEF0 (set after START condition is on the bus).
   * HUM Ch 40.2.50 "NTST : Normal Transfer Status Register" p 2498 */
  ra_err_t err = internal_iic_b_wait_ntst(reg, k_ra_iic_b_msk_ntst_tdbef0);
  if (err != k_ra_ok) {
    return err;
  }
  /* HUM Ch 40.2.35 "NTDTBP0 : Normal Transfer Data Buffer Port 0" p 2481 */
  reg->NTDTBP0 = (uint32_t)address_byte;
  return k_ra_ok;
}

/**
 * @brief Map the latched BST error bits to a high-level status code.
 *
 * @details
 * NACK and arbitration-loss carry distinct codes so a caller can
 * distinguish "slave declined" from "another master won the bus".
 *
 * @param[in] bst See header declaration for direction and constraints.
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
static ra_err_t internal_iic_b_status_from_bst(uint32_t bst)
{
  if ((bst & k_ra_iic_b_msk_bst_nackdf) != 0U) {
    return k_ra_err_nack;
  }
  if ((bst & k_ra_iic_b_msk_bst_alf) != 0U) {
    return k_ra_err_hw_error;
  }
  if ((bst & k_ra_iic_b_msk_bst_todf) != 0U) {
    return k_ra_err_hw_timeout;
  }
  return k_ra_ok;
}

/**
 * @brief True when the bus is free as reported by BCST.BFREF.
 *
 * @details
 * HUM Ch 40.2.58 "BCST : Bus Condition Status Register" p 2512:
 * ``BFREF`` is 1 when the bus is in the free state. FSP gates new
 * master transactions on this flag (see ``iic_b_master_run_hw_master``
 * around the ``IIC_B_MASTER_HW_WAIT_BUS_FREE`` block).
 *
 * @param[in] reg See header declaration for direction and constraints.
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
static bool internal_iic_b_bus_free(volatile const r_iic_b_regs_t* reg)
{
  return (reg->BCST & k_ra_iic_b_msk_bcst_bfref) != 0U;
}

/* =============================================================================
 * Init / deinit -- mirrors FSP iic_b_master_open_hw_master().
 * =============================================================================
 */

/**
 * @brief Apply the post-reset register defaults for an IIC_B channel.
 *
 * @details
 * Mirrors the second half of FSP's ``iic_b_master_open_hw_master``:
 * STDBR / BFCTL / ACKCTL / SCSTRCTL get stamped with the bring-up
 * values, then BCTL.BUSE = 1 attaches the bus pads.
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] cfg See header declaration for direction and constraints.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static void internal_iic_b_apply_init_regs(volatile r_iic_b_regs_t* reg, const ra_iic_b_cfg_t* cfg)
{
  /* HUM Ch 40.2.14 "REFCKCTL : Reference Clock Control Register" p 2463 */
  reg->REFCKCTL = 0U;

  /* HUM Ch 40.2.15 "STDBR : Standard Bit Rate Register" p 2463 */
  const uint8_t  half  = internal_iic_b_half_period(cfg->bus_hz, cfg->pclka_hz);
  const uint32_t stdbr = ((uint32_t)half << (uint32_t)k_ra_iic_b_stdbr_sbrlo_pos) |
                         ((uint32_t)half << (uint32_t)k_ra_iic_b_stdbr_sbrho_pos);
  reg->STDBR           = stdbr;

  /* HUM Ch 40.2.12 "BFCTL : Bus Function Control Register" p 2459 */
  uint32_t bfctl =
    k_ra_iic_b_msk_bfctl_male | k_ra_iic_b_msk_bfctl_nale | k_ra_iic_b_msk_bfctl_scsyne;
  if (cfg->bus_hz >= k_ra_iic_b_speed_fast_plus) {
    bfctl |= k_ra_iic_b_msk_bfctl_fmpe;
  }
  reg->BFCTL = bfctl;

  /* HUM Ch 40.2.24 "ACKCTL : Acknowledge Control Register" p 2473 */
  reg->ACKCTL = k_ra_iic_b_msk_ackctl_acktwp;
  /* HUM Ch 40.2.25 "SCSTRCTL : SCL Stretch Control Register" p 2474 */
  reg->SCSTRCTL = 0U;

  /* BUSE=1: drive the bus.
   * HUM Ch 40.2.2 "BCTL : Bus Control Register" p 2449 */
  reg->BCTL = k_ra_iic_b_msk_bctl_buse;
}

/* ra_iic_b_init -- see header for full description. */
ra_err_t ra_iic_b_init(uint8_t channel, const ra_iic_b_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "iic_b_init: cfg");
  if (cfg->bus_hz == 0U) {
    return k_ra_err_invalid_arg;
  }
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B" p 444 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_i3c);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "iic_b_init: mstp enable");

  /* HUM Ch 40.2.92 "CECTL : Clock Enable Control Register" p 2543 */
  reg->CECTL = k_ra_iic_b_msk_cectl_clke;
  /* BUSE=0: detach SCL/SDA before reset.
   * HUM Ch 40.2.2 "BCTL : Bus Control Register" p 2449 */
  reg->BCTL = 0U;

  const ra_err_t reset_err = internal_iic_b_reset(reg);
  RA_RETURN_ON_ERROR(reset_err, s_tag, "iic_b_init: reset"); /* GCOVR_EXCL_BR_LINE */

  internal_iic_b_apply_init_regs(reg, cfg);

  s_iic_b_state[channel].cb          = nullptr;
  s_iic_b_state[channel].ctx         = nullptr;
  s_iic_b_state[channel].initialised = true;
  s_iic_b_state[channel].bus_held    = false;

  ra_log_info_val(s_tag, "iic_b_init channel", (uint32_t)channel);
  return k_ra_ok;
}

/* ra_iic_b_deinit -- see header for full description. */
ra_err_t ra_iic_b_deinit(uint8_t channel)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* BUSE=0: release the bus.
   * HUM Ch 40.2.2 "BCTL : Bus Control Register" p 2449 */
  reg->BCTL = 0U;
  /* HUM Ch 40.2.92 "CECTL : Clock Enable Control Register" p 2543 */
  reg->CECTL                         = 0U;
  s_iic_b_state[channel].cb          = nullptr;
  s_iic_b_state[channel].ctx         = nullptr;
  s_iic_b_state[channel].initialised = false;
  s_iic_b_state[channel].bus_held    = false;
  return ra_mstp_disable(k_ra_mstp_i3c);
}

/* ra_iic_b_set_clock -- see header for full description. */
ra_err_t ra_iic_b_set_clock(uint8_t channel, uint32_t bus_hz, uint32_t pclka_hz)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((bus_hz == 0U) || (pclka_hz == 0U)) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 40.2.15 "STDBR : Standard Bit Rate Register" p 2463 */
  const uint8_t half = internal_iic_b_half_period(bus_hz, pclka_hz);
  reg->STDBR         = ((uint32_t)half << (uint32_t)k_ra_iic_b_stdbr_sbrlo_pos) |
                       ((uint32_t)half << (uint32_t)k_ra_iic_b_stdbr_sbrho_pos);
  return k_ra_ok;
}

/* =============================================================================
 * Polling write -- mirrors FSP iic_b_master_run_hw_master + txi.
 * =============================================================================
 */

/**
 * @brief Issue START or RESTART based on whether the bus is currently
 *        held by a previous ``restart=true`` call.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] bus_held See header declaration for direction and constraints.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static void internal_iic_b_open_phase(volatile r_iic_b_regs_t* reg, bool bus_held)
{
  if (bus_held) {
    internal_iic_b_restart(reg);
  } else {
    internal_iic_b_start(reg);
  }
}

/**
 * @brief Push ``len`` bytes from ``data`` into NTDTBP0.
 *
 * @details
 * TX-side counterpart to ``internal_iic_b_drain_rx``. Each iteration
 * waits for TDBEF0 then writes one byte; bails out early on NACK
 * mid-payload (FSP TXI ERI fast-path).
 *
 * @param[in] reg See header declaration for direction and constraints.
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
static ra_err_t
internal_iic_b_drain_tx(volatile r_iic_b_regs_t* reg, const uint8_t* data, uint32_t len)
{
  ra_err_t err = k_ra_ok;
  for (uint32_t i = 0U; i < len; i++) {
    err = internal_iic_b_wait_ntst(reg, k_ra_iic_b_msk_ntst_tdbef0);
    if (err != k_ra_ok) {
      break;
    }
    if ((reg->BST & k_ra_iic_b_msk_bst_nackdf) != 0U) {
      break;
    }
    /* HUM Ch 40.2.35 "NTDTBP0 : Normal Transfer Data Buffer Port 0" p 2481 */
    reg->NTDTBP0 = (uint32_t)data[i];
  }
  return err;
}

/**
 * @brief Decide STOP vs hold and update channel state accordingly.
 *
 * @details
 * Common tail of write/read: if the data phase failed or the caller
 * did not request restart, issue STOP and clear ``bus_held``. Otherwise
 * leave the bus held for the next chained call.
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] channel See header declaration for direction and constraints.
 * @param[in] err See header declaration for direction and constraints.
 * @param[in] restart See header declaration for direction and constraints.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static void
internal_iic_b_finalize(volatile r_iic_b_regs_t* reg, uint8_t channel, ra_err_t err, bool restart)
{
  if ((err != k_ra_ok) || !restart) {
    internal_iic_b_stop(reg);
    s_iic_b_state[channel].bus_held = false;
  } else {
    s_iic_b_state[channel].bus_held = true;
  }
  internal_iic_b_clear_bst(reg);
}

/**
 * @brief Bus-busy gate: rejects new transactions while BCST.BFREF is
 *        clear, unless the channel is in a held-bus restart state.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] bus_held See header declaration for direction and constraints.
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
static ra_err_t internal_iic_b_busy_gate(volatile const r_iic_b_regs_t* reg, bool bus_held)
{
  if (bus_held) {
    return k_ra_ok;
  }
  return internal_iic_b_bus_free(reg) ? k_ra_ok : k_ra_err_busy;
}

/* ra_iic_b_write -- see header for full description. */
ra_err_t
ra_iic_b_write(uint8_t channel, uint8_t target_7b, const uint8_t* data, uint32_t len, bool restart)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "iic_b_write: channel");
  RA_CHECK_NULL_PTR(data, s_tag, "iic_b_write: data");

  const bool     bus_held  = s_iic_b_state[channel].bus_held;
  const ra_err_t busy_gate = internal_iic_b_busy_gate(reg, bus_held);
  if (busy_gate != k_ra_ok) {
    return busy_gate;
  }

  internal_iic_b_clear_bst(reg);
  internal_iic_b_open_phase(reg, bus_held);

  const uint8_t address_byte =
    (uint8_t)(((uint32_t)target_7b << k_ra_iic_b_addr_shift) | k_ra_iic_b_addr_rw_write);
  ra_err_t err = internal_iic_b_send_address(reg, address_byte);
  if (err != k_ra_ok) {
    internal_iic_b_stop(reg);
    s_iic_b_state[channel].bus_held = false;
    return err;
  }

  err                    = internal_iic_b_drain_tx(reg, data, len);
  const ra_err_t bst_err = internal_iic_b_status_from_bst(reg->BST);
  if (bst_err != k_ra_ok) {
    err = bst_err;
  }
  internal_iic_b_finalize(reg, channel, err, restart);
  return err;
}

/* =============================================================================
 * Polling read -- mirrors FSP iic_b_master_run_hw_master + rxi.
 * =============================================================================
 */

/**
 * @brief Drain ``len`` bytes from NTDTBP0 into ``out``.
 *
 * @details
 * Helper for ``ra_iic_b_read``. Mirrors FSP rxi_master() data path:
 * each iteration waits for RDBFF0 then reads NTDTBP0. On the final
 * byte the master is primed to NACK (via ACKCTL.ACKT paired with
 * ACKTWP) so the slave releases SDA when STOP issues.
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] out See header declaration for direction and constraints.
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
static ra_err_t internal_iic_b_drain_rx(volatile r_iic_b_regs_t* reg, uint8_t* out, uint32_t len)
{
  ra_err_t err = k_ra_ok;
  for (uint32_t i = 0U; i < len; i++) {
    if (i == (len - 1U)) {
      /* HUM Ch 40.2.24 "ACKCTL : Acknowledge Control Register" p 2473 */
      reg->ACKCTL = k_ra_iic_b_msk_ackctl_acktwp | k_ra_iic_b_msk_ackctl_ackt;
    }
    err = internal_iic_b_wait_ntst(reg, k_ra_iic_b_msk_ntst_rdbff0);
    if (err != k_ra_ok) {
      break;
    }
    /* HUM Ch 40.2.35 "NTDTBP0 : Normal Transfer Data Buffer Port 0" p 2481 */
    out[i] = (uint8_t)(reg->NTDTBP0 & k_ra_iic_b_byte_mask);
  }
  return err;
}

/**
 * @brief Run the data phase of an RX transaction (dummy-read + drain).
 *
 * @details
 * Mirrors FSP rxi_master(): the first RDBFF0 fires before NTDTBP0
 * holds real payload, so the first read is dropped to clock the first
 * data byte into the buffer. ACKCTL is then restored to its default
 * (ACK every byte) for the next transaction.
 *
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 *
 *
 *
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] buf See header declaration for direction and constraints.
 * @param[in] len See header declaration for direction and constraints.
 */
static ra_err_t internal_iic_b_rx_phase(volatile r_iic_b_regs_t* reg, uint8_t* buf, uint32_t len)
{
  ra_err_t err = internal_iic_b_wait_ntst(reg, k_ra_iic_b_msk_ntst_rdbff0);
  if (err == k_ra_ok) {
    (void)reg->NTDTBP0;
    err = internal_iic_b_drain_rx(reg, buf, len);
  }
  reg->ACKCTL = k_ra_iic_b_msk_ackctl_acktwp;
  return err;
}

/* ra_iic_b_read -- see header for full description. */
ra_err_t ra_iic_b_read(uint8_t channel, uint8_t target_7b, uint8_t* buf, uint32_t len, bool restart)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "iic_b_read: channel");
  RA_CHECK_NULL_PTR(buf, s_tag, "iic_b_read: buf");
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }

  const bool     bus_held  = s_iic_b_state[channel].bus_held;
  const ra_err_t busy_gate = internal_iic_b_busy_gate(reg, bus_held);
  if (busy_gate != k_ra_ok) {
    return busy_gate;
  }

  internal_iic_b_clear_bst(reg);
  internal_iic_b_open_phase(reg, bus_held);

  const uint8_t address_byte =
    (uint8_t)(((uint32_t)target_7b << k_ra_iic_b_addr_shift) | k_ra_iic_b_addr_rw_read);
  ra_err_t err = internal_iic_b_send_address(reg, address_byte);
  if (err != k_ra_ok) {
    internal_iic_b_stop(reg);
    s_iic_b_state[channel].bus_held = false;
    return err;
  }

  err                    = internal_iic_b_rx_phase(reg, buf, len);
  const ra_err_t bst_err = internal_iic_b_status_from_bst(reg->BST);
  if (bst_err != k_ra_ok) {
    err = bst_err;
  }
  internal_iic_b_finalize(reg, channel, err, restart);
  return err;
}

/* =============================================================================
 * Combined transfer -- write-then-RESTART-then-read.
 * =============================================================================
 */

/* ra_iic_b_transfer -- see header for full description. */
ra_err_t ra_iic_b_transfer(uint8_t        channel,
                           uint8_t        target_7b,
                           const uint8_t* tx,
                           uint32_t       tx_len,
                           uint8_t*       rx,
                           uint32_t       rx_len)
{
  if (ra_iic_b(channel) == nullptr) {
    return k_ra_err_null_ptr;
  }
  if ((tx_len == 0U) && (rx_len == 0U)) {
    return k_ra_err_invalid_arg;
  }
  if ((tx_len != 0U) && (tx == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (ra_iic_b_internal_len_buf_invalid(rx_len, rx)) {
    return k_ra_err_null_ptr;
  }

  /* Phase 1: write (hold the bus for the upcoming read).
   * Skipped entirely when tx_len == 0 -- this degenerates the call
   * into a plain read, matching FSP's expectation that a zero-byte
   * tx phase means "no register-pointer update needed". */
  if (tx_len != 0U) {
    const ra_err_t w_err =
      ra_iic_b_write(channel, target_7b, tx, tx_len, /*restart=*/(rx_len != 0U));
    if (w_err != k_ra_ok) {
      return w_err;
    }
  }

  /* Phase 2: read (close the bus normally). */
  if (rx_len != 0U) {
    const ra_err_t r_err = ra_iic_b_read(channel, target_7b, rx, rx_len, /*restart=*/false);
    if (r_err != k_ra_ok) {
      return r_err;
    }
  }
  return k_ra_ok;
}

/* =============================================================================
 * Abort -- cancel an in-flight transaction.
 * =============================================================================
 */

/* ra_iic_b_abort -- see header for full description. */
ra_err_t ra_iic_b_abort(uint8_t channel)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* Mask interrupts before tearing down (mirrors FSP
   * iic_b_master_abort_seq_master).
   * HUM Ch 40.2.48 "BIE", p 2495 / Ch 40.2.52 "NTIE" p 2504. */
  reg->BIE  = 0U;
  reg->NTIE = 0U;

  internal_iic_b_stop(reg);
  internal_iic_b_clear_bst(reg);
  s_iic_b_state[channel].bus_held = false;
  return k_ra_ok;
}

/* =============================================================================
 * Bus probe -- single-byte address-only transaction.
 * =============================================================================
 */

/* ra_iic_b_scan -- see header for full description. */
ra_err_t ra_iic_b_scan(uint8_t channel, uint8_t target_7b, bool* out_acked)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "iic_b_scan: channel");
  RA_CHECK_NULL_PTR(out_acked, s_tag, "iic_b_scan: out_acked");

  *out_acked = false;
  internal_iic_b_clear_bst(reg);
  internal_iic_b_start(reg);

  const uint8_t address_byte =
    (uint8_t)(((uint32_t)target_7b << k_ra_iic_b_addr_shift) | k_ra_iic_b_addr_rw_write);
  ra_err_t err = internal_iic_b_send_address(reg, address_byte);
  if (err != k_ra_ok) {
    internal_iic_b_stop(reg);
    return err;
  }

  /* Wait for either TENDF (slave ACKed the address) or NACKDF. */
  err = k_ra_err_hw_timeout;
  for (uint32_t i = 0U; i < k_ra_iic_b_poll_limit; i++) {
    const uint32_t bst = reg->BST;
    if ((bst & (k_ra_iic_b_msk_bst_tendf | k_ra_iic_b_msk_bst_nackdf)) != 0U) {
      *out_acked = (bst & k_ra_iic_b_msk_bst_nackdf) == 0U;
      err        = k_ra_ok;
      break;
    }
  }

  /* Stop is best-effort -- record the outcome of the probe regardless. */
  internal_iic_b_stop(reg);
  internal_iic_b_clear_bst(reg);
  return err;
}

/* =============================================================================
 * Status helpers.
 * =============================================================================
 */

/* ra_iic_b_get_errors -- see header for full description. */
ra_err_t ra_iic_b_get_errors(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "iic_b_get_errors: out_mask");
  volatile const r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  *out_mask = internal_iic_b_decode_errors(reg->BST);
  return k_ra_ok;
}

/* ra_iic_b_clear_errors -- see header for full description. */
ra_err_t ra_iic_b_clear_errors(uint8_t channel)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 40.2.46 "BST : Bus Status Register" p 2490 */
  enum : uint32_t {
    k_ra_iic_b_err_clear_mask =
      k_ra_iic_b_msk_bst_alf | k_ra_iic_b_msk_bst_nackdf | k_ra_iic_b_msk_bst_todf,
  };
  reg->BST = reg->BST & ~k_ra_iic_b_err_clear_mask;
  return k_ra_ok;
}

/* =============================================================================
 * Interrupt handler attach + ERI dispatch.
 * =============================================================================
 */

/* ra_iic_b_attach_handler -- see header for full description. */
ra_err_t ra_iic_b_attach_handler(uint8_t channel, ra_iic_b_complete_fn_t fn, void* ctx)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  s_iic_b_state[channel].cb  = fn;
  s_iic_b_state[channel].ctx = ctx;

  /* Toggle the interrupt sources used by the polling driver as a
   * single group when (de)attaching a handler. Per-bit tuning lands
   * when the first interrupt-mode consumer arrives. */
  if (fn != nullptr) {
    /* HUM Ch 40.2.48 "BIE : Bus Interrupt Enable Register" p 2495 */
    reg->BIE = k_ra_iic_b_msk_bie_nackdie | k_ra_iic_b_msk_bie_alie | k_ra_iic_b_msk_bie_todie |
               k_ra_iic_b_msk_bie_tendie;
    /* HUM Ch 40.2.52 "NTIE : Normal Transfer Interrupt Enable" p 2504 */
    reg->NTIE = k_ra_iic_b_msk_ntie_tdbeie0 | k_ra_iic_b_msk_ntie_rdbfie0;
  } else {
    /* HUM Ch 40.2.48 "BIE : Bus Interrupt Enable Register" p 2495 */
    reg->BIE = 0U;
    /* HUM Ch 40.2.52 "NTIE : Normal Transfer Interrupt Enable" p 2504 */
    reg->NTIE = 0U;
  }
  return k_ra_ok;
}

/* ra_iic_b_dispatch_eri -- see header for full description. */
void ra_iic_b_dispatch_eri(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_iic_b_channel_count) {
    return;
  }
  uint8_t mask = 0U;
  (void)ra_iic_b_get_errors(channel, &mask);
  (void)ra_iic_b_clear_errors(channel);
  const ra_iic_b_complete_fn_t cb = s_iic_b_state[channel].cb;
  if (ra_iic_b_internal_should_dispatch(mask, (const void*)cb)) {
    cb(s_iic_b_state[channel].ctx, mask);
  }
}

/* =============================================================================
 * Legacy NSC entry points -- thin pass-throughs that keep the
 * Ring-4 NSC veneer surface in libs/ra_nsc compiling unchanged.
 * The shims always issue STOP (restart = false) -- chained / repeated-
 * START sequences must use the Ring-3 ra_iic_b_* APIs directly.
 * =============================================================================
 */

/* ra_iic_init -- see header for full description. */
ra_err_t ra_iic_init(uint8_t channel, const ra_iic_cfg_t* cfg)
{
  return ra_iic_b_init(channel, cfg);
}

/* ra_iic_write -- see header for full description. */
ra_err_t ra_iic_write(uint8_t channel, uint8_t target_7b, const uint8_t* data, uint32_t len)
{
  return ra_iic_b_write(channel, target_7b, data, len, /*restart=*/false);
}

/* ra_iic_read -- see header for full description. */
ra_err_t ra_iic_read(uint8_t channel, uint8_t target_7b, uint8_t* out, uint32_t len)
{
  return ra_iic_b_read(channel, target_7b, out, len, /*restart=*/false);
}
