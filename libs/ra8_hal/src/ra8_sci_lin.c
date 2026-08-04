/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_sci_lin.c
 * @brief LIN commander + responder driver implementation on SCI_B
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements the LIN API declared in ``ra8_sci_lin.h`` on top of the SCI_B
 * Simple-LIN sub-mode (HUM Ch 38.11 "Simple LIN Mode", p 2338). Break-field
 * generation (commander) uses the dedicated break-field timer (XCR0.TCSS
 * clock, XCR0.BFE, XCR2.BFLW length, XCR1.TCST start trigger); break-field
 * detection (responder) uses Start-Frame detection (XCR1.SDST) plus bit-rate
 * measurement (XCR1.BMEN) and polls XSR0.BFDF, clearing the LIN latches
 * through XFCLR. The SYNC byte, protected identifier, and response data are
 * sent / received as ordinary UART frames through ``ra8_sci_putc_polling`` /
 * ``ra8_sci_getc_polling``.
 *
 * The base async-UART bring-up (MSTP gate, baud, framing) is delegated to
 * ``ra8_sci_init`` so this file owns only the LIN-specific register
 * sequence and the pure PID / checksum / header-validation math. It
 * deliberately keeps the driver small -- it does not touch the per-channel
 * async TX/RX dispatch state owned by ``ra8_sci.c``.
 *
 * @par Inclusive-terminology note
 * The LIN commander is the bus controller; responders are the subordinate
 * nodes. The legacy LIN node words are avoided in favour of Commander /
 * Responder.
 */

#include "ra8_sci_lin.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_sci.h"
#include "ra8_sci_regs.h"

static const char* s_tag = "LIN";

/* =============================================================================
 * File-local constants
 * =============================================================================
 */

/**
 * @enum ra8_sci_lin_pid_shift_t
 * @brief Bit positions used when packing / unpacking a LIN PID.
 *
 * @details The frame id occupies bits 0..5; the two parity bits land in
 * bits 6 (P0) and 7 (P1). HUM is not the source here -- these are LIN
 * protocol bit positions, not register fields.
 */
typedef enum : uint8_t {
  k_ra8_sci_lin_id_bit0      = 0U, /**< Frame-id bit 0 position.    */
  k_ra8_sci_lin_id_bit1      = 1U, /**< Frame-id bit 1 position.    */
  k_ra8_sci_lin_id_bit2      = 2U, /**< Frame-id bit 2 position.    */
  k_ra8_sci_lin_id_bit3      = 3U, /**< Frame-id bit 3 position.    */
  k_ra8_sci_lin_id_bit4      = 4U, /**< Frame-id bit 4 position.    */
  k_ra8_sci_lin_id_bit5      = 5U, /**< Frame-id bit 5 position.    */
  k_ra8_sci_lin_pid_p0_shift = 6U, /**< PID parity bit P0 position. */
  k_ra8_sci_lin_pid_p1_shift = 7U, /**< PID parity bit P1 position. */
} ra8_sci_lin_pid_shift_t;

/**
 * @enum ra8_sci_lin_const_t
 * @brief Byte-level masks and protocol constants for the LIN math.
 */
typedef enum : uint8_t {
  k_ra8_sci_lin_id_mask     = 0x3FU, /**< 6-bit LIN frame-id mask.        */
  k_ra8_sci_lin_bit_mask    = 0x01U, /**< Single-bit extract / invert.    */
  k_ra8_sci_lin_sync_byte   = 0x55U, /**< LIN SYNC field byte (0x55).     */
  k_ra8_sci_lin_fold_passes = 2U,    /**< Carry-fold passes for checksum. */
  k_ra8_sci_lin_data_max    = 8U,    /**< Max LIN response data bytes.    */
} ra8_sci_lin_const_t;

/**
 * @enum ra8_sci_lin_fold_const_t
 * @brief 16-bit masks used while folding the LIN checksum carry.
 */
typedef enum : uint16_t {
  k_ra8_sci_lin_byte_mask = 0x00FFU, /**< Low-byte mask.               */
  k_ra8_sci_lin_byte_bits = 8U,      /**< Bits per byte (carry shift). */
} ra8_sci_lin_fold_const_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Program the SCI_B mode + break-field timer/detector for a LIN role.
 *
 * @details
 * Runs after ``ra8_sci_init`` has set up baud / framing. Drops CCR0 to take
 * the transmitter offline, switches CCR3.MOD to Simple LIN while preserving
 * the framing bits, programs the break-field timer clock and length, sets
 * the role-specific XCR1 (commander: idle; responder: SDST + BMEN), and
 * re-enables TE + RE. Called once during init under an IRQ-masked or
 * single-threaded context, so no register guard is needed.
 *
 * @param[in,out] reg       Channel register bank, non-NULL.
 * @param[in]     role      Commander (generate) or responder (detect).
 * @param[in]     tcss      XCR0.TCSS timer-clock encoding (1..3).
 * @param[in]     break_len XCR2.BFLW break-field length value.
 *
 * @pre ``reg`` is the canonical bank pointer for the active channel.
 * @pre ``ra8_sci_init`` has already programmed CCR1..CCR4.
 * @post CCR3.MOD == Simple LIN; XCR0/XCR2 hold the break-field config.
 * @post XCR1 reflects ``role``; CCR0.TE and CCR0.RE are set.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_lin_program_mode(volatile r_sci_regs_t* reg,
                                      ra8_sci_lin_role_t     role,
                                      uint32_t               tcss,
                                      uint16_t               break_len)
{
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0" p 2182 -- drop TE/RE
   * before changing the operating mode. */
  reg->CCR0 = 0U;

  /* HUM Ch 38.2.8 "CCR3 : Common Control Register 3" p 2203 -- select
   * Simple LIN (MOD = 110b), preserving LSBF / BPEN / CHR / STP set by
   * ra8_sci_init. */
  uint32_t ccr3 = reg->CCR3;
  ccr3 &= ~(uint32_t)k_ra8_sci_ccr3_mask_mod;
  ccr3 |= ((uint32_t)k_ra8_sci_ccr3_mod_simple_lin << k_ra8_sci_ccr3_shift_mod);
  reg->CCR3 = ccr3;

  /* HUM Ch 38.2.14 "XCR0 : Simple LIN Control Register 0" p 2220 -- select
   * the break-field timer clock (TCSS) and enable break-field logic (BFE);
   * BFE gates the responder's break detector as well as commander output. */
  reg->XCR0 = (tcss << k_ra8_sci_xcr0_shift_tcss) | (1U << k_ra8_sci_xcr0_bit_bfe);

  /* HUM Ch 38.2.16 "XCR2 : Simple LIN Control Register 2" p 2224 -- break
   * dominant time = (BFLW + 1) x timer clock. */
  reg->XCR2 = ((uint32_t)break_len << k_ra8_sci_xcr2_shift_bflw);

  /* HUM Ch 38.2.15 "XCR1 : Simple LIN Control Register 1" p 2223 -- a
   * commander leaves the trigger / detection fields idle (TCST is pulsed per
   * frame by ra8_sci_lin_send_break); a responder arms Start-Frame detection
   * (SDST) and bit-rate measurement (BMEN) so an inbound break sets XSR0.BFDF
   * (HUM Ch 38.11.2 "Simple LIN Start Frame Reception" p 2341). */
  uint32_t xcr1 = 0U;
  if (role == k_ra8_sci_lin_role_responder) {
    xcr1 = (1U << k_ra8_sci_xcr1_bit_sdst) | (1U << k_ra8_sci_xcr1_bit_bmen);
  }
  reg->XCR1 = xcr1;

  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0" p 2182 -- re-enable
   * the transmitter and receiver. */
  reg->CCR0 = (1U << k_ra8_sci_ccr0_bit_te) | (1U << k_ra8_sci_ccr0_bit_re);
}

/**
 * @brief Spin until the break-field timer (XCR1.TCST) self-clears.
 *
 * @details
 * On the target, TCST holds 1 while the break field is on the wire and
 * self-clears when the BFLW-programmed dominant time completes. On the
 * host (``RA8_OFF_TARGET``) the timer drain is not modelled, so the
 * routine short-circuits to success.
 *
 * @param[in] reg Channel register bank, non-NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            TCST observed clear (or fake stub).
 * @retval k_ra8_err_hw_timeout TCST did not clear within the budget.
 *
 * @pre ``reg`` is non-NULL.
 * @pre A break field was just started via XCR1.TCST.
 * @post On success, the break-field timer is idle.
 * @post No data register is touched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_lin_wait_break_done(volatile const r_sci_regs_t* reg)
{
  /* HUM Ch 38.2.15 "XCR1 : Simple LIN Control Register 1" p 2223 -- TCST holds 1
   * during break output and clears on completion. The ra8_fake_mmio host seam
   * drives this real poll on the unit-test build (T1-01): default RAM (TCST
   * clear) takes the success leg, a staged TCST runs the loop to its timeout. */
  const uint32_t mask = (1U << k_ra8_sci_xcr1_bit_tcst);
  return ra8_hw_wait_flag_clear32(&reg->XCR1, mask, k_ra8_hw_budget_long);
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
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_sci_lin_fold_passes; ++i) {
    const uint16_t low  = (uint16_t)(sum & (uint16_t)k_ra8_sci_lin_byte_mask);
    const uint16_t high = (uint16_t)(sum >> (uint16_t)k_ra8_sci_lin_byte_bits);
    sum                 = (uint16_t)(low + high);
  }
  return (uint8_t)(~sum & (uint16_t)k_ra8_sci_lin_byte_mask);
}

/**
 * @brief Poll-transmit ``len`` bytes of a LIN response data field.
 *
 * @details
 * Clocks ``data[0..len-1]`` out through ``ra8_sci_putc_polling``, returning
 * the first error if any byte's TDRE poll times out. Factored out of
 * ``ra8_sci_lin_send_response`` to keep that public function within the
 * NASA Rule 4 length budget.
 *
 * @param[in] channel SCI channel number (0..9).
 * @param[in] data    Non-NULL data-field byte buffer.
 * @param[in] len     Number of bytes to send (1..8, validated by caller).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            All ``len`` bytes clocked out.
 * @retval k_ra8_err_null_ptr  ``data`` was NULL.
 * @retval k_ra8_err_hw_timeout A TDRE poll timed out mid-frame.
 *
 * @pre ``data`` is non-NULL.
 * @pre The caller already validated ``channel`` and ``len``.
 * @post On success every data byte has reached TDR.
 * @post No control register is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_lin_tx_buf(uint8_t channel, const uint8_t* data, uint8_t len)
{
  RA8_CHECK_NULL_PTR(data, s_tag, "lin_tx_buf: data");
  for (uint8_t i = 0U; i < len; ++i) {
    const ra8_err_t err = ra8_sci_putc_polling(channel, data[i]);
    RA8_RETURN_ON_ERROR(err, s_tag, "lin_tx_buf");
  }
  return k_ra8_ok;
}

/**
 * @brief Poll-receive ``len`` bytes of a LIN response data field.
 *
 * @details
 * Drains ``len`` bytes from RDR into ``buf[0..len-1]`` through
 * ``ra8_sci_getc_polling``, returning the first error if any byte's RDRF
 * poll times out. Factored out of ``ra8_sci_lin_read_response`` to keep
 * that public function within the NASA Rule 4 length budget.
 *
 * @param[in]  channel SCI channel number (0..9).
 * @param[out] buf     Non-NULL destination for ``len`` bytes.
 * @param[in]  len     Number of bytes to read (1..8, validated by caller).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            All ``len`` bytes drained.
 * @retval k_ra8_err_null_ptr  ``buf`` was NULL.
 * @retval k_ra8_err_hw_timeout An RDRF poll timed out mid-frame.
 *
 * @pre ``buf`` is non-NULL.
 * @pre The caller already validated ``channel`` and ``len``.
 * @post On success ``buf[0..len-1]`` hold the received bytes.
 * @post No control register is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_lin_rx_buf(uint8_t channel, uint8_t* buf, uint8_t len)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "lin_rx_buf: buf");
  for (uint8_t i = 0U; i < len; ++i) {
    const ra8_err_t err = ra8_sci_getc_polling(channel, &buf[i]);
    RA8_RETURN_ON_ERROR(err, s_tag, "lin_rx_buf");
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_sci_lin_init(uint8_t channel, const ra8_sci_lin_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "lin_init: cfg");
  volatile r_sci_regs_t* reg = ra8_sci(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "lin_init: channel out of range");
  if (cfg->role > k_ra8_sci_lin_role_responder) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->break_field_len > (uint16_t)k_ra8_sci_xcr2_bflw_max) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->timer_clk < k_ra8_sci_lin_clk_div4) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->timer_clk > k_ra8_sci_lin_clk_div64) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t base_err = ra8_sci_init(channel, &cfg->uart);
  RA8_RETURN_ON_ERROR(base_err, s_tag, "lin_init: base uart");

  internal_lin_program_mode(reg, cfg->role, (uint32_t)cfg->timer_clk, cfg->break_field_len);
  ra8_log_info_val(s_tag, "lin_init channel", (uint32_t)channel);
  return k_ra8_ok;
}

ra8_err_t ra8_sci_lin_send_break(uint8_t channel)
{
  volatile r_sci_regs_t* reg = ra8_sci(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "lin_send_break: channel out of range");
  /* HUM Ch 38.2.15 "XCR1 : Simple LIN Control Register 1" p 2223 -- writing
   * 1 to TCST starts break-field output on TXD; the bit self-clears when the
   * BFLW dominant time elapses. The other XCR1 fields stay idle in commander
   * mode, so a full-word write is deterministic. */
  reg->XCR1 = (1U << k_ra8_sci_xcr1_bit_tcst);
  return internal_lin_wait_break_done(reg);
}

ra8_err_t ra8_sci_lin_send_header(uint8_t channel, uint8_t id)
{
  volatile r_sci_regs_t* reg = ra8_sci(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "lin_send_header: channel out of range");
  if (id > (uint8_t)k_ra8_sci_lin_id_max) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t brk_err = ra8_sci_lin_send_break(channel);
  RA8_RETURN_ON_ERROR(brk_err, s_tag, "lin_send_header: break");

  /* SYNC field: the fixed 0x55 byte the responders use to lock the bit
   * rate, sent as an ordinary UART frame after the break. */
  const ra8_err_t sync_err = ra8_sci_putc_polling(channel, (uint8_t)k_ra8_sci_lin_sync_byte);
  RA8_RETURN_ON_ERROR(sync_err, s_tag, "lin_send_header: sync");

  /* Protected identifier: 6-bit id with the two LIN parity bits. */
  const uint8_t   pid     = ra8_sci_lin_pid(id);
  const ra8_err_t pid_err = ra8_sci_putc_polling(channel, pid);
  RA8_RETURN_ON_ERROR(pid_err, s_tag, "lin_send_header: pid");
  return k_ra8_ok;
}

ra8_err_t ra8_sci_lin_break_detected(uint8_t channel, bool* out_detected)
{
  RA8_CHECK_NULL_PTR(out_detected, s_tag, "lin_break_detected: out_detected");
  volatile const r_sci_regs_t* reg = ra8_sci(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "lin_break_detected: channel out of range");
  /* HUM Ch 38.2.22 "XSR0 : Simple LIN Status Register 0" p 2235 -- BFDF is
   * set once the Start-Frame detector sees a break field; read-only here. */
  const uint32_t mask = (1U << k_ra8_sci_xsr0_bit_bfdf);
  *out_detected       = ((reg->XSR0 & mask) != 0U);
  return k_ra8_ok;
}

ra8_err_t ra8_sci_lin_wait_break(uint8_t channel)
{
  volatile const r_sci_regs_t* reg = ra8_sci(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "lin_wait_break: channel out of range");
  /* HUM Ch 38.2.22 "XSR0 : Simple LIN Status Register 0" p 2235 -- spin until
   * BFDF = 1 (break field detected). The ra8_fake_mmio host seam drives this
   * real poll on the unit-test build: a staged BFDF takes the success leg, an
   * armed fail runs the loop to its timeout. */
  const uint32_t mask = (1U << k_ra8_sci_xsr0_bit_bfdf);
  return ra8_hw_wait_flag_set32(&reg->XSR0, mask, k_ra8_hw_budget_long);
}

ra8_err_t ra8_sci_lin_clear_status(uint8_t channel)
{
  volatile r_sci_regs_t* reg = ra8_sci(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "lin_clear_status: channel out of range");
  /* HUM Ch 38.2.28 "XFCLR : Simple LIN Flag Clear Register" p 2240 -- writing
   * the clear-all-LIN mask clears every W1C XSR0 latch (BFDF, AEDF, ...). */
  reg->XFCLR = (uint32_t)k_ra8_sci_xfclr_default;
  return k_ra8_ok;
}

ra8_err_t ra8_sci_lin_send_response(uint8_t                     channel,
                                    ra8_sci_lin_checksum_mode_t mode,
                                    uint8_t                     pid,
                                    const uint8_t*              data,
                                    uint8_t                     len)
{
  volatile const r_sci_regs_t* reg = ra8_sci(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "lin_send_response: channel out of range");
  if (len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (len > (uint8_t)k_ra8_sci_lin_data_max) {
    return k_ra8_err_invalid_arg;
  }

  /* Compute the checksum first: this also validates mode and the data / len
   * pairing (NULL data with len > 0 -> k_ra8_err_null_ptr). */
  uint8_t         checksum = 0U;
  const ra8_err_t cerr     = ra8_sci_lin_checksum(mode, pid, data, len, &checksum);
  RA8_RETURN_ON_ERROR(cerr, s_tag, "lin_send_response: checksum");

  const ra8_err_t derr = internal_lin_tx_buf(channel, data, len);
  RA8_RETURN_ON_ERROR(derr, s_tag, "lin_send_response: data");
  return ra8_sci_putc_polling(channel, checksum);
}

ra8_err_t
ra8_sci_lin_read_response(uint8_t channel, uint8_t* out_data, uint8_t len, uint8_t* out_checksum)
{
  volatile const r_sci_regs_t* reg = ra8_sci(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "lin_read_response: channel out of range");
  RA8_CHECK_NULL_PTR(out_data, s_tag, "lin_read_response: out_data");
  RA8_CHECK_NULL_PTR(out_checksum, s_tag, "lin_read_response: out_checksum");
  if (len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (len > (uint8_t)k_ra8_sci_lin_data_max) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t derr = internal_lin_rx_buf(channel, out_data, len);
  RA8_RETURN_ON_ERROR(derr, s_tag, "lin_read_response: data");
  return ra8_sci_getc_polling(channel, out_checksum);
}

uint8_t ra8_sci_lin_pid(uint8_t id)
{
  /* Mask any caller bits above bit 5 so parity is computed over a legal
   * 6-bit identifier (the public range check lives in send_header). */
  const uint8_t id6 = (uint8_t)(id & (uint8_t)k_ra8_sci_lin_id_mask);
  const uint8_t b0  = (uint8_t)((id6 >> k_ra8_sci_lin_id_bit0) & (uint8_t)k_ra8_sci_lin_bit_mask);
  const uint8_t b1  = (uint8_t)((id6 >> k_ra8_sci_lin_id_bit1) & (uint8_t)k_ra8_sci_lin_bit_mask);
  const uint8_t b2  = (uint8_t)((id6 >> k_ra8_sci_lin_id_bit2) & (uint8_t)k_ra8_sci_lin_bit_mask);
  const uint8_t b3  = (uint8_t)((id6 >> k_ra8_sci_lin_id_bit3) & (uint8_t)k_ra8_sci_lin_bit_mask);
  const uint8_t b4  = (uint8_t)((id6 >> k_ra8_sci_lin_id_bit4) & (uint8_t)k_ra8_sci_lin_bit_mask);
  const uint8_t b5  = (uint8_t)((id6 >> k_ra8_sci_lin_id_bit5) & (uint8_t)k_ra8_sci_lin_bit_mask);
  /* P0 = ID0 ^ ID1 ^ ID2 ^ ID4 (even parity over the LIN-specified set). */
  const uint8_t p0 = (uint8_t)(b0 ^ b1 ^ b2 ^ b4);
  /* P1 = NOT(ID1 ^ ID3 ^ ID4 ^ ID5) (odd parity over the LIN set). */
  const uint8_t p1 = (uint8_t)((b1 ^ b3 ^ b4 ^ b5) ^ (uint8_t)k_ra8_sci_lin_bit_mask);
  return (uint8_t)(id6 | (uint8_t)(p0 << k_ra8_sci_lin_pid_p0_shift) |
                   (uint8_t)(p1 << k_ra8_sci_lin_pid_p1_shift));
}

ra8_err_t ra8_sci_lin_checksum(ra8_sci_lin_checksum_mode_t mode,
                               uint8_t                     pid,
                               const uint8_t*              data,
                               uint8_t                     len,
                               uint8_t*                    out_checksum)
{
  RA8_CHECK_NULL_PTR(out_checksum, s_tag, "lin_checksum: out_checksum");
  if (data == nullptr) {
    if (len != 0U) {
      return k_ra8_err_null_ptr;
    }
  }
  if (mode > k_ra8_sci_lin_checksum_enhanced) {
    return k_ra8_err_invalid_arg;
  }

  uint16_t sum = 0U;
  /* Enhanced (LIN 2.x) folds the protected identifier into the sum;
   * classic (LIN 1.x) sums the data bytes only. */
  if (mode == k_ra8_sci_lin_checksum_enhanced) {
    sum = (uint16_t)pid;
  }
  for (uint8_t i = 0U; i < len; ++i) {
    sum = (uint16_t)(sum + (uint16_t)data[i]);
  }
  *out_checksum = internal_lin_fold_complement(sum);
  return k_ra8_ok;
}

ra8_err_t ra8_sci_lin_check_header(uint8_t sync, uint8_t pid, uint8_t* out_id, bool* out_valid)
{
  RA8_CHECK_NULL_PTR(out_id, s_tag, "lin_check_header: out_id");
  RA8_CHECK_NULL_PTR(out_valid, s_tag, "lin_check_header: out_valid");

  const uint8_t id = (uint8_t)(pid & (uint8_t)k_ra8_sci_lin_id_mask);
  *out_id          = id;
  /* A responder accepts the header only when the SYNC field is exactly 0x55
   * AND the received PID matches the parity recomputed from its low 6 bits. */
  const bool sync_ok = (sync == (uint8_t)k_ra8_sci_lin_sync_byte);
  const bool pid_ok  = (ra8_sci_lin_pid(id) == pid);
  *out_valid         = sync_ok && pid_ok;
  return k_ra8_ok;
}

ra8_err_t ra8_sci_lin_check_response(ra8_sci_lin_checksum_mode_t mode,
                                     uint8_t                     pid,
                                     const uint8_t*              data,
                                     uint8_t                     len,
                                     uint8_t                     received,
                                     bool*                       out_valid)
{
  RA8_CHECK_NULL_PTR(out_valid, s_tag, "lin_check_response: out_valid");

  /* Recompute the expected checksum; ra8_sci_lin_checksum validates mode and
   * the data / len pairing, so a bad mode or NULL-with-len propagates here. */
  uint8_t         expected = 0U;
  const ra8_err_t cerr     = ra8_sci_lin_checksum(mode, pid, data, len, &expected);
  RA8_RETURN_ON_ERROR(cerr, s_tag, "lin_check_response: checksum");

  *out_valid = (expected == received);
  return k_ra8_ok;
}
