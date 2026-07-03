/**
 * @file ra_sci_lin.c
 * @brief LIN commander-mode driver implementation on SCI_B
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements the LIN commander API declared in ``ra_sci_lin.h`` on top of
 * the SCI_B Simple-LIN sub-mode (HUM Ch 38). Break-field generation uses
 * the dedicated break-field timer (XCR0.TCSS clock, XCR0.BFE, XCR2.BFLW
 * length, XCR1.TCST start trigger); the SYNC byte and protected identifier
 * are sent as ordinary UART frames through ``ra_sci_putc_polling``.
 *
 * The base async-UART bring-up (MSTP gate, baud, framing) is delegated to
 * ``ra_sci_init`` so this file owns only the LIN-specific register
 * sequence and the pure PID / checksum math. It deliberately keeps the
 * driver small -- it does not touch the per-channel async TX/RX dispatch
 * state owned by ``ra_sci.c``.
 *
 * @par Inclusive-terminology note
 * The LIN commander is the bus controller; this driver implements only
 * that role. Subordinate nodes are termed "responders". The legacy LIN
 * node words are avoided in favour of Commander / Responder.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sci_lin.h"

#include <stdint.h>

#include "ra8d2_sci_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_hw_err.h"
#include "ra_log.h"
#include "ra_sci.h"

static const char* s_tag = "LIN";

/* =============================================================================
 * File-local constants
 * =============================================================================
 */

/**
 * @enum ra_sci_lin_pid_shift_t
 * @brief Bit positions used when packing / unpacking a LIN PID.
 *
 * @details The frame id occupies bits 0..5; the two parity bits land in
 * bits 6 (P0) and 7 (P1). HUM is not the source here -- these are LIN
 * protocol bit positions, not register fields.
 */
typedef enum : uint8_t {
  k_ra_sci_lin_id_bit0      = 0U, /**< Frame-id bit 0 position.    */
  k_ra_sci_lin_id_bit1      = 1U, /**< Frame-id bit 1 position.    */
  k_ra_sci_lin_id_bit2      = 2U, /**< Frame-id bit 2 position.    */
  k_ra_sci_lin_id_bit3      = 3U, /**< Frame-id bit 3 position.    */
  k_ra_sci_lin_id_bit4      = 4U, /**< Frame-id bit 4 position.    */
  k_ra_sci_lin_id_bit5      = 5U, /**< Frame-id bit 5 position.    */
  k_ra_sci_lin_pid_p0_shift = 6U, /**< PID parity bit P0 position. */
  k_ra_sci_lin_pid_p1_shift = 7U, /**< PID parity bit P1 position. */
} ra_sci_lin_pid_shift_t;

/**
 * @enum ra_sci_lin_const_t
 * @brief Byte-level masks and protocol constants for the LIN math.
 */
typedef enum : uint8_t {
  k_ra_sci_lin_id_mask     = 0x3FU, /**< 6-bit LIN frame-id mask.        */
  k_ra_sci_lin_bit_mask    = 0x01U, /**< Single-bit extract / invert.    */
  k_ra_sci_lin_sync_byte   = 0x55U, /**< LIN SYNC field byte (0x55).     */
  k_ra_sci_lin_fold_passes = 2U,    /**< Carry-fold passes for checksum. */
} ra_sci_lin_const_t;

/**
 * @enum ra_sci_lin_fold_const_t
 * @brief 16-bit masks used while folding the LIN checksum carry.
 */
typedef enum : uint16_t {
  k_ra_sci_lin_byte_mask = 0x00FFU, /**< Low-byte mask.               */
  k_ra_sci_lin_byte_bits = 8U,      /**< Bits per byte (carry shift). */
} ra_sci_lin_fold_const_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Program the SCI_B mode + break-field timer for LIN commander use.
 *
 * @details
 * Runs after ``ra_sci_init`` has set up baud / framing. Drops CCR0 to take
 * the transmitter offline, switches CCR3.MOD to Simple LIN while preserving
 * the framing bits, programs the break-field timer clock and length, clears
 * XCR1, and re-enables TE + RE. Called once during init under an IRQ-masked
 * or single-threaded context, so no register guard is needed.
 *
 * @param[in,out] reg       Channel register bank, non-NULL.
 * @param[in]     tcss      XCR0.TCSS timer-clock encoding (1..3).
 * @param[in]     break_len XCR2.BFLW break-field length value.
 *
 * @pre ``reg`` is the canonical bank pointer for the active channel.
 * @pre ``ra_sci_init`` has already programmed CCR1..CCR4.
 * @post CCR3.MOD == Simple LIN; XCR0/XCR2 hold the break-field config.
 * @post CCR0.TE and CCR0.RE are set.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_lin_program_mode(volatile r_sci_regs_t* reg, uint32_t tcss, uint16_t break_len)
{
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0" p 2182 -- drop TE/RE
   * before changing the operating mode. */
  reg->CCR0 = 0U;

  /* HUM Ch 38.2.8 "CCR3 : Common Control Register 3" p 2203 -- select
   * Simple LIN (MOD = 110b), preserving LSBF / BPEN / CHR / STP set by
   * ra_sci_init. */
  uint32_t ccr3 = reg->CCR3;
  ccr3 &= ~(uint32_t)k_ra_sci_ccr3_mask_mod;
  ccr3 |= ((uint32_t)k_ra_sci_ccr3_mod_simple_lin << k_ra_sci_ccr3_shift_mod);
  reg->CCR3 = ccr3;

  /* HUM Ch 38.2.14 "XCR0 : Simple LIN Control Register 0" p 2220 -- select
   * the break-field timer clock (TCSS) and enable break-field generation
   * for the start frame (BFE). */
  reg->XCR0 = (tcss << k_ra_sci_xcr0_shift_tcss) | (1U << k_ra_sci_xcr0_bit_bfe);

  /* HUM Ch 38.2.16 "XCR2 : Simple LIN Control Register 2" p 2224 -- break
   * dominant time = (BFLW + 1) x timer clock. */
  reg->XCR2 = ((uint32_t)break_len << k_ra_sci_xcr2_shift_bflw);

  /* HUM Ch 38.2.15 "XCR1 : Simple LIN Control Register 1" p 2223 -- idle
   * the trigger / detection field before going live. */
  reg->XCR1 = 0U;

  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0" p 2182 -- re-enable
   * the transmitter and receiver. */
  reg->CCR0 = (1U << k_ra_sci_ccr0_bit_te) | (1U << k_ra_sci_ccr0_bit_re);
}

/**
 * @brief Spin until the break-field timer (XCR1.TCST) self-clears.
 *
 * @details
 * On the target, TCST holds 1 while the break field is on the wire and
 * self-clears when the BFLW-programmed dominant time completes. On the
 * host (``RA_SIMULATOR_MODE``) the timer drain is not modelled, so the
 * routine short-circuits to success.
 *
 * @param[in] reg Channel register bank, non-NULL.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok            TCST observed clear (or simulator stub).
 * @retval k_ra_err_hw_timeout TCST did not clear within the budget.
 *
 * @pre ``reg`` is non-NULL.
 * @pre A break field was just started via XCR1.TCST.
 * @post On success, the break-field timer is idle.
 * @post No data register is touched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_lin_wait_break_done(volatile const r_sci_regs_t* reg)
{
#ifdef RA_SIMULATOR_MODE
  (void)reg;
  return k_ra_ok;
#else
  /* HUM Ch 38.2.15 "XCR1 : Simple LIN Control Register 1" p 2223 -- TCST
   * holds 1 during break output and clears on completion. */
  const uint32_t mask = (1U << k_ra_sci_xcr1_bit_tcst);
  return ra_hw_wait_flag_clear32(&reg->XCR1, mask, k_ra_hw_budget_long);
#endif
}

/**
 * @brief Fold a LIN checksum accumulator and return its one's complement.
 *
 * @details
 * Folds the carry bits back into the low byte to form the modulo-255 sum,
 * then returns its inversion. Two folds cover the worst case (a PID plus up
 * to 8 data bytes sums to less than 0x900, so one residual carry remains
 * after the first fold).
 *
 * @param[in] sum Running byte sum (PID optionally pre-added).
 *
 * @return The inverted modulo-255 sum (the LIN checksum byte).
 * @retval 0x00..0xFF One's complement of the folded modulo-255 sum.
 *
 * @pre ``sum`` is the unfolded accumulator of a LIN frame.
 * @pre The frame had at most 8 data bytes (standard LIN).
 * @post The result is the one's complement of the modulo-255 sum.
 * @post No state outside the function is modified.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t internal_lin_fold_complement(uint16_t sum)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_sci_lin_fold_passes; ++i) {
    const uint16_t low  = (uint16_t)(sum & (uint16_t)k_ra_sci_lin_byte_mask);
    const uint16_t high = (uint16_t)(sum >> (uint16_t)k_ra_sci_lin_byte_bits);
    sum                 = (uint16_t)(low + high);
  }
  return (uint8_t)(~sum & (uint16_t)k_ra_sci_lin_byte_mask);
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra_err_t ra_sci_lin_init(uint8_t channel, const ra_sci_lin_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "lin_init: cfg");
  volatile r_sci_regs_t* reg = ra_sci(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "lin_init: channel out of range");
  if (cfg->break_field_len > (uint16_t)k_ra_sci_xcr2_bflw_max) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->timer_clk < k_ra_sci_lin_clk_div4) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->timer_clk > k_ra_sci_lin_clk_div64) {
    return k_ra_err_invalid_arg;
  }

  const ra_err_t base_err = ra_sci_init(channel, &cfg->uart);
  RA_RETURN_ON_ERROR(base_err, s_tag, "lin_init: base uart");

  internal_lin_program_mode(reg, (uint32_t)cfg->timer_clk, cfg->break_field_len);
  ra_log_info_val(s_tag, "lin_init channel", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_sci_lin_send_break(uint8_t channel)
{
  volatile r_sci_regs_t* reg = ra_sci(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "lin_send_break: channel out of range");
  /* HUM Ch 38.2.15 "XCR1 : Simple LIN Control Register 1" p 2223 -- writing
   * 1 to TCST starts break-field output on TXD; the bit self-clears when the
   * BFLW dominant time elapses. The other XCR1 fields stay idle in commander
   * mode, so a full-word write is deterministic. */
  reg->XCR1 = (1U << k_ra_sci_xcr1_bit_tcst);
  return internal_lin_wait_break_done(reg);
}

ra_err_t ra_sci_lin_send_header(uint8_t channel, uint8_t id)
{
  volatile r_sci_regs_t* reg = ra_sci(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "lin_send_header: channel out of range");
  if (id > (uint8_t)k_ra_sci_lin_id_max) {
    return k_ra_err_invalid_arg;
  }

  const ra_err_t brk_err = ra_sci_lin_send_break(channel);
  RA_RETURN_ON_ERROR(brk_err, s_tag, "lin_send_header: break");

  /* SYNC field: the fixed 0x55 byte the responders use to lock the bit
   * rate, sent as an ordinary UART frame after the break. */
  const ra_err_t sync_err = ra_sci_putc_polling(channel, (uint8_t)k_ra_sci_lin_sync_byte);
  RA_RETURN_ON_ERROR(sync_err, s_tag, "lin_send_header: sync");

  /* Protected identifier: 6-bit id with the two LIN parity bits. */
  const uint8_t  pid     = ra_sci_lin_pid(id);
  const ra_err_t pid_err = ra_sci_putc_polling(channel, pid);
  RA_RETURN_ON_ERROR(pid_err, s_tag, "lin_send_header: pid");
  return k_ra_ok;
}

uint8_t ra_sci_lin_pid(uint8_t id)
{
  /* Mask any caller bits above bit 5 so parity is computed over a legal
   * 6-bit identifier (the public range check lives in send_header). */
  const uint8_t id6 = (uint8_t)(id & (uint8_t)k_ra_sci_lin_id_mask);
  const uint8_t b0  = (uint8_t)((id6 >> k_ra_sci_lin_id_bit0) & (uint8_t)k_ra_sci_lin_bit_mask);
  const uint8_t b1  = (uint8_t)((id6 >> k_ra_sci_lin_id_bit1) & (uint8_t)k_ra_sci_lin_bit_mask);
  const uint8_t b2  = (uint8_t)((id6 >> k_ra_sci_lin_id_bit2) & (uint8_t)k_ra_sci_lin_bit_mask);
  const uint8_t b3  = (uint8_t)((id6 >> k_ra_sci_lin_id_bit3) & (uint8_t)k_ra_sci_lin_bit_mask);
  const uint8_t b4  = (uint8_t)((id6 >> k_ra_sci_lin_id_bit4) & (uint8_t)k_ra_sci_lin_bit_mask);
  const uint8_t b5  = (uint8_t)((id6 >> k_ra_sci_lin_id_bit5) & (uint8_t)k_ra_sci_lin_bit_mask);
  /* P0 = ID0 ^ ID1 ^ ID2 ^ ID4 (even parity over the LIN-specified set). */
  const uint8_t p0 = (uint8_t)(b0 ^ b1 ^ b2 ^ b4);
  /* P1 = NOT(ID1 ^ ID3 ^ ID4 ^ ID5) (odd parity over the LIN set). */
  const uint8_t p1 = (uint8_t)((b1 ^ b3 ^ b4 ^ b5) ^ (uint8_t)k_ra_sci_lin_bit_mask);
  return (uint8_t)(id6 | (uint8_t)(p0 << k_ra_sci_lin_pid_p0_shift) |
                   (uint8_t)(p1 << k_ra_sci_lin_pid_p1_shift));
}

ra_err_t ra_sci_lin_checksum(ra_sci_lin_checksum_mode_t mode,
                             uint8_t                    pid,
                             const uint8_t*             data,
                             uint8_t                    len,
                             uint8_t*                   out_checksum)
{
  RA_CHECK_NULL_PTR(out_checksum, s_tag, "lin_checksum: out_checksum");
  if (data == nullptr) {
    if (len != 0U) {
      return k_ra_err_null_ptr;
    }
  }
  if (mode > k_ra_sci_lin_checksum_enhanced) {
    return k_ra_err_invalid_arg;
  }

  uint16_t sum = 0U;
  /* Enhanced (LIN 2.x) folds the protected identifier into the sum;
   * classic (LIN 1.x) sums the data bytes only. */
  if (mode == k_ra_sci_lin_checksum_enhanced) {
    sum = (uint16_t)pid;
  }
  for (uint8_t i = 0U; i < len; ++i) {
    sum = (uint16_t)(sum + (uint16_t)data[i]);
  }
  *out_checksum = internal_lin_fold_complement(sum);
  return k_ra_ok;
}
