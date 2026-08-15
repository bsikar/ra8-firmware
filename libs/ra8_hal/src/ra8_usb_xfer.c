/**
 * @file ra8_usb_xfer.c
 * @brief USB device-mode data path: pipe queue, DCP control data, SETUP
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Device-mode transfer entry points: ``ra8_usb_queue_in`` /
 * ``ra8_usb_queue_out`` (pipe FIFO push / drain), the DCP control-IN data
 * stage (``ra8_usb_dcp_in_data`` and its chunk / ZLP helpers), the OUT
 * pipe re-arm / park controls, the SETUP-packet drains
 * (``ra8_usb_read_setup_if_valid`` / ``ra8_usb_read_setup_unconditional``),
 * and ``ra8_usb_control_response``. Split out of ``ra8_usb.c`` so every
 * translation unit stays under the 1000-line cap; the shared register and
 * FIFO helpers it calls live in ``ra8_usb.c`` (declared in
 * ``ra8_usb_internal.h``). Modelled on FSP ``r_usb_preg_abs.c`` /
 * ``r_usb_plibusbip.c``; no FSP source ships in this tree.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_usb.h"
#include "ra8_usb_internal.h"
#include "ra8_usb_regs.h"

static const char* s_tag = "USB";

/* =============================================================================
 * Device-mode data path (pipe queue + DCP control data)
 * =============================================================================
 */

/**
 * @brief Implementation of `ra8_usb_queue_in()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] pipe_num See implementation.
 * @param[in] data See implementation.
 * @param[in] len See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t
ra8_usb_queue_in(ra8_usb_speed_t speed, uint8_t pipe_num, const uint8_t* data, uint16_t len)
{
  /* THROUGHPUT NOTE.
   * queue_in writes ONE packet (up to MPS bytes) into the IN pipe's
   * CFIFO bank and asserts BVAL. The hardware transmits the bank on
   * the next IN token from the host. Bulk pipes run single-buffered
   * (PIPECFG.DBLB clear), so the next packet cannot be staged until
   * the current bank drains. Our measured ceiling (2.66 MB/s on HS)
   * is bounded by the Linux cdc_acm read-URB completion path on the
   * host, not by this code. See the matching note in
   * port/usbx/src/ux_dcd_ra8_usb.c (auto-echo block) for the full
   * chain-of-causality + what it would take to lift the ceiling. */
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((pipe_num == 0U) || (pipe_num > k_ra8_usb_max_pipe_num)) {
    return k_ra8_err_invalid_arg;
  }
  if ((len > k_ra8_usb_pipe_max_packet) || ((data == nullptr) && (len != 0U))) {
    return k_ra8_err_invalid_arg;
  }

  priv_select_cfifo(reg, pipe_num, true);
  const ra8_err_t ready = priv_wait_frdy(reg);
  RA8_RETURN_ON_ERROR(ready, s_tag, "queue_in: FRDY timeout");

  if (len > 0U) {
    priv_fifo_write(reg, data, len);
  }

  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979 */
  reg->CFIFOCTR = k_ra8_fifoctr_bval;
  priv_pipe_pid(reg, pipe_num, k_ra8_pid_buf);
  return k_ra8_ok;
}

/**
 * @brief Send the EP0 IN data-stage payload as one or more DCP chunks.
 *
 * @details Drives the multi-chunk loop so ``ra8_usb_dcp_in_data`` stays
 * under the clang-tidy statement-count threshold. Pushes at most
 * DCPMAXP bytes per iteration via ``priv_dcp_push_chunk``, then
 * raises DCPCTR.PID to BUF after the first successful push. The
 * controller services subsequent IN tokens automatically.
 *
 * @param[in,out] reg  Selected DCP register block (CFIFO already
 *                     pointed at DCP / IN direction by the caller).
 * @param[in]     data Source payload; must hold at least ``len`` bytes.
 * @param[in]     len  Total payload length in bytes; must be > 0.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            Payload fully queued; PID set to BUF.
 * @retval k_ra8_err_hw_timeout FRDY never asserted for some chunk.
 *
 * @pre ``reg`` is non-NULL and CFIFOSEL is already programmed for DCP IN.
 * @pre ``data`` is non-NULL and ``len > 0``.
 * @post On success, all ``len`` bytes have been queued and DCPCTR.PID == BUF.
 * @post On error, DCPCTR.PID is left unchanged from its prior value.
 *
 * @note Not thread-safe; caller holds the DCP lock.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_dcp_in_payload(volatile r_usb_regs_t* reg, const uint8_t* data, uint16_t len)
{
  uint16_t remaining  = len;
  uint16_t offset     = 0U;
  bool     pid_raised = false;
  while (remaining > 0U) {
    const uint16_t chunk =
      (remaining > k_ra8_usb_dcp_max_packet) ? (uint16_t)k_ra8_usb_dcp_max_packet : remaining;
    const ra8_err_t pushed = priv_dcp_push_chunk(reg, &data[offset], chunk);
    RA8_RETURN_ON_ERROR(pushed, s_tag, "dcp_in_data: chunk push failed");
    if (!pid_raised) {
      /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1999 */
      priv_dcp_pid(reg, k_ra8_pid_buf);
      pid_raised = true;
    }
    offset    = (uint16_t)(offset + chunk);
    remaining = (uint16_t)(remaining - chunk);
  }
  return k_ra8_ok;
}

/**
 * @brief Send a zero-length data stage on the DCP IN endpoint.
 *
 * @details Waits for FRDY, pulses CFIFOCTR.BVAL on an empty buffer
 * (producing a ZLP on the wire) and raises DCPCTR.PID to BUF.
 * Extracted from ``ra8_usb_dcp_in_data`` so the top-level function fits
 * under the clang-tidy statement-count threshold.
 *
 * @param[in,out] reg Selected DCP register block (CFIFO already
 *                    pointed at DCP / IN direction by the caller).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           ZLP queued; PID set to BUF.
 * @retval k_ra8_err_hw_timeout FRDY never asserted.
 *
 * @pre ``reg`` is non-NULL and CFIFOSEL is already programmed for DCP IN.
 * @pre USB module clock and power are on.
 * @post On success, an empty buffer is queued and DCPCTR.PID == BUF.
 * @post On error, DCPCTR.PID is left unchanged from its prior value.
 *
 * @note Not thread-safe; caller holds the DCP lock.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_dcp_in_zlp(volatile r_usb_regs_t* reg)
{
  const ra8_err_t ready = priv_wait_frdy(reg);
  RA8_RETURN_ON_ERROR(ready, s_tag, "dcp_in_data: FRDY timeout (zlp)");
  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979 */
  reg->CFIFOCTR = k_ra8_fifoctr_bval;
  /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1999 */
  priv_dcp_pid(reg, k_ra8_pid_buf);
  return k_ra8_ok;
}

/* Diagnostic probes for the DCP IN data push (read via JLink). */
volatile uint32_t g_dcp_push_count       = 0U;
volatile uint16_t g_dcp_dcpctr_pre_push  = 0U;
volatile uint16_t g_dcp_dcpctr_post_push = 0U;
volatile uint16_t g_dcp_cfifoctr_pre     = 0U;
volatile uint16_t g_dcp_cfifoctr_post    = 0U;
volatile uint16_t g_dcp_last_len         = 0U;
volatile uint8_t  g_dcp_last_err         = 0U; /* 0=ok, 1=frdy timeout, 2=null arg */

/**
 * @brief Implementation of `ra8_usb_dcp_in_data()`.
 * @details Push a control-IN data-stage payload (or zero-length packet) into
 *          the DCP FIFO, set BVAL=1 and DCPCTR.PID=BUF so the chip
 *          transmits on the next IN token from the host. Captures
 *          pre/post register snapshots into ::g_dcp_push_count and
 *          friends for JLink-readable diagnostic. The status stage (CCPL) is
 *          intentionally NOT pulsed here -- the bridge handles it on the CTSQ
 *          status-stage edge.
 * @param[in] speed Which controller (FS or HS).
 * @param[in] data  Payload bytes (may be NULL when len==0).
 * @param[in] len   Byte count; may exceed DCPMAXP and will be chunked.
 * @return ra8_err_t result code.
 * @retval k_ra8_ok               Payload queued; PID=BUF.
 * @retval k_ra8_err_invalid_arg  speed out of range OR data NULL with
 *                                len > 0.
 * @retval k_ra8_err_hw_timeout   FRDY never asserted within the bound.
 * @pre Caller has cleared INTSTS0.VALID (PID writes are gated by VALID
 *      per HUM Ch 37.2.31 p 2095).
 * @pre USB module clock and power are on.
 * @post On success, len bytes have been queued and DCPCTR.PID == BUF.
 * @post On error, DCPCTR.PID is left unchanged from its prior value.
 * @note Not thread-safe; caller holds the DCP lock.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_dcp_in_data(ra8_usb_speed_t speed, const uint8_t* data, uint16_t len)
{
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    g_dcp_last_err = 2U;
    return k_ra8_err_invalid_arg;
  }
  if ((data == nullptr) && (len != 0U)) {
    g_dcp_last_err = 2U;
    return k_ra8_err_invalid_arg;
  }
  g_dcp_push_count++;
  g_dcp_last_len        = len;
  g_dcp_dcpctr_pre_push = reg->DCPCTR;

  /* Select DCP (CURPIPE = 0) on CFIFO in IN direction. Done once; the
   * selection persists across chunks.
   * HUM Ch 36.2.7 "CFIFOSEL : CFIFO Port Select Register", p 1976 */
  priv_select_cfifo(reg, 0U, true);
  g_dcp_cfifoctr_pre = reg->CFIFOCTR;

  /* Discard any stale, unconsumed buffer content before writing the
   * fresh response. If a previous control-IN payload was never pulled
   * by the host (a missed response window followed by a SETUP
   * retransmit), the CFIFO is left non-empty, FRDY write-ready never
   * re-asserts, and priv_wait_frdy below times out. Mirrors the
   * hw_usb_set_bclr in FSP usb_pstd_ctrl_read.
   * HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979 */
  reg->CFIFOCTR = (uint16_t)k_ra8_fifoctr_bclr;

  ra8_err_t err;
  if (len == 0U) {
    err = internal_dcp_in_zlp(reg);
  } else {
    err = internal_dcp_in_payload(reg, data, len);
  }
  g_dcp_cfifoctr_post    = reg->CFIFOCTR;
  g_dcp_dcpctr_post_push = reg->DCPCTR;
  g_dcp_last_err         = (uint8_t)((err == k_ra8_ok) ? 0U : 1U);
  return err;
}

/**
 * @brief Argument validation helper for `ra8_usb_queue_out`.
 *
 * @details Read-only over the buffers; the caller mutates them on
 * success. Marked `const` so clang-tidy's `readability-non-const-parameter`
 * is satisfied.
 *
 * @param[in] pipe_num See implementation.
 * @param[in] out_buf See implementation.
 * @param[in] inout_len See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_check_queue_out_args(uint8_t pipe_num, const uint8_t* out_buf, const uint16_t* inout_len)
{
  if ((out_buf == nullptr) || (inout_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((pipe_num == 0U) || (pipe_num > k_ra8_usb_max_pipe_num)) {
    return k_ra8_err_invalid_arg;
  }
  if ((*inout_len == 0U) || (*inout_len > k_ra8_usb_pipe_max_packet)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_queue_out()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] pipe_num See implementation.
 * @param[in] out_buf See implementation.
 * @param[in] inout_len See implementation.
 * @param[in] rearm See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_queue_out(ra8_usb_speed_t speed,
                            uint8_t         pipe_num,
                            uint8_t*        out_buf,
                            uint16_t*       inout_len,
                            bool            rearm)
{
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t arg_err = internal_check_queue_out_args(pipe_num, out_buf, inout_len);
  if (arg_err != k_ra8_ok) {
    return arg_err;
  }

  /* Fast path: BRDYSTS bit `pipe_num` is set by hardware when an OUT
   * packet has landed in this pipe's FIFO buffer. If it's clear, no
   * data is waiting -- return k_ra8_err_no_data WITHOUT entering the
   * 10ms FRDY spin.
   * HUM Ch 36.2.12 "BRDYSTS : BRDY Interrupt Status Register", p 1983. */
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  if ((reg->BRDYSTS & pipe_bit) == 0U) {
    *inout_len = 0U;
    return k_ra8_err_no_data;
  }

  /* W0C BRDYSTS for this pipe BEFORE draining (FSP pattern, mirrors
   * `r_usb_pinthandler_usbip0.c` order). Critical for PIPECFG.DBLB:
   * if bank B raises a fresh BRDY edge while we are still draining
   * bank A, the BRDYSTS bit will be re-set by hardware and survive a
   * future read; clearing AFTER the drain would wipe that edge and
   * lose every other packet. */
  reg->BRDYSTS = (uint16_t)(~pipe_bit);

  priv_select_cfifo(reg, pipe_num, false);
  const ra8_err_t ready = priv_wait_frdy(reg);
  RA8_RETURN_ON_ERROR(ready, s_tag, "queue_out: FRDY timeout");

  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979 */
  const uint16_t available = (uint16_t)(reg->CFIFOCTR & k_ra8_fifoctr_dtln);
  if (available == 0U) {
    /* Zero-length packet (ZLP). FSP releases the bank explicitly via
     * BCLR in this case (`r_usb_plibusbip.c` usb_pstd_read_data line
     * 700: "0 length packet -> Clear BVAL"). For a non-zero drain the
     * FIFO empties naturally as we read DTLN bytes; BCLR is NOT used
     * and would confuse the bank pointer in DBLB mode. */
    reg->CFIFOCTR = k_ra8_fifoctr_bclr;
    *inout_len    = 0U;
    return k_ra8_err_no_data;
  }
  const uint16_t take = (available < *inout_len) ? available : *inout_len;
  priv_fifo_read(reg, out_buf, take);
  *inout_len = take;
  /* No BCLR for non-zero drain (FSP semantics). The FIFO read above
   * drained `take` bytes; the remainder (if take < available) is lost
   * by design -- our caller passed a buffer large enough for one MPS
   * packet, which is what one bank holds. */
  if (rearm) {
    /* Re-arm PID=BUF; rearm == false callers own the arm/park decision. */
    priv_pipe_pid(reg, pipe_num, k_ra8_pid_buf);
  }
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_rearm_out_pipe()`.
 *
 * @details See the public header for the documented contract; this
 * definition implements it. The hardware-required sequence per HUM
 * Ch 36.2.13 (NRDYSTS, W0C) and Ch 36.2.27 (PIPECTR.PID) is:
 *   1. Ack NRDYSTS bit `pipe_num` by writing 0 to that bit (W0C: write
 *      `~pipe_bit` to clear only the target bit and preserve the rest).
 *   2. Force PID=BUF on the pipe so the controller ACKs the next OUT
 *      token from the host instead of NAK'ing it.
 *
 * @param[in] speed    See header.
 * @param[in] pipe_num See header.
 *
 * @return Result code.
 * @retval k_ra8_ok              Pipe re-armed.
 * @retval k_ra8_err_invalid_arg Argument out of range.
 *
 * @pre Speed maps to a real controller.
 * @pre Pipe 1..9.
 * @post NRDYSTS bit `pipe_num` cleared.
 * @post PIPECTR PID == BUF for `pipe_num`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_rearm_out_pipe(ra8_usb_speed_t speed, uint8_t pipe_num)
{
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((pipe_num == 0U) || (pipe_num > k_ra8_usb_max_pipe_num)) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 36.2.13 "NRDYSTS : NRDY Interrupt Status Register", p 1992.
   * W0C semantics: write 0 to clear the target bit, 1 to preserve the
   * rest. */
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  reg->NRDYSTS            = (uint16_t)(~pipe_bit);
  /* HUM Ch 36.2.27 "PIPEnCTR : PIPE n Control Register", p 2005. Force
   * PID=BUF so the next host OUT token is ACKed. */
  priv_pipe_pid(reg, pipe_num, k_ra8_pid_buf);
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_park_out_pipe()`.
 *
 * @details See the public header for the documented contract; this
 * definition implements it. Forces PIPECTR.PID = NAK so the controller
 * NAKs (rather than ACKs) host OUT tokens while the pipe has no
 * consumer, keeping BRDYSTS clear and the USB ISR quiescent.
 *
 * @param[in] speed    See header.
 * @param[in] pipe_num See header.
 *
 * @return Result code.
 * @retval k_ra8_ok              Pipe parked at PID=NAK.
 * @retval k_ra8_err_invalid_arg Argument out of range.
 *
 * @pre Speed maps to a real controller.
 * @pre Pipe 1..9.
 * @post PIPECTR PID == NAK for `pipe_num`.
 * @post The host's subsequent OUT tokens on this pipe are NAKed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_park_out_pipe(ra8_usb_speed_t speed, uint8_t pipe_num)
{
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((pipe_num == 0U) || (pipe_num > k_ra8_usb_max_pipe_num)) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 36.2.27 "PIPEnCTR : PIPE n Control Register" p 2005 */
  priv_pipe_pid(reg, pipe_num, k_ra8_pid_nak);
  return k_ra8_ok;
}

/* =============================================================================
 * Control transfers
 * =============================================================================
 */

/**
 * @brief Implementation of `ra8_usb_read_setup_if_valid()`.
 * @details VALID-gated SETUP drain. Returns ::k_ra8_err_no_data when
 *          INTSTS0.VALID is clear; otherwise drains USBREQ/USBVAL/
 *          USBINDX/USBLENG and W0C-clears VALID.
 * @param[in] speed See header.
 * @param[in] out_setup See header.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @retval k_ra8_err_no_data INTSTS0.VALID was clear at entry.
 * @retval k_ra8_err_invalid_arg speed out of range.
 * @retval k_ra8_err_null_ptr out_setup was NULL.
 * @pre Module state is consistent.
 * @pre out_setup is non-NULL.
 * @post On success, INTSTS0.VALID is W0C-cleared.
 * @post On success, *out_setup mirrors the chip's SETUP latch.
 * @note Not thread-safe; FS / CTRT path uses this variant.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_read_setup_if_valid(ra8_usb_speed_t speed, ra8_usb_setup_t* out_setup)
{
  RA8_CHECK_NULL_PTR(out_setup, s_tag, "read_setup_if_valid: out_setup");
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1986 */
  if ((reg->INTSTS0 & k_ra8_intsts0_mask_valid) == 0U) {
    return k_ra8_err_no_data;
  }

  /* HUM Ch 36.2.17 "USBREQ : USB Request Type Register", p 1995 */
  const uint16_t req         = reg->USBREQ;
  out_setup->bm_request_type = (uint8_t)(req & k_ra8_usb_byte_mask);
  out_setup->b_request       = (uint8_t)((req >> k_ra8_usb_byte_bits) & k_ra8_usb_byte_mask);
  out_setup->w_value         = reg->USBVAL;
  out_setup->w_index         = reg->USBINDX;
  out_setup->w_length        = reg->USBLENG;

  /* Clear VALID by writing zero to the bit (W0C semantics). */
  reg->INTSTS0 = (uint16_t)(reg->INTSTS0 & (uint16_t)~k_ra8_intsts0_mask_valid);
  return k_ra8_ok;
}

/**
 * @brief Implementation of ra8_usb_read_setup_unconditional (see header).
 * @details Race-free SETUP drain for the HS / SQMON polled-worker path.
 *          On HS the polled dispatcher routinely observes
 *          DCPCTR.SQMON == 1 (race-immune SETUP-latched signal, HUM
 *          Ch 37.2.31 p 2095) AFTER the SIE has already auto-cleared
 *          INTSTS0.VALID. The captured registers USBREQ/USBVAL/USBINDX/
 *          USBLENG remain latched (HUM Ch 37.2.21..24 p 2087..2090) --
 *          only the VALID flag is cleared. This entry point therefore
 *          skips the VALID gate and drains the captured registers
 *          directly, then defensively W0C-acks VALID in case the SIE
 *          re-asserted it before our store.
 * @param[in] speed Which controller.
 * @param[out] out_setup Decoded 8-byte SETUP packet.
 * @return Result code.
 * @retval k_ra8_ok SETUP drained from the captured registers.
 * @retval k_ra8_err_invalid_arg speed out of range.
 * @retval k_ra8_err_null_ptr out_setup was NULL.
 * @pre Caller has independent proof a SETUP arrived (e.g. SQMON==1).
 * @pre out_setup is non-NULL.
 * @post *out_setup mirrors USBREQ/USBVAL/USBINDX/USBLENG.
 * @post INTSTS0.VALID is W0C-cleared (no-op if already 0).
 * @note Not thread-safe; HS / SQMON path uses this variant.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_read_setup_unconditional(ra8_usb_speed_t speed, ra8_usb_setup_t* out_setup)
{
  RA8_CHECK_NULL_PTR(out_setup, s_tag, "read_setup_unconditional: out_setup");
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 36.2.17 / 37.2.21 "USBREQ : USB Request Type Register",
   * p 1989 / 2087. The SETUP latch survives VALID being auto-cleared
   * by the SIE; only a fresh SETUP token can overwrite it. */
  const uint16_t req         = reg->USBREQ;
  out_setup->bm_request_type = (uint8_t)(req & k_ra8_usb_byte_mask);
  out_setup->b_request       = (uint8_t)((req >> k_ra8_usb_byte_bits) & k_ra8_usb_byte_mask);
  out_setup->w_value         = reg->USBVAL;
  out_setup->w_index         = reg->USBINDX;
  out_setup->w_length        = reg->USBLENG;

  /* HUM Ch 36.2.14 INTSTS0 p 1985: defensively W0C-clear VALID in case
   * the SIE has re-asserted it (no-op when already 0). */
  reg->INTSTS0 = (uint16_t)(reg->INTSTS0 & (uint16_t)~k_ra8_intsts0_mask_valid);
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_control_response()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] accept See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_control_response(ra8_usb_speed_t speed, bool accept)
{
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!accept) {
    /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1999 */
    priv_dcp_pid(reg, k_ra8_pid_stall);
    return k_ra8_ok;
  }
  priv_dcp_pid(reg, k_ra8_pid_buf);
  /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1999 */
  priv_rmw16(&reg->DCPCTR, (uint16_t)(1U << k_ra8_dcpctr_bit_ccpl), 0U);
  return k_ra8_ok;
}
