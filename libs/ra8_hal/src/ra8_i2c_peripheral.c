/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_i2c_peripheral.c
 * @brief I2C Bus Interface (IIC) target (peripheral) role -- polling mode
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Target-role plane of the RA8D2 RIIC polling driver, split out of
 * ``ra8_i2c.c`` so the controller transfer plane stays under the file-size
 * cap. Lets the RA8D2 answer a remote controller once one of its own-address
 * slots matches. This driver uses the inclusive "target / peripheral"
 * terminology for that role while keeping the HUM's register names
 * (SARLy, AASy, ...) verbatim so cross-referencing the manual stays easy.
 *
 * Mirrors FSP ``r_iic_slave`` collapsed into synchronous helpers -- no DTC
 * fast path and no interrupt path. Own-address matching, clock stretching and
 * both transfer directions are implemented; the controller transfer plane in
 * ``ra8_i2c.c`` and the bring-up plane in ``ra8_i2c_config.c`` are unchanged.
 * Both planes share ``s_i2c_state`` and the log tag via ``ra8_i2c_internal.h``.
 *
 * Target-role state machine (synchronous reduction of HUM Ch 39.3.5 /
 * 39.3.6):
 *
 * @verbatim
 *                +-----------------------------------------------+
 *                |                  TARGET_IDLE                  |
 *                |  (peripheral mode, ICCR2.MST = 0, own-address |
 *                |   match armed in ICSER, AASy flags clear)     |
 *                +-----------------------------------------------+
 *                   | poll: AASy/GCA set            | poll: AASy/GCA set
 *                   | and ICCR2.TRS = 0             | and ICCR2.TRS = 1
 *                   v  (controller WRITE)           v  (controller READ)
 *          +------------------+            +-------------------+
 *          |    RX_ACTIVE     |            |     TX_ACTIVE     |
 *          | dummy-read addr, |            | push ICDRT while  |
 *          | read ICDRR while |            | TDRE set and no   |
 *          | RDRF set and room|            | NACK, until len   |
 *          +------------------+            +-------------------+
 *                   | STOP or buffer full           | NACK (controller end)
 *                   | (clear ICSR2.STOP)            | or sent == len; wait
 *                   |                               | TEND, dummy-read ICDRR,
 *                   v                               v clear NACKF/STOP
 *                +-----------------------------------------------+
 *                |                  TARGET_IDLE                  |
 *                +-----------------------------------------------+
 * @endverbatim
 *
 * Owns every write to the RIIC own-address and target-transfer registers.
 * See HUM Ch 39 "I2C Bus Interface (IIC)", p 2367-2444.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_i2c.h"
#include "ra8_i2c_internal.h"
#include "ra8_i2c_regs.h"

/**
 * @enum ra8_i2c_peripheral_limits_t
 * @brief Spin budgets and validation bounds for the target plane.
 */
typedef enum : uint32_t {
  /** Generic spin budget for status-flag polls, matching the controller
   * plane: ~200k iterations keeps the worst-case stall under a few ms. */
  k_ra8_i2c_peripheral_poll_limit = 200000U,
} ra8_i2c_peripheral_limits_t;

/**
 * @enum ra8_i2c_peripheral_const_t
 * @brief Addressing constants for the target plane.
 */
typedef enum : uint8_t {
  k_ra8_i2c_peripheral_addr_7b_max = 0x7FU, /**< Largest valid 7-bit own address.     */
  k_ra8_i2c_peripheral_slot_max    = 2U,    /**< Highest own-address comparator slot. */
  /** Shift to place a 7-bit own address into SARLy.SVA[6:0] (bits [7:1]). */
  k_ra8_i2c_peripheral_addr_shift = (uint8_t)k_ra8_i2c_sarl_sva_pos,
  /** SARUy value for the 7-bit address format (FS = 0, SVA[1:0] = 0). */
  k_ra8_i2c_peripheral_saru_7bit = 0U,
  /** ICIER target-interrupt arm mask: RIE (RXI, bit 5) | TIE (TXI, bit 7) |
   * SPIE (STOP, bit 3) -- the own-address-match data + STOP interrupts. */
  k_ra8_i2c_peripheral_icier_arm =
    (uint8_t)((1U << (uint8_t)k_ra8_i2c_icier_rie_pos) | (1U << (uint8_t)k_ra8_i2c_icier_tie_pos) |
              (1U << (uint8_t)k_ra8_i2c_icier_spie_pos)),
} ra8_i2c_peripheral_const_t;

/* =============================================================================
 * Pure decision predicates (TU-external for MC/DC -- see ra8_i2c_internal.h).
 * =============================================================================
 */

bool ra8_i2c_internal_peripheral_poll_done(uint8_t icsr1, uint8_t icsr2)
{
  return ((icsr1 & (uint8_t)k_ra8_i2c_msk_icsr1_match) != 0U) ||
         ((icsr2 & (uint8_t)k_ra8_i2c_msk_icsr2_stop) != 0U);
}

bool ra8_i2c_internal_peripheral_rx_continue(uint8_t icsr2, uint32_t received, uint32_t capacity)
{
  return ((icsr2 & (uint8_t)k_ra8_i2c_msk_icsr2_stop) == 0U) && (received < capacity);
}

bool ra8_i2c_internal_peripheral_tx_done(uint8_t icsr2)
{
  return ((icsr2 & (uint8_t)k_ra8_i2c_msk_icsr2_nackf) != 0U) ||
         ((icsr2 & (uint8_t)k_ra8_i2c_msk_icsr2_tend) != 0U);
}

bool ra8_i2c_internal_peripheral_tx_continue(uint8_t icsr2, uint32_t sent, uint32_t len)
{
  return ((icsr2 & (uint8_t)k_ra8_i2c_msk_icsr2_nackf) == 0U) && (sent < len);
}

/* =============================================================================
 * Local helpers.
 * =============================================================================
 */

/**
 * @brief Classify a latched address-match snapshot into a poll event.
 *
 * @details
 * Returns ``none`` when no own-address / general-call flag is set; otherwise
 * decodes ICCR2.TRS, which the hardware sets to 1 when the controller's R/W#
 * bit was 1 (controller read -> target transmit) and leaves 0 for a write.
 *
 * @param[in] icsr1 Snapshot of ICSR1 (own-address detection flags).
 * @param[in] iccr2 Snapshot of ICCR2 (transmit/receive mode bit TRS).
 *
 * @return The decoded ``ra8_i2c_peripheral_event_t``.
 * @retval k_ra8_i2c_peripheral_event_none  No own-address match latched.
 * @retval k_ra8_i2c_peripheral_event_read  Match and TRS = 1 (controller reads).
 * @retval k_ra8_i2c_peripheral_event_write Match and TRS = 0 (controller writes).
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the two input snapshots.
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static ra8_i2c_peripheral_event_t internal_i2c_target_classify(uint8_t icsr1, uint8_t iccr2)
{
  if ((icsr1 & (uint8_t)k_ra8_i2c_msk_icsr1_match) == 0U) {
    return k_ra8_i2c_peripheral_event_none;
  }
  if ((iccr2 & (uint8_t)k_ra8_i2c_msk_iccr2_trs) != 0U) {
    return k_ra8_i2c_peripheral_event_read;
  }
  return k_ra8_i2c_peripheral_event_write;
}

/**
 * @brief Wait (bounded) for a flag in ICSR2 to set.
 *
 * @details
 * Spins up to ``k_ra8_i2c_peripheral_poll_limit`` iterations reading ICSR2 and
 * returning success the moment the masked flag is observed set. The fixed
 * bound satisfies NASA P10 Rule 2.
 *
 * @param[in] reg  Channel register block.
 * @param[in] mask Bit mask to test against ICSR2.
 *
 * @return ``ra8_err_t`` outcome of the poll.
 * @retval k_ra8_ok             Masked flag observed set.
 * @retval k_ra8_err_hw_timeout Spin budget exhausted before the flag set.
 *
 * @pre reg is non-NULL.
 * @pre mask is a non-zero mask.
 * @post On success the masked flag was observed set.
 * @post No register write occurs.
 * @note Thread safety: not thread-safe (reads a single channel).
 * @since 0.1.0
 */
static ra8_err_t internal_i2c_target_wait(volatile const r_i2c_regs_t* reg, uint8_t mask)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_i2c_peripheral_poll_limit;
       i++) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
    if ((reg->ICSR2 & mask) != 0U) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Programme a 7-bit own address into the SARLy/SARUy pair for a slot.
 *
 * @details
 * Writes ``addr_7b`` left-shifted into SARLy.SVA[6:0] and clears SARUy to
 * select the 7-bit address format (HUM Ch 39.2.13 p 2390, Ch 39.2.14 p 2391).
 * The three comparator slots are discrete registers, so a ``switch`` selects
 * the pair rather than indexing.
 *
 * @param[in] reg     Channel register block.
 * @param[in] slot    Own-address comparator slot (0, 1 or 2).
 * @param[in] addr_7b 7-bit own address (already range-checked).
 *
 * @pre reg is non-NULL.
 * @pre slot is 0, 1 or 2.
 * @post The selected SARLy holds ``addr_7b`` << 1 and SARUy reads 0.
 * @post No other register is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void internal_i2c_target_set_addr(volatile r_i2c_regs_t* reg, uint8_t slot, uint8_t addr_7b)
{
  const uint8_t sarl = (uint8_t)((uint32_t)addr_7b << (uint32_t)k_ra8_i2c_peripheral_addr_shift);
  switch (slot) {
    case (uint8_t)k_ra8_i2c_peripheral_slot_0:
      /* HUM Ch 39.2.13 "SARLy : own-address register Ly" p 2390 */
      reg->SARL0 = sarl;
      /* HUM Ch 39.2.14 "SARUy : own-address register Uy" p 2391 */
      reg->SARU0 = (uint8_t)k_ra8_i2c_peripheral_saru_7bit;
      break;
    case (uint8_t)k_ra8_i2c_peripheral_slot_1:
      /* HUM Ch 39.2.13 "SARLy : own-address register Ly" p 2390 */
      reg->SARL1 = sarl;
      /* HUM Ch 39.2.14 "SARUy : own-address register Uy" p 2391 */
      reg->SARU1 = (uint8_t)k_ra8_i2c_peripheral_saru_7bit;
      break;
    default:
      /* HUM Ch 39.2.13 "SARLy : own-address register Ly" p 2390 */
      reg->SARL2 = sarl;
      /* HUM Ch 39.2.14 "SARUy : own-address register Uy" p 2391 */
      reg->SARU2 = (uint8_t)k_ra8_i2c_peripheral_saru_7bit;
      break;
  }
}

/**
 * @brief Build the ICSER enable mask for one own-address slot.
 *
 * @details
 * ``SARyE`` for slot ``y`` is bit ``y`` of ICSER, so the slot enable is
 * ``1 << slot``; ``GCAE`` is OR-ed in when the general-call address is also
 * to be answered (HUM Ch 39.2.7 p 2380).
 *
 * @param[in] slot         Own-address comparator slot (0, 1 or 2).
 * @param[in] general_call True to also enable general-call matching.
 *
 * @return The ICSER value to write.
 * @retval 0 Never (at least one SARyE bit is always set).
 *
 * @pre slot is 0, 1 or 2.
 * @pre None.
 * @post Return has exactly the SARyE bit (and optionally GCAE) set.
 * @post No state is mutated.
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t internal_i2c_target_icser_mask(uint8_t slot, bool general_call)
{
  uint8_t mask = (uint8_t)((uint32_t)k_ra8_i2c_msk_icser_sar0e << (uint32_t)slot);
  if (general_call) {
    mask |= (uint8_t)k_ra8_i2c_msk_icser_gcae;
  }
  return mask;
}

/** @brief Implementation of `ra8_i2c_internal_target_drain_rx()` -- trailing-byte ICDRR drain. */
ra8_err_t ra8_i2c_internal_target_drain_rx(volatile r_i2c_regs_t* reg,
                                           uint8_t*               buf,
                                           uint32_t               capacity,
                                           uint32_t*              out_count)
{
  uint32_t  count = 0U;
  ra8_err_t err   = k_ra8_ok;
  for (uint32_t i = 0U; i <= capacity; i++) {
    err = internal_i2c_target_wait(
      reg,
      (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_rdrf | (uint8_t)k_ra8_i2c_msk_icsr2_stop));
    if (err != k_ra8_ok) {
      break;
    }
    /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
    const uint8_t icsr2 = reg->ICSR2;
    if (!ra8_i2c_internal_peripheral_rx_continue(icsr2, count, capacity)) {
      /* STOP detected or buffer full: drain a final pending byte if room. */
      if (((icsr2 & (uint8_t)k_ra8_i2c_msk_icsr2_rdrf) != 0U) && (count < capacity)) {
        /* HUM Ch 39.2.18 "ICDRR : I2C Bus Receive Data Register" p 2393 */
        buf[count] = reg->ICDRR;
        count++;
      }
      break;
    }
    if ((icsr2 & (uint8_t)k_ra8_i2c_msk_icsr2_rdrf) != 0U) {
      /* HUM Ch 39.2.18 "ICDRR : I2C Bus Receive Data Register" p 2393 */
      buf[count] = reg->ICDRR;
      count++;
    }
  }
  *out_count = count;
  return err;
}

/**
 * @brief Push controller-read data bytes from ``data`` into ICDRT.
 *
 * @details
 * The per-byte transmit loop extracted from ``ra8_i2c_peripheral_transmit``. Each
 * iteration waits for ICSR2.TDRE, then -- while
 * ``ra8_i2c_internal_peripheral_tx_continue`` holds (no controller NACK and bytes
 * remain) -- writes one byte to ICDRT. Stops on NACK, on a TDRE timeout, or
 * when every byte is queued. The loop is bounded by ``len + 1`` (NASA P10
 * Rule 2). The completion status is left to ``internal_i2c_target_finish_tx``.
 *
 * @param[in]  reg      Channel register block.
 * @param[in]  data     Source buffer.
 * @param[in]  len      Bytes available to send (non-zero).
 * @param[out] out_sent Number of bytes accepted by the controller.
 *
 * @pre reg, data and out_sent are non-NULL.
 * @pre The channel was addressed for a controller read (TRS = 1).
 * @post ``*out_sent`` <= ``len``.
 * @post Each accepted byte was written to ICDRT in order.
 * @note Thread safety: not thread-safe (drives a single channel).
 * @since 0.1.0
 */
static void internal_i2c_target_fill_tx(volatile r_i2c_regs_t* reg,
                                        const uint8_t*         data,
                                        uint32_t               len,
                                        uint32_t*              out_sent)
{
  uint32_t sent = 0U;
  for (uint32_t i = 0U; i <= len; i++) {
    /* Step 3 (peripheral transmit): wait for TDRE before each byte.
     * HUM Ch 39.3.5 "Peripheral Transmit Operation" p 2405 */
    if (internal_i2c_target_wait(reg, (uint8_t)k_ra8_i2c_msk_icsr2_tdre) != k_ra8_ok) {
      break;
    }
    /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
    const uint8_t icsr2 = reg->ICSR2;
    if (!ra8_i2c_internal_peripheral_tx_continue(icsr2, sent, len)) {
      break;
    }
    /* HUM Ch 39.2.17 "ICDRT : I2C Bus Transmit Data Register" p 2393 */
    reg->ICDRT = data[sent];
    sent++;
  }
  *out_sent = sent;
}

/**
 * @brief Close a target-transmit frame: wait for end, release SCL, clear flags.
 *
 * @details
 * Implements steps 4-7 of HUM Ch 39.3.5 p 2405: wait for the controller's
 * TEND or NACK, snapshot whether the frame ended cleanly via
 * ``ra8_i2c_internal_peripheral_tx_done``, dummy-read ICDRR to release SCL, then
 * clear NACKF and STOP (W0C) for the next transfer.
 *
 * @param[in] reg Channel register block.
 *
 * @return Whether the controller ended the read (TEND or NACK observed).
 * @retval true  TEND or NACK was latched within the spin budget.
 * @retval false The end-of-frame poll timed out.
 *
 * @pre reg is non-NULL.
 * @pre The transmit data phase has completed.
 * @post SCL is released and ICSR2.NACKF / STOP read back zero.
 * @post No data register other than the ICDRR release-read is touched.
 * @note Thread safety: not thread-safe (drives a single channel).
 * @since 0.1.0
 */
static bool internal_i2c_target_finish_tx(volatile r_i2c_regs_t* reg)
{
  /* Step 4: wait for the controller's TEND or NACK to mark frame end. */
  (void)internal_i2c_target_wait(
    reg,
    (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tend | (uint8_t)k_ra8_i2c_msk_icsr2_nackf));
  /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
  const bool ended = ra8_i2c_internal_peripheral_tx_done(reg->ICSR2);

  /* Step 5: dummy-read ICDRR to release SCL.
   * HUM Ch 39.2.18 "ICDRR : I2C Bus Receive Data Register" p 2393 */
  (void)reg->ICDRR;

  /* Step 7: clear NACKF and STOP (W0C) for the next transfer.
   * HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2 -- W0C" p 2384 */
  reg->ICSR2 = (uint8_t)(reg->ICSR2 & (uint8_t)~(uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_nackf |
                                                          (uint8_t)k_ra8_i2c_msk_icsr2_stop));
  return ended;
}

/* =============================================================================
 * Public target-role API.
 * =============================================================================
 */

ra8_err_t ra8_i2c_peripheral_init(uint8_t channel, const ra8_i2c_peripheral_cfg_t* cfg)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_i2c_tag, "i2c_target_init: channel");
  RA8_CHECK_NULL_PTR(cfg, s_i2c_tag, "i2c_target_init: cfg");
  if (cfg->own_addr_7b > (uint8_t)k_ra8_i2c_peripheral_addr_7b_max) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)cfg->slot > (uint8_t)k_ra8_i2c_peripheral_slot_max) {
    return k_ra8_err_invalid_arg;
  }

  const uint8_t slot = (uint8_t)cfg->slot;
  internal_i2c_target_set_addr(reg, slot, cfg->own_addr_7b);

  /* HUM Ch 39.2.7 "ICSER : I2C Bus Status Enable Register" p 2380 */
  reg->ICSER = internal_i2c_target_icser_mask(slot, cfg->general_call);

  if (cfg->clock_stretch) {
    /* HUM Ch 39.2.5 "ICMR3 : I2C Bus Mode Register 3 -- WAIT" p 2376 */
    reg->ICMR3 = (uint8_t)(reg->ICMR3 | (uint8_t)k_ra8_i2c_msk_icmr3_wait);
  }

  if (cfg->irq_enable) {
    /* HUM Ch 39.2.8 "ICIER : I2C Bus Interrupt Enable Register" p 2381 */
    reg->ICIER = (uint8_t)(reg->ICIER | (uint8_t)k_ra8_i2c_peripheral_icier_arm);
  }

  s_i2c_state[channel].peripheral_active = true;
  return k_ra8_ok;
}

ra8_err_t ra8_i2c_peripheral_deinit(uint8_t channel)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_i2c_state[channel].peripheral_active) {
    return k_ra8_err_not_initialized;
  }
  /* HUM Ch 39.2.7 "ICSER : I2C Bus Status Enable Register" p 2380 */
  reg->ICSER = 0U;
  /* HUM Ch 39.2.5 "ICMR3 : I2C Bus Mode Register 3 -- WAIT" p 2376 */
  reg->ICMR3 = (uint8_t)(reg->ICMR3 & (uint8_t)~(uint8_t)k_ra8_i2c_msk_icmr3_wait);
  /* HUM Ch 39.2.8 "ICIER : I2C Bus Interrupt Enable Register" p 2381 */
  reg->ICIER = (uint8_t)(reg->ICIER & (uint8_t)~(uint8_t)k_ra8_i2c_peripheral_icier_arm);
  s_i2c_state[channel].peripheral_active = false;
  return k_ra8_ok;
}

ra8_err_t ra8_i2c_peripheral_poll(uint8_t channel, ra8_i2c_peripheral_event_t* out_event)
{
  volatile const r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_i2c_tag, "i2c_target_poll: channel");
  RA8_CHECK_NULL_PTR(out_event, s_i2c_tag, "i2c_target_poll: out_event");
  *out_event = k_ra8_i2c_peripheral_event_none;
  if (!s_i2c_state[channel].peripheral_active) {
    return k_ra8_err_not_initialized;
  }

  for (uint32_t i = 0U; i < (uint32_t)k_ra8_i2c_peripheral_poll_limit;
       i++) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 39.2.9 "ICSR1 : I2C Bus Status Register 1" p 2382 */
    const uint8_t icsr1 = reg->ICSR1;
    /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
    const uint8_t icsr2 = reg->ICSR2;
    if (ra8_i2c_internal_peripheral_poll_done(icsr1, icsr2)) { /* GCOVR_EXCL_BR_LINE */
      break;
    }
  }

  /* HUM Ch 39.2.9 "ICSR1 : I2C Bus Status Register 1" p 2382 */
  const uint8_t icsr1 = reg->ICSR1;
  /* HUM Ch 39.2.2 "ICCR2 : I2C Bus Control Register 2 -- TRS" p 2371 */
  const uint8_t iccr2 = reg->ICCR2;
  *out_event          = internal_i2c_target_classify(icsr1, iccr2);
  return k_ra8_ok;
}

ra8_err_t
ra8_i2c_peripheral_receive(uint8_t channel, uint8_t* buf, uint32_t capacity, uint32_t* out_received)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_i2c_tag, "i2c_target_receive: channel");
  RA8_CHECK_NULL_PTR(buf, s_i2c_tag, "i2c_target_receive: buf");
  RA8_CHECK_NULL_PTR(out_received, s_i2c_tag, "i2c_target_receive: out_received");
  *out_received = 0U;
  if (capacity == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_i2c_state[channel].peripheral_active) {
    return k_ra8_err_not_initialized;
  }

  /* Step 3 (peripheral receive): the address-phase RDRF, then dummy-read
   * ICDRR to discard the matched address byte and clear RDRF.
   * HUM Ch 39.3.6 "Peripheral Receive Operation" p 2408 */
  ra8_err_t err = internal_i2c_target_wait(reg, (uint8_t)k_ra8_i2c_msk_icsr2_rdrf);
  if (err != k_ra8_ok) {
    return err;
  }
  /* HUM Ch 39.2.18 "ICDRR : I2C Bus Receive Data Register" p 2393 */
  (void)reg->ICDRR;

  /* Steps 4-5: drain data bytes until STOP or the buffer fills. */
  uint32_t count = 0U;
  err            = ra8_i2c_internal_target_drain_rx(reg, buf, capacity, &count);

  /* Step 6: clear the STOP flag (W0C) for the next transfer.
   * HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2 -- STOP W0C" p 2384 */
  reg->ICSR2    = (uint8_t)(reg->ICSR2 & (uint8_t)~(uint8_t)k_ra8_i2c_msk_icsr2_stop);
  *out_received = count;
  return (count == 0U) ? err : k_ra8_ok;
}

ra8_err_t
ra8_i2c_peripheral_transmit(uint8_t channel, const uint8_t* data, uint32_t len, uint32_t* out_sent)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_i2c_tag, "i2c_target_transmit: channel");
  RA8_CHECK_NULL_PTR(data, s_i2c_tag, "i2c_target_transmit: data");
  RA8_CHECK_NULL_PTR(out_sent, s_i2c_tag, "i2c_target_transmit: out_sent");
  *out_sent = 0U;
  if (len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_i2c_state[channel].peripheral_active) {
    return k_ra8_err_not_initialized;
  }

  /* Step 3: push data bytes to ICDRT until NACK or every byte is queued. */
  uint32_t sent = 0U;
  internal_i2c_target_fill_tx(reg, data, len, &sent);

  /* Steps 4-7: confirm the controller ended the read, release SCL, clear
   * NACKF / STOP. ``ended`` is k_ra8_ok-equivalent: the end-of-frame poll only
   * times out (ended == false) when no TEND / NACK ever arrives, which is the
   * single failure mode for both an empty and a non-empty transmit. */
  const bool ended = internal_i2c_target_finish_tx(reg);
  *out_sent        = sent;
  return ended ? k_ra8_ok : k_ra8_err_hw_timeout;
}

/* =============================================================================
 * Event dispatch -- attached-callback path (RIIC ISR or poll loop).
 * =============================================================================
 */

/**
 * @brief Classify an own-address / STOP status snapshot into a dispatch event.
 *
 * @details
 * Reduces the latched snapshot to a single ``ra8_i2c_peripheral_event_t``. An
 * own-address (or general-call) match decodes via ICCR2.TRS to ``read``
 * (controller reads -> target transmits) or ``write`` (controller writes ->
 * target receives) through ``internal_i2c_target_classify``. With no match a
 * latched ICSR2.STOP decodes to ``stop``; otherwise ``none``. Match and STOP
 * are mutually exclusive in hardware -- the AASy flags clear on the STOP -- so
 * a match is tested first.
 *
 * @param[in] icsr1 Snapshot of ICSR1 (own-address detection flags).
 * @param[in] icsr2 Snapshot of ICSR2 (STOP detection flag).
 * @param[in] iccr2 Snapshot of ICCR2 (transmit/receive mode bit TRS).
 *
 * @return The decoded ``ra8_i2c_peripheral_event_t``.
 * @retval k_ra8_i2c_peripheral_event_write Match, controller writes.
 * @retval k_ra8_i2c_peripheral_event_read  Match, controller reads.
 * @retval k_ra8_i2c_peripheral_event_stop  No match but a STOP is latched.
 * @retval k_ra8_i2c_peripheral_event_none  Neither a match nor a STOP.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the three input snapshots.
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static ra8_i2c_peripheral_event_t
internal_i2c_dispatch_event(uint8_t icsr1, uint8_t icsr2, uint8_t iccr2)
{
  const ra8_i2c_peripheral_event_t match = internal_i2c_target_classify(icsr1, iccr2);
  if (match != k_ra8_i2c_peripheral_event_none) {
    return match;
  }
  if ((icsr2 & (uint8_t)k_ra8_i2c_msk_icsr2_stop) != 0U) {
    return k_ra8_i2c_peripheral_event_stop;
  }
  return k_ra8_i2c_peripheral_event_none;
}

ra8_err_t
ra8_i2c_peripheral_attach_handler(uint8_t channel, ra8_i2c_peripheral_event_fn_t fn, void* ctx)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_i2c_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  s_i2c_state[channel].peripheral_handler = fn;
  s_i2c_state[channel].peripheral_ctx     = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_i2c_peripheral_dispatch(uint8_t channel)
{
  volatile const r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  if (reg == nullptr) {
    return;
  }
  if (!s_i2c_state[channel].peripheral_active) {
    return;
  }
  const ra8_i2c_peripheral_event_fn_t fn = s_i2c_state[channel].peripheral_handler;
  if (fn == nullptr) {
    return;
  }

  /* HUM Ch 39.2.9 "ICSR1 : I2C Bus Status Register 1" p 2382 */
  const uint8_t icsr1 = reg->ICSR1;
  /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
  const uint8_t icsr2 = reg->ICSR2;
  /* HUM Ch 39.2.2 "ICCR2 : I2C Bus Control Register 2 -- TRS" p 2371 */
  const uint8_t iccr2 = reg->ICCR2;

  const ra8_i2c_peripheral_event_t event = internal_i2c_dispatch_event(icsr1, icsr2, iccr2);
  if (event != k_ra8_i2c_peripheral_event_none) {
    fn(s_i2c_state[channel].peripheral_ctx, event);
  }
}
