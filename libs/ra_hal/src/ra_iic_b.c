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
#include "ra_log.h"
#include "ra_mstp.h"

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
  /** R/W bit on the address byte (1 = read). */
  k_ra_iic_b_addr_rw_pos = 0U,
  /** Shift count to convert a 7-bit address into the on-the-wire byte. */
  k_ra_iic_b_addr_shift = 1U,
} ra_iic_b_internal_t;

/**
 * @struct ra_iic_b_state_t
 * @brief Per-channel dispatch state.
 */
typedef struct {
  ra_iic_b_complete_fn_t cb;          /**< Callback or NULL. */
  void*                  ctx;         /**< Callback context. */
  bool                   initialised; /**< Tracks ``ra_iic_b_init`` /
                                         ``ra_iic_b_deinit``. */
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
 * Ch 40.2.16 "STDBR : Standard Bit Rate Register", p 2462.
 *
 * @param[in] bus_hz   Target bus clock (non-zero).
 * @param[in] pclka_hz PCLKA frequency (non-zero).
 * @return One half-period count, ``[0, 0xFF]``.
 */
static uint8_t internal_iic_b_half_period(uint32_t bus_hz, uint32_t pclka_hz)
{
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
 * HUM Ch 40.2.6 "RSTCTL : Reset Control Register" p 2456
 */
static ra_err_t internal_iic_b_reset(volatile r_iic_b_regs_t* reg)
{
  /* HUM Ch 40.2.6 "RSTCTL : Reset Control Register" p 2456 */
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
 */
static void internal_iic_b_start(volatile r_iic_b_regs_t* reg)
{
  /* HUM Ch 40.2.27 "CNDCTL : Condition Control Register" p 2473 */
  reg->CNDCTL = k_ra_iic_b_msk_cndctl_stcnd;
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
 * HUM Ch 40.2.27 "CNDCTL : Condition Control Register" p 2473
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
 */
static void internal_iic_b_clear_bst(volatile r_iic_b_regs_t* reg)
{
  enum : uint32_t {
    k_ra_iic_b_bst_clear_mask = k_ra_iic_b_msk_bst_stcnddf | k_ra_iic_b_msk_bst_spcnddf |
                                k_ra_iic_b_msk_bst_nackdf | k_ra_iic_b_msk_bst_tendf |
                                k_ra_iic_b_msk_bst_alf | k_ra_iic_b_msk_bst_todf,
  };
  /* HUM Ch 40.2.31 "BST : Bus Status Register" p 2482 */
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
 */
static ra_err_t internal_iic_b_send_address(volatile r_iic_b_regs_t* reg, uint8_t address_byte)
{
  /* Wait for first TDBEF0 (set after START condition is on the bus).
   * HUM Ch 40.2.34 "NTST : Normal Transfer Status Register" p 2486 */
  ra_err_t err = internal_iic_b_wait_ntst(reg, k_ra_iic_b_msk_ntst_tdbef0);
  if (err != k_ra_ok) {
    return err;
  }
  /* HUM Ch 40.2.30 "NTDTBP0 : Normal Transfer Data Buffer Port 0" p 2476 */
  reg->NTDTBP0 = (uint32_t)address_byte;
  return k_ra_ok;
}

/**
 * @brief Map the latched BST error bits to a high-level status code.
 */
static ra_err_t internal_iic_b_status_from_bst(uint32_t bst)
{
  if ((bst & (k_ra_iic_b_msk_bst_alf | k_ra_iic_b_msk_bst_nackdf)) != 0U) {
    return k_ra_err_hw_error;
  }
  if ((bst & k_ra_iic_b_msk_bst_todf) != 0U) {
    return k_ra_err_hw_timeout;
  }
  return k_ra_ok;
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
 */
static void internal_iic_b_apply_init_regs(volatile r_iic_b_regs_t* reg, const ra_iic_b_cfg_t* cfg)
{
  /* HUM Ch 40.2.15 "REFCKCTL : Reference Clock Control Register" p 2462 */
  reg->REFCKCTL = 0U;

  /* HUM Ch 40.2.16 "STDBR : Standard Bit Rate Register" p 2462 */
  const uint8_t  half  = internal_iic_b_half_period(cfg->bus_hz, cfg->pclka_hz);
  const uint32_t stdbr = ((uint32_t)half << (uint32_t)k_ra_iic_b_stdbr_sbrlo_pos) |
                         ((uint32_t)half << (uint32_t)k_ra_iic_b_stdbr_sbrho_pos);
  reg->STDBR           = stdbr;

  /* HUM Ch 40.2.12 "BFCTL : Bus Function Control Register" p 2461 */
  uint32_t bfctl =
    k_ra_iic_b_msk_bfctl_male | k_ra_iic_b_msk_bfctl_nale | k_ra_iic_b_msk_bfctl_scsyne;
  if (cfg->bus_hz >= k_ra_iic_b_speed_fast_plus) {
    bfctl |= k_ra_iic_b_msk_bfctl_fmpe;
  }
  reg->BFCTL = bfctl;

  /* HUM Ch 40.2.22 "ACKCTL : Acknowledge Control Register" p 2468 */
  reg->ACKCTL = k_ra_iic_b_msk_ackctl_acktwp;
  /* HUM Ch 40.2.23 "SCSTRCTL : SCL Stretch Control Register" p 2469 */
  reg->SCSTRCTL = 0U;

  /* BUSE=1: drive the bus.
   * HUM Ch 40.2.4 "BCTL : Bus Control Register" p 2454 */
  reg->BCTL = k_ra_iic_b_msk_bctl_buse;
}

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

  /* HUM Ch 40.2.3 "CECTL : Clock Enable Control Register" p 2453 */
  reg->CECTL = k_ra_iic_b_msk_cectl_clke;
  /* BUSE=0: detach SCL/SDA before reset.
   * HUM Ch 40.2.4 "BCTL : Bus Control Register" p 2454 */
  reg->BCTL = 0U;

  const ra_err_t reset_err = internal_iic_b_reset(reg);
  RA_RETURN_ON_ERROR(reset_err, s_tag, "iic_b_init: reset"); /* GCOVR_EXCL_BR_LINE */

  internal_iic_b_apply_init_regs(reg, cfg);

  s_iic_b_state[channel].cb          = nullptr;
  s_iic_b_state[channel].ctx         = nullptr;
  s_iic_b_state[channel].initialised = true;

  ra_log_info_val(s_tag, "iic_b_init channel", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_iic_b_deinit(uint8_t channel)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* BUSE=0: release the bus.
   * HUM Ch 40.2.4 "BCTL : Bus Control Register" p 2454 */
  reg->BCTL = 0U;
  /* HUM Ch 40.2.3 "CECTL : Clock Enable Control Register" p 2453 */
  reg->CECTL                         = 0U;
  s_iic_b_state[channel].cb          = nullptr;
  s_iic_b_state[channel].ctx         = nullptr;
  s_iic_b_state[channel].initialised = false;
  return ra_mstp_disable(k_ra_mstp_i3c);
}

ra_err_t ra_iic_b_set_clock(uint8_t channel, uint32_t bus_hz, uint32_t pclka_hz)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((bus_hz == 0U) || (pclka_hz == 0U)) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 40.2.16 "STDBR : Standard Bit Rate Register" p 2462 */
  const uint8_t half = internal_iic_b_half_period(bus_hz, pclka_hz);
  reg->STDBR         = ((uint32_t)half << (uint32_t)k_ra_iic_b_stdbr_sbrlo_pos) |
                       ((uint32_t)half << (uint32_t)k_ra_iic_b_stdbr_sbrho_pos);
  return k_ra_ok;
}

/* =============================================================================
 * Polling write -- mirrors FSP iic_b_master_run_hw_master + txi.
 * =============================================================================
 */

ra_err_t ra_iic_b_write(uint8_t channel, uint8_t target_7b, const uint8_t* data, uint32_t len)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "iic_b_write: channel");
  RA_CHECK_NULL_PTR(data, s_tag, "iic_b_write: data");

  internal_iic_b_clear_bst(reg);
  internal_iic_b_start(reg);

  /* The address slot uses the same TDBEF0 gate as the data slots --
   * no separate STCNDDF wait per FSP TXI flow. */
  const uint8_t address_byte = (uint8_t)((uint32_t)target_7b << k_ra_iic_b_addr_shift);
  ra_err_t      err          = internal_iic_b_send_address(reg, address_byte);
  if (err != k_ra_ok) {
    internal_iic_b_stop(reg);
    return err;
  }

  for (uint32_t i = 0U; i < len; i++) {
    err = internal_iic_b_wait_ntst(reg, k_ra_iic_b_msk_ntst_tdbef0);
    if (err != k_ra_ok) {
      break;
    }
    /* HUM Ch 40.2.30 "NTDTBP0 : Normal Transfer Data Buffer Port 0" p 2476 */
    reg->NTDTBP0 = (uint32_t)data[i];
  }

  /* Note: FSP's TXI/TEI ISR pair waits for TENDF before issuing STOP.
   * In the synchronous polling driver the last NTDTBP0 write has
   * already drained into the bus FIFO; we issue STOP unconditionally
   * and let internal_iic_b_status_from_bst surface any latched error
   * (NACK / arbitration loss / timeout). */
  internal_iic_b_stop(reg);
  if (err == k_ra_ok) {
    err = internal_iic_b_status_from_bst(reg->BST);
  }
  internal_iic_b_clear_bst(reg);
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
 */
static ra_err_t internal_iic_b_drain_rx(volatile r_iic_b_regs_t* reg, uint8_t* out, uint32_t len)
{
  enum : uint32_t {
    k_ra_iic_b_byte_mask = 0xFFU,
  };
  ra_err_t err = k_ra_ok;
  for (uint32_t i = 0U; i < len; i++) {
    if (i == (len - 1U)) {
      /* HUM Ch 40.2.22 "ACKCTL : Acknowledge Control Register" p 2468 */
      reg->ACKCTL = k_ra_iic_b_msk_ackctl_acktwp | k_ra_iic_b_msk_ackctl_ackt;
    }
    err = internal_iic_b_wait_ntst(reg, k_ra_iic_b_msk_ntst_rdbff0);
    if (err != k_ra_ok) {
      break;
    }
    /* HUM Ch 40.2.30 "NTDTBP0 : Normal Transfer Data Buffer Port 0" p 2476 */
    out[i] = (uint8_t)(reg->NTDTBP0 & k_ra_iic_b_byte_mask);
  }
  return err;
}

ra_err_t ra_iic_b_read(uint8_t channel, uint8_t target_7b, uint8_t* out, uint32_t len)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "iic_b_read: channel");
  RA_CHECK_NULL_PTR(out, s_tag, "iic_b_read: out");
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }

  internal_iic_b_clear_bst(reg);
  internal_iic_b_start(reg);

  enum : uint32_t {
    k_ra_iic_b_addr_read_bit = 1U,
  };
  const uint8_t address_byte =
    (uint8_t)(((uint32_t)target_7b << k_ra_iic_b_addr_shift) | k_ra_iic_b_addr_read_bit);
  ra_err_t err = internal_iic_b_send_address(reg, address_byte);
  if (err != k_ra_ok) {
    internal_iic_b_stop(reg);
    return err;
  }

  /* Dummy read: per FSP rxi_master() the first RDBFF0 fires before
   * NTDTBP0 holds real data; reading once clocks the first byte in. */
  err = internal_iic_b_wait_ntst(reg, k_ra_iic_b_msk_ntst_rdbff0);
  if (err == k_ra_ok) {
    (void)reg->NTDTBP0;
    err = internal_iic_b_drain_rx(reg, out, len);
  }

  /* Restore ACK = 0 for the next transaction. */
  reg->ACKCTL = k_ra_iic_b_msk_ackctl_acktwp;

  internal_iic_b_stop(reg);
  if (err == k_ra_ok) {
    err = internal_iic_b_status_from_bst(reg->BST);
  }
  internal_iic_b_clear_bst(reg);
  return err;
}

/* =============================================================================
 * Bus probe -- single-byte address-only transaction.
 * =============================================================================
 */

ra_err_t ra_iic_b_scan(uint8_t channel, uint8_t target_7b, bool* out_acked)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "iic_b_scan: channel");
  RA_CHECK_NULL_PTR(out_acked, s_tag, "iic_b_scan: out_acked");

  *out_acked = false;
  internal_iic_b_clear_bst(reg);
  internal_iic_b_start(reg);

  const uint8_t address_byte = (uint8_t)((uint32_t)target_7b << k_ra_iic_b_addr_shift);
  ra_err_t      err          = internal_iic_b_send_address(reg, address_byte);
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

ra_err_t ra_iic_b_clear_errors(uint8_t channel)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 40.2.31 "BST : Bus Status Register" p 2482 */
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
    /* HUM Ch 40.2.33 "BIE : Bus Interrupt Enable Register" p 2484 */
    reg->BIE = k_ra_iic_b_msk_bie_nackdie | k_ra_iic_b_msk_bie_alie | k_ra_iic_b_msk_bie_todie |
               k_ra_iic_b_msk_bie_tendie;
    /* HUM Ch 40.2.36 "NTIE : Normal Transfer Interrupt Enable" p 2488 */
    reg->NTIE = k_ra_iic_b_msk_ntie_tdbeie0 | k_ra_iic_b_msk_ntie_rdbfie0;
  } else {
    /* HUM Ch 40.2.33 "BIE : Bus Interrupt Enable Register" p 2484 */
    reg->BIE = 0U;
    /* HUM Ch 40.2.36 "NTIE : Normal Transfer Interrupt Enable" p 2488 */
    reg->NTIE = 0U;
  }
  return k_ra_ok;
}

void ra_iic_b_dispatch_eri(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_iic_b_channel_count) {
    return;
  }
  uint8_t mask = 0U;
  (void)ra_iic_b_get_errors(channel, &mask);
  (void)ra_iic_b_clear_errors(channel);
  const ra_iic_b_complete_fn_t cb = s_iic_b_state[channel].cb;
  if ((mask != 0U) && (cb != nullptr)) {
    cb(s_iic_b_state[channel].ctx, mask);
  }
}

/* =============================================================================
 * Legacy NSC entry points -- thin pass-throughs that keep the
 * Ring-4 NSC veneer surface in libs/ra_nsc compiling unchanged.
 * =============================================================================
 */

ra_err_t ra_iic_init(uint8_t channel, const ra_iic_cfg_t* cfg)
{
  return ra_iic_b_init(channel, cfg);
}

ra_err_t ra_iic_write(uint8_t channel, uint8_t target_7b, const uint8_t* data, uint32_t len)
{
  return ra_iic_b_write(channel, target_7b, data, len);
}

ra_err_t ra_iic_read(uint8_t channel, uint8_t target_7b, uint8_t* out, uint32_t len)
{
  return ra_iic_b_read(channel, target_7b, out, len);
}
