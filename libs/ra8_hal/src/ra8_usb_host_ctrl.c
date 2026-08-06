/**
 * @file ra8_usb_host_ctrl.c
 * @brief USB host-mode control-transfer engine (polled, synchronous)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Host-mode control-transfer engine plus the device-address (DEVADDn)
 * programmer it shares with the bulk engine. Composes the shared FIFO /
 * PID helpers into a blocking GET / SET control transfer over the DCP
 * (pipe 0): SETUP stage, optional DATA-IN / DATA-OUT stage, and the
 * zero-length STATUS stage. Also hosts the non-blocking device-side
 * control-OUT DCP arm / read helpers (``ra8_usb_dcp_out_arm`` /
 * ``ra8_usb_dcp_out_read``). Split out of ``ra8_usb.c`` so every
 * translation unit stays under the 1000-line cap; the shared register
 * helpers and host addressing enums it uses are declared in
 * ``ra8_usb_internal.h``. Modelled on FSP ``r_usb_hreg_abs.c``; no FSP
 * source ships in this tree.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_usb.h"
#include "ra8_usb_internal.h"
#include "ra8_usb_regs.h"

static const char* s_tag = "USB";

/* =============================================================================
 * Host-mode control-transfer engine (polled, synchronous)
 *
 * The RA SIE signals host control-transfer progress through SUREQ-clear
 * (SETUP done), BRDY (a DATA packet landed in the DCP buffer) and BEMP
 * (the STATUS stage completed) -- NOT through CTRT, which is a device-mode
 * control-stage signal. This block composes the existing FIFO/PID helpers
 * into a blocking GET/SET control transfer over the DCP (pipe 0).
 * =============================================================================
 */

/** @brief Tunables for the polled host control-transfer engine. */
typedef enum : uint32_t {
  k_ra8_usb_ctrl_poll_limit = 2000000UL, /**< Spin bound per control stage. */
} ra8_usb_host_ctrl_lim_t;

/** @brief Stage codes recorded by the host control-transfer engine. */
typedef enum : uint8_t {
  k_ra8_usb_cs_begin     = 0U, /**< Before the SETUP stage.       */
  k_ra8_usb_cs_setup     = 1U, /**< SETUP stage completed.        */
  k_ra8_usb_cs_data_in   = 2U, /**< DATA-IN stage entered.        */
  k_ra8_usb_cs_data_pkt  = 3U, /**< First DATA packet received.   */
  k_ra8_usb_cs_data_done = 4U, /**< DATA stage completed.         */
  k_ra8_usb_cs_status    = 5U, /**< STATUS stage entered.         */
  k_ra8_usb_cs_done      = 6U, /**< STATUS done; transfer closed. */
  k_ra8_usb_cs_data_out  = 7U, /**< DATA-OUT stage entered.       */
} ra8_usb_host_ctrl_stage_code_t;

/** @brief Last host control-transfer stage reached (bring-up diagnostic). */
static volatile uint8_t s_host_ctrl_stage = (uint8_t)k_ra8_usb_cs_begin;

uint8_t ra8_usb_host_ctrl_stage(void)
{
  return s_host_ctrl_stage;
}
/**
 * @brief Program DEVADDn.USBSPD with the connected device's link speed.
 *
 * @details The RA host SIE will not run transactions to a device address
 * until that address's DEVADDn slot carries the link speed. The DEVADDn
 * registers sit past the modelled register window, so this addresses the
 * slot by raw offset (0xD0 + 2n) and copies DVSTCTR0.RHST (01=LS, 10=FS,
 * 11=HS) into USBSPD[7:6].
 *
 * @param[in] reg      Selected controller register block (its base address).
 * @param[in] dev_addr Address slot to program (0..::k_ra8_usb_dev_addr_max).
 * @pre The bus reset has completed so RHST reflects the device speed.
 * @pre @p reg is non-NULL and @p dev_addr is within the DEVADD range.
 * @post DEVADDn.USBSPD matches the connected speed; the address is usable.
 * @post No other DEVADDn field is changed (the rest are reserved/zero).
 * @note Single-device bring-up: no hub fields (UPPHUB/HUBPORT) are set.
 * @since 0.1.0
 */
void internal_host_program_devadd(volatile r_usb_regs_t* reg, uint8_t dev_addr)
{
  const uint16_t  rhst = (uint16_t)(reg->DVSTCTR0 & (uint16_t)k_ra8_usb_rhst_mask);
  const uintptr_t off =
    (uintptr_t)k_ra8_usb_devadd0_off + ((uintptr_t)dev_addr * (uintptr_t)k_ra8_usb_devadd_stride);
  volatile uint16_t* const devadd = (volatile uint16_t*)((uintptr_t)reg + off);
  *devadd                         = (uint16_t)(rhst << (uint16_t)k_ra8_usb_usbspd_shift);
}

/**
 * @brief Spin until a W0C status bit asserts or the deadline elapses.
 *
 * @details Bounded busy-wait over a USB interrupt-status register; the
 * polled host control engine uses it to gate on DCP BRDY/BEMP edges
 * without arming the NVIC USB line.
 *
 * @param[in] sts  Pointer to the status register (BRDYSTS / BEMPSTS).
 * @param[in] mask Bit to wait for.
 * @return k_ra8_ok on assertion, k_ra8_err_hw_timeout otherwise.
 * @retval k_ra8_ok             The masked bit asserted within the bound.
 * @retval k_ra8_err_hw_timeout The bit never asserted before the deadline.
 * @pre @p sts points at a live USB status register.
 * @pre Interrupts for this controller are quiescent (polled driver).
 * @post No register is modified.
 * @post On timeout the caller aborts the transfer.
 * @note Bounded busy-wait; FS control stages settle well inside the bound.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_host_wait_sts(volatile const uint16_t* sts, uint16_t mask)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_usb_ctrl_poll_limit; ++i) {
    if ((*sts & mask) != 0U) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Wait for a DCP IN packet (BRDY), re-arming on NRDY give-ups.
 *
 * @details In host mode the SIE retries a NAKed IN a few times, then
 * latches NRDY and parks the DCP PID at NAK. A device that is still
 * processing a request (e.g. applying SET_CONFIGURATION before its
 * status ZLP) NAKs long enough to trip this, so treat NRDY as "retry":
 * clear it and re-arm PID=BUF until BRDY or the time bound.
 *
 * @param[in] reg Selected controller register block.
 * @return Wait outcome.
 * @retval k_ra8_ok             A packet landed (BRDY).
 * @retval k_ra8_err_hw_timeout No packet before the spin bound.
 * @pre The DCP receive buffer is free and PID is BUF.
 * @pre BRDYENB/NRDYENB carry the DCP bit so the status can latch.
 * @post BRDY is left set for the caller to consume; NRDY is clear.
 * @post On timeout the DCP PID state is whatever the SIE parked.
 * @note Blocking; bounded by ::k_ra8_usb_ctrl_poll_limit.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_host_dcp_in_wait(volatile r_usb_regs_t* reg)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_usb_ctrl_poll_limit; ++i) {
    if ((reg->BRDYSTS & (uint16_t)k_ra8_usb_dcp_pipe0_bit) != 0U) {
      return k_ra8_ok;
    }
    if ((reg->DCPCTR & (uint16_t)k_ra8_usb_pid_stall_bit) != 0U) {
      return k_ra8_err_hw_error;
    }
    if ((reg->NRDYSTS & (uint16_t)k_ra8_usb_dcp_pipe0_bit) != 0U) {
      reg->NRDYSTS = (uint16_t) ~(uint16_t)k_ra8_usb_dcp_pipe0_bit;
      internal_dcp_pid(reg, k_ra8_pid_buf);
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Clear the SETUP outcome flags, launch SUREQ, and await SACK/SIGN.
 *
 * @details Split out of ::internal_host_ctrl_setup so the blocking wait keeps
 * that function inside one page. This W0C-clears the stale SACK/SIGN latches,
 * asserts SUREQ, and spins until the SIE latches SACK (device ACK), SIGN (three
 * failed attempts), or the bound elapses. The host unit-test build runs this
 * SAME bounded poll loop: only the pre-loop W0C clear is skipped there (its
 * plain-RAM INTSTS1 cannot be re-latched after a clear, so wiping it would erase
 * a test's pre-loaded outcome), and each SACK poll is routed through the
 * ra8_fake_mmio host seam -- transparent when a test pre-loads INTSTS1, or armable
 * to force the timeout leg or step the loop for MC/DC. The SACK, SIGN, and
 * timeout legs thus each execute against the real code path in host unit tests.
 *
 * @param[in] reg Selected controller register block.
 * @return SETUP wait outcome.
 * @retval k_ra8_ok             SACK latched -- the device ACKed the SETUP.
 * @retval k_ra8_err_hw_error   SIGN latched -- three transmission attempts failed.
 * @retval k_ra8_err_hw_timeout No SACK/SIGN before the spin bound.
 * @pre @p reg points at a live controller block with SUREQ armable.
 * @pre INTENB1 already carries the SACK/SIGN enables.
 * @post On success SACK is cleared; on error SIGN is cleared.
 * @post SUREQ has been asserted exactly once.
 * @note Blocking; bounded by ::k_ra8_usb_ctrl_poll_limit.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_host_setup_wait(volatile r_usb_regs_t* reg)
{
  const uint16_t sack  = (uint16_t)(1U << k_ra8_int1_bit_sack);
  const uint16_t sign  = (uint16_t)(1U << k_ra8_int1_bit_sign);
  const uint16_t sureq = (uint16_t)(1U << k_ra8_dcpctr_bit_sureq);
#if !(defined(RA8_OFF_TARGET) && defined(UNIT_TEST))
  /* Clear the stale SACK/SIGN latches before asserting SUREQ. Skipped on the
   * host unit-test build: its plain-RAM INTSTS1 cannot be re-latched by a SIE
   * after a W0C clear, so wiping it here would erase the SACK/SIGN outcome a
   * test pre-loaded and the poll below would never observe it. */
  reg->INTSTS1 = (uint16_t) ~(uint16_t)(sack | sign);
#endif
  internal_rmw16(&reg->DCPCTR, sureq, 0U);
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_usb_ctrl_poll_limit; ++i) {
    const uint16_t sts1 = reg->INTSTS1;
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    const bool got_sack = ra8_fake_mmio_wait_eval(&reg->INTSTS1, i, ((sts1 & sack) != 0U));
#else
    const bool got_sack = ((sts1 & sack) != 0U);
#endif
    if (got_sack) {
      reg->INTSTS1 = (uint16_t)~sack;
      return k_ra8_ok;
    }
    if ((sts1 & sign) != 0U) {
      reg->INTSTS1 = (uint16_t)~sign;
      return k_ra8_err_hw_error;
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Issue the SETUP stage of a host control transfer and wait for it.
 *
 * @details Parks the DCP NAK, programs the target device speed, loads the
 * USBREQ/VAL/INDX/LENG mirror registers, asserts SUREQ and spins until the
 * SIE self-clears it (the SETUP token has been delivered + handshaked).
 *
 * @param[in] reg   Selected controller register block.
 * @param[in] setup Setup packet to transmit.
 * @return k_ra8_ok once SUREQ clears, else k_ra8_err_busy / k_ra8_err_hw_timeout.
 * @retval k_ra8_ok             SETUP delivered and SUREQ cleared.
 * @retval k_ra8_err_busy       A control transfer was already pending.
 * @retval k_ra8_err_hw_timeout SUREQ did not clear before the deadline.
 * @pre @p reg / @p setup are non-NULL; the bus is reset and UACT is on.
 * @pre No control transfer is already pending (SUREQ clear).
 * @post The 8-byte SETUP token has been delivered to the device.
 * @post Stale DCP BRDY/BEMP status has been cleared.
 * @note Blocking; bounded by ::k_ra8_usb_ctrl_poll_limit.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_host_ctrl_setup(volatile r_usb_regs_t* reg, const ra8_usb_setup_t* setup)
{
  const uint16_t sureq = (uint16_t)(1U << k_ra8_dcpctr_bit_sureq);
  if ((reg->DCPCTR & sureq) != 0U) {
    /* A SETUP issued with the port inactive (e.g. before any device was
     * attached) leaves SUREQ wedged. The USBHS instance provides
     * SUREQCLR to abort it; try that before reporting busy. */
    if (internal_is_hs(reg)) {
      internal_rmw16(&reg->DCPCTR, (uint16_t)(1U << k_ra8_dcpctr_bit_sureqclr), 0U);
    }
    if ((reg->DCPCTR & sureq) != 0U) {
      return k_ra8_err_busy;
    }
  }
  internal_dcp_pid(reg, k_ra8_pid_nak);
  /* Clear any stale CCPL left by a prior transfer's status stage. With
   * CCPL still set, arming PID=BUF for the new DATA stage makes the SIE
   * run a status stage instead, so the data stage never moves (observed
   * as stage=2 with DCPCTR=0x0004 on hardware). */
  internal_rmw16(&reg->DCPCTR, 0U, (uint16_t)(1U << k_ra8_dcpctr_bit_ccpl));
  /* Refresh the DEVADD slot of the address the DCP currently targets
   * (DCPMAXP.DEVSEL); 0 before SET_ADDRESS, the assigned address after
   * ra8_usb_host_set_target. */
  const uint8_t devsel = (uint8_t)((uint16_t)(reg->DCPMAXP >> (uint16_t)k_ra8_usb_devsel_shift) &
                                   (uint16_t)k_ra8_usb_devsel_field_mask);
  internal_host_program_devadd(reg, devsel);
#ifdef RA8_OFF_TARGET
  /* BEMPSTS/BRDYSTS are W1C: on hardware, writing ~DCP clears every set status
   * bit EXCEPT the DCP pipe. A plain-RAM fake would blind-assign 0xFFFE and zero
   * the DCP bit a test pre-loaded for the DATA stage, so model the W1C (preserve
   * DCP, clear the rest) explicitly off-target. */
  reg->BEMPSTS = (uint16_t)(reg->BEMPSTS & (uint16_t)k_ra8_usb_dcp_pipe0_bit);
  reg->BRDYSTS = (uint16_t)(reg->BRDYSTS & (uint16_t)k_ra8_usb_dcp_pipe0_bit);
#else
  reg->BEMPSTS = (uint16_t) ~(uint16_t)k_ra8_usb_dcp_pipe0_bit;
  reg->BRDYSTS = (uint16_t) ~(uint16_t)k_ra8_usb_dcp_pipe0_bit;
#endif
  const uint16_t req = (uint16_t)((uint16_t)setup->bm_request_type |
                                  (uint16_t)((uint16_t)setup->b_request << k_ra8_usb_byte_bits));
  reg->USBREQ        = req;
  reg->USBVAL        = setup->w_value;
  reg->USBINDX       = setup->w_index;
  reg->USBLENG       = setup->w_length;
  /* Arm + clear the SETUP outcome flags: SACK latches when the device
   * ACKs the SETUP, SIGN when three transmission attempts fail. SUREQ
   * self-clearing alone is NOT an ACK indication, so gate on these. */
  const uint16_t sack = (uint16_t)(1U << k_ra8_int1_bit_sack);
  const uint16_t sign = (uint16_t)(1U << k_ra8_int1_bit_sign);
  reg->INTENB1        = (uint16_t)(reg->INTENB1 | (uint16_t)(sack | sign));
  return internal_host_setup_wait(reg);
}

/**
 * @brief Arm the DCP to receive a control-read data stage.
 *
 * @details Selects the DCP read window, frees the receive buffer (BCLR),
 * enables the BRDY/NRDY status latches, forces the receive toggle to
 * DATA1 (FSP usb_hstd_ctrl_read_start: the first data packet after SETUP
 * is always DATA1, and a stale SQMON makes the SIE silently discard it),
 * then sets PID=BUF so IN tokens flow.
 *
 * @param[in] reg Selected controller register block.
 * @pre The SETUP stage for an IN request has completed (SACK).
 * @pre The DCP PID is NAK on entry.
 * @post The DCP is armed (PID=BUF) with a clean receive buffer.
 * @post BRDYENB/NRDYENB carry the DCP bit.
 * @note Helper split out for the clang-tidy size/complexity gate.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_host_ctrl_data_arm(volatile r_usb_regs_t* reg)
{
  internal_select_cfifo(reg, 0U, false);
  reg->CFIFOCTR = (uint16_t)k_ra8_fifoctr_bclr;
  reg->BRDYENB  = (uint16_t)(reg->BRDYENB | (uint16_t)k_ra8_usb_dcp_pipe0_bit);
  reg->NRDYENB  = (uint16_t)(reg->NRDYENB | (uint16_t)k_ra8_usb_dcp_pipe0_bit);
  internal_rmw16(&reg->DCPCTR, (uint16_t)(1U << k_ra8_dcpctr_bit_sqset), 0U);
  internal_dcp_pid(reg, k_ra8_pid_buf);
}

/**
 * @brief Run the DATA-IN stage of a host control read into @p data.
 *
 * @details Arms the DCP for IN (PID=BUF, BRDY/NRDY status enabled), then
 * loops reading BRDY-gated CFIFO packets via ::internal_fifo_read until a
 * short packet or @p want bytes, parking the pipe NAK on exit.
 *
 * @param[in]  reg     Selected controller register block.
 * @param[out] data    Destination buffer.
 * @param[in]  want    Bytes the caller can accept.
 * @param[in]  mxps    DCP max packet size (short-packet threshold).
 * @param[out] out_rx  Receives the byte count read from the device.
 * @return k_ra8_ok on a complete/short-packet read, else a timeout code.
 * @retval k_ra8_ok             A short packet or @p want bytes were read.
 * @retval k_ra8_err_hw_timeout No DATA packet arrived before the deadline.
 * @pre The SETUP stage for an IN request has completed.
 * @pre @p data holds at least @p want bytes.
 * @post @p out_rx holds the number of bytes the device returned.
 * @post The DCP PID is left NAK.
 * @note Blocking; each packet is bounded by the poll limit.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_host_ctrl_data_in(volatile r_usb_regs_t* reg,
                                            uint8_t*               data,
                                            uint16_t               want,
                                            uint16_t               mxps,
                                            uint16_t*              out_rx)
{
  uint16_t rx   = 0U;
  bool     done = false;
  internal_host_ctrl_data_arm(reg);
  while (!done) {
    const ra8_err_t br = internal_host_dcp_in_wait(reg);
    if (br != k_ra8_ok) {
      internal_dcp_pid(reg, k_ra8_pid_nak);
      return br;
    }
    s_host_ctrl_stage = (uint8_t)k_ra8_usb_cs_data_pkt;
    internal_select_cfifo(reg, 0U, false);
    if (internal_wait_frdy(reg) != k_ra8_ok) {
      internal_dcp_pid(reg, k_ra8_pid_nak);
      return k_ra8_err_hw_timeout;
    }
    const uint16_t dtln  = (uint16_t)(reg->CFIFOCTR & k_ra8_fifoctr_dtln);
    uint16_t       chunk = dtln;
    if ((uint16_t)(rx + chunk) > want) {
      chunk = (uint16_t)(want - rx);
    }
    if (chunk > 0U) {
      internal_fifo_read(reg, &data[rx], chunk);
    }
    reg->CFIFOCTR = (uint16_t)k_ra8_fifoctr_bclr;
    rx            = (uint16_t)(rx + dtln);
    reg->BRDYSTS  = (uint16_t) ~(uint16_t)k_ra8_usb_dcp_pipe0_bit;
    if (dtln < mxps) {
      done = true;
    }
    if (rx >= want) {
      done = true;
    }
  }
  internal_dcp_pid(reg, k_ra8_pid_nak);
  *out_rx = rx;
  return k_ra8_ok;
}

/**
 * @brief Run the zero-length STATUS stage and complete the control transfer.
 *
 * @details Parks the DCP NAK, drains residual data-stage bytes, forces the
 * DATA1 sequence bit, then asserts CCPL + PID=BUF so the SIE runs the
 * opposite-direction zero-length status stage; completion is taken from
 * CCPL self-clearing.
 *
 * @param[in] reg       Selected controller register block.
 * @param[in] write_zlp true => status is OUT (after a data-IN read); false =>
 *                      status is IN (control write / no-data).
 * @return Always k_ra8_ok (the status stage is best-effort).
 * @retval k_ra8_ok The status sequence was driven; the wire-level handshake
 *                 is verified by the next transfer / the class protocol.
 * @pre The SETUP (and any DATA) stage has completed.
 * @pre The DCP PID is NAK on entry.
 * @post CCPL has been asserted so the SIE closes the control transfer.
 * @post The DCP PID is NAK and CCPL is cleared on exit.
 * @note Best-effort by design: hardware bring-up showed the device
 *       completes the status handshake (the SIE auto-ACKs its ZLP) while
 *       the DCP BRDY/BEMP completion indication is unreliable, so gating
 *       on it deadlocks transfers that actually succeeded. Real
 *       verification comes from the next SETUP (control) or the
 *       BOT/CSW exchange (bulk).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_host_ctrl_status(volatile r_usb_regs_t* reg, bool write_zlp)
{
  const uint16_t ccpl = (uint16_t)(1U << k_ra8_dcpctr_bit_ccpl);
  reg->BEMPSTS        = (uint16_t) ~(uint16_t)k_ra8_usb_dcp_pipe0_bit;
  reg->BRDYSTS        = (uint16_t) ~(uint16_t)k_ra8_usb_dcp_pipe0_bit;
  reg->BEMPENB        = (uint16_t)(reg->BEMPENB | (uint16_t)k_ra8_usb_dcp_pipe0_bit);
  reg->BRDYENB        = (uint16_t)(reg->BRDYENB | (uint16_t)k_ra8_usb_dcp_pipe0_bit);
  /* A control-WRITE data stage left DCPCFG.DIR = 1 (OUT tokens); its IN-ZLP
   * status stage needs DIR = 0 so the host issues an IN token to collect the
   * device's status ZLP. Restore it for the write / no-data status (the
   * control-READ OUT-ZLP status keeps the default DIR and is best-effort).
   * HUM Ch 36.2.18 "DCPCFG : DCP Configuration" p 1989. */
  if (!write_zlp) {
    internal_rmw16(&reg->DCPCFG, 0U, (uint16_t)(1U << (uint8_t)k_ra8_dcpcfg_bit_dir));
  }
  /* The status stage is always DATA1; force the DCP sequence bit so the
   * device does not NAK a toggle mismatch (seen as NRDYSTS bit 0). */
  internal_rmw16(&reg->DCPCTR, (uint16_t)(1U << k_ra8_dcpctr_bit_sqset), 0U);
  internal_select_cfifo(reg, 0U, write_zlp);
  if (write_zlp) {
    /* Control-read status is an OUT the host must drive: enqueue a
     * zero-length packet (BVAL, no CFIFO write) and assert CCPL. This is
     * best-effort -- the DATA-IN payload the caller wanted is already in
     * hand, and a fresh SETUP re-syncs the control endpoint regardless, so
     * a slow/NAKed OUT status does not invalidate the transfer. */
    reg->CFIFOCTR = (uint16_t)k_ra8_fifoctr_bval;
    internal_dcp_pid(reg, k_ra8_pid_buf);
    internal_rmw16(&reg->DCPCTR, ccpl, 0U);
    (void)internal_host_wait_sts(&reg->BEMPSTS, k_ra8_usb_dcp_pipe0_bit);
    internal_dcp_pid(reg, k_ra8_pid_nak);
    internal_rmw16(&reg->DCPCTR, 0U, ccpl);
    return k_ra8_ok;
  }
  /* Control-write / no-data status is an IN: free the DCP receive buffer
   * FIRST -- a stale BSTS blocks the incoming ZLP, the SIE NAKs it
   * forever, and the request never takes effect on the device (observed
   * on hardware as SET_CONFIGURATION "passing" while the bulk endpoints
   * stayed dead). Then arm PID=BUF + CCPL and require the BRDY that
   * marks the device's status ZLP received. */
  reg->CFIFOCTR = (uint16_t)k_ra8_fifoctr_bclr;
  reg->NRDYSTS  = (uint16_t) ~(uint16_t)k_ra8_usb_dcp_pipe0_bit;
  internal_dcp_pid(reg, k_ra8_pid_buf);
  internal_rmw16(&reg->DCPCTR, ccpl, 0U);
  const ra8_err_t ierr = internal_host_dcp_in_wait(reg);
  internal_dcp_pid(reg, k_ra8_pid_nak);
  internal_rmw16(&reg->DCPCTR, 0U, ccpl);
  /* Drain the status ZLP so it cannot alias the next data stage. */
  reg->CFIFOCTR = (uint16_t)k_ra8_fifoctr_bclr;
  reg->BRDYSTS  = (uint16_t) ~(uint16_t)k_ra8_usb_dcp_pipe0_bit;
  return ierr;
}

/**
 * @brief Run the DATA-OUT stage of a host control write from @p data.
 *
 * @details The host-side mirror of ::internal_host_ctrl_data_in: selects the
 * DCP write window, discards any stale bank, forces the DATA1 start toggle
 * (the first data packet after a SETUP is always DATA1), then pushes the
 * payload to the device in MPS-sized CFIFO packets via ::internal_dcp_push_chunk
 * (BVAL) + PID=BUF, waiting for the buffer-empty (BEMP) event after each, and
 * parks the DCP NAK on exit. This is what lets a class control write carry a
 * data stage host -> device -- e.g. a DFU_DNLOAD firmware block.
 *
 * @param[in] reg  Selected controller register block.
 * @param[in] data Source payload (host -> device).
 * @param[in] want Bytes to send (the SETUP wLength, clamped to the buffer).
 * @param[in] mxps DCP max packet size (per-packet chunk bound).
 * @return k_ra8_ok when the whole payload was transmitted, else a timeout code.
 * @retval k_ra8_ok             All @p want bytes were sent and the buffer drained.
 * @retval k_ra8_err_hw_timeout A packet never drained (BEMP) before the deadline.
 * @pre The SETUP stage for an OUT request with wLength > 0 completed (SACK).
 * @pre @p data holds at least @p want bytes; the DCP PID is NAK on entry.
 * @post The whole payload was driven to the device; the DCP PID is left NAK.
 * @post BEMPENB carries the DCP bit; BEMPSTS is cleared.
 * @note Blocking; each packet is bounded by the control poll limit.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_host_ctrl_data_out(volatile r_usb_regs_t* reg,
                                             const uint8_t*         data,
                                             uint16_t               want,
                                             uint16_t               mxps)
{
  /* Host DCP issues IN tokens by default (DCPCFG.DIR = 0, which serves the
   * common control-READ data stage). A control-WRITE data stage must flip
   * DIR = 1 so the controller issues OUT tokens; otherwise the device sees an
   * IN token in its write-data stage and flags CTSQ = SQER, dropping the
   * payload (CFIFOSEL.ISEL only sets the CPU FIFO-access direction, not the
   * wire token direction). HUM Ch 36.2.18 "DCPCFG : DCP Configuration" p 1989. */
  internal_rmw16(&reg->DCPCFG, (uint16_t)(1U << (uint8_t)k_ra8_dcpcfg_bit_dir), 0U);
  internal_select_cfifo(reg, 0U, true);
  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979 -- discard
   * any stale bank so FRDY re-asserts for the fresh write. */
  reg->CFIFOCTR = (uint16_t)k_ra8_fifoctr_bclr;
  /* First data packet after SETUP is DATA1; force the DCP sequence bit so the
   * device does not drop it on a toggle mismatch. */
  internal_rmw16(&reg->DCPCTR, (uint16_t)(1U << k_ra8_dcpctr_bit_sqset), 0U);
  reg->BEMPSTS = (uint16_t) ~(uint16_t)k_ra8_usb_dcp_pipe0_bit;
  reg->BEMPENB = (uint16_t)(reg->BEMPENB | (uint16_t)k_ra8_usb_dcp_pipe0_bit);

  const uint16_t step = (mxps == 0U) ? (uint16_t)1U : mxps;
  uint16_t       off  = 0U;
  while (off < want) {
    uint16_t chunk = (uint16_t)(want - off);
    if (chunk > step) {
      chunk = step;
    }
    const ra8_err_t perr = internal_dcp_push_chunk(reg, &data[off], chunk);
    if (perr != k_ra8_ok) {
      internal_dcp_pid(reg, k_ra8_pid_nak);
      return perr;
    }
    internal_dcp_pid(reg, k_ra8_pid_buf);
    /* Best-effort drain wait. The DCP BEMP completion indication is unreliable
     * on this silicon (see ::internal_host_ctrl_status) -- gating hard on it
     * deadlocks an OUT that actually landed. Give the bank a bounded window to
     * drain (back-pressure for multi-packet) but proceed regardless; the IN
     * status stage + the class protocol (DFU_GETSTATUS) confirm delivery. */
    (void)internal_host_wait_sts(&reg->BEMPSTS, (uint16_t)k_ra8_usb_dcp_pipe0_bit);
    reg->BEMPSTS = (uint16_t) ~(uint16_t)k_ra8_usb_dcp_pipe0_bit;
    off          = (uint16_t)(off + chunk);
  }
  internal_dcp_pid(reg, k_ra8_pid_nak);
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_dcp_out_arm()` -- arm the DCP for control-OUT.
 * @details Prepares the DCP (EP0) to receive the data stage of a host-to-device
 *          control transfer (e.g. a DFU_DNLOAD firmware block) WITHOUT blocking:
 *          clears any stale DCP BRDY latch, enables the DCP BRDY interrupt so the
 *          host's OUT packet raises a fresh USB IRQ, and sets PID=BUF so the SIE
 *          ACKs the OUT token (until then the host NAK-retries the data packet).
 *          The matching `ra8_usb_dcp_out_read()` drains the bank from the BRDY ISR.
 *          Splitting arm from read is mandatory on the self-loop: the FS device
 *          ISR and the HS host worker share one CPU, so a blocking receive in the
 *          SETUP ISR would spin out the very thread that must SEND the data.
 * @param[in] speed Controller (FS/HS) the control transfer is on.
 * @return k_ra8_ok when the DCP is armed, else an arg/HW code.
 * @retval k_ra8_ok              DCP armed; BRDY enabled and PID=BUF.
 * @retval k_ra8_err_invalid_arg @p speed selects no controller.
 * @retval k_ra8_err_hw_timeout  BRDYENB read-back did not latch the DCP bit.
 * @pre A SETUP for an OUT request with wLength > 0 has just been decoded.
 * @pre INTSTS0.VALID has been cleared (the PID write gate is open).
 * @post DCP BRDY is enabled and PID=BUF; the host's OUT data is ACKed on arrival.
 * @note Non-blocking; ISR-safe. Device-side only.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_dcp_out_arm(ra8_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* Receiving the data stage of a control-WRITE on the DCP. The first data
   * packet after a SETUP is ALWAYS DATA1 (USB 2.0 sec 8.6); a stale DCP
   * sequence toggle makes the SIE silently ACK-and-discard the host's packet
   * with NO BRDY -- the exact failure the host read-arm guards against in
   * ::internal_host_ctrl_data_arm. So clear the stale BRDY latch, select the
   * DCP read window, clear the receive bank (BCLR), enable the DCP BRDY
   * interrupt, force the receive toggle to DATA1 via DCPCTR.SQSET, and set
   * PID=BUF so the SIE ACKs the OUT token.
   * HUM Ch 36.2.21 "DCPCTR" p 1991 (SQSET) / Ch 36.2.11 "BRDYENB" p 1982 /
   * Ch 36.2.8 "CFIFOCTR" p 1979 (BCLR). */
  internal_dcp_pid(reg, k_ra8_pid_nak);
  /* Clear any stale CCPL left by the previous transfer's status stage. With
   * CCPL still set, arming PID=BUF makes the SIE run a status stage instead of
   * accepting the data stage -- CTSQ goes to SQER and the OUT data never lands.
   * Device-side mirror of the host guard in ::internal_host_ctrl_setup.
   * HUM Ch 36.2.21 "DCPCTR : DCP Control Register" p 1991 (CCPL). */
  internal_rmw16(&reg->DCPCTR, 0U, (uint16_t)(1U << (uint8_t)k_ra8_dcpctr_bit_ccpl));
  reg->BRDYSTS = (uint16_t) ~(uint16_t)k_ra8_usb_dcp_pipe0_bit;
  internal_select_cfifo(reg, 0U, false);
  reg->CFIFOCTR = (uint16_t)k_ra8_fifoctr_bclr;
  reg->BRDYENB  = (uint16_t)(reg->BRDYENB | (uint16_t)k_ra8_usb_dcp_pipe0_bit);
  internal_rmw16(&reg->DCPCTR, (uint16_t)(1U << (uint8_t)k_ra8_dcpctr_bit_sqset), 0U);
  internal_dcp_pid(reg, k_ra8_pid_buf);
  if ((reg->BRDYENB & (uint16_t)k_ra8_usb_dcp_pipe0_bit) == 0U) {
    /* The line above OR-set the DCP bit into BRDYENB and no helper touches
     * BRDYENB before this re-read of the same word, so under RA8_OFF_TARGET
     * (plain-RAM registers) the bit is always present; this timeout is only
     * reachable on silicon where the SIE refuses the enable. */
    return k_ra8_err_hw_timeout; /* GCOVR_EXCL_LINE */
  }
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_dcp_out_read()` -- drain an armed control-OUT.
 * @details Drains one buffer-bank's worth of control-OUT data from the DCP (EP0)
 *          after `ra8_usb_dcp_out_arm()` armed it and the host's OUT packet landed
 *          (DCP BRDY asserted). Disables the one-shot DCP BRDY, W0C-clears the
 *          latch, drains via the CFIFO, and parks the DCP NAK. Does NOT re-arm
 *          (unlike the old blocking variant): the caller is the BRDY ISR, so the
 *          bank is already full. Single bank (one packet up to the DCP MPS); the
 *          control-write status stage is driven separately via the CCPL pulse.
 * @param[in]  speed  Controller (FS/HS) the transfer is on.
 * @param[out] buf    Destination for the received bytes.
 * @param[in]  cap    Capacity of @p buf in bytes.
 * @param[out] out_rx Receives the byte count the host sent (DTLN).
 * @return k_ra8_ok on a drained packet (incl. a ZLP), else a no-data/timeout code.
 * @retval k_ra8_ok              A packet (possibly zero-length) was drained.
 * @retval k_ra8_err_invalid_arg @p speed invalid or @p buf / @p out_rx NULL.
 * @retval k_ra8_err_no_data     DCP BRDY is not set; the OUT packet has not landed.
 * @retval k_ra8_err_hw_timeout  CFIFO never reported FRDY for the DCP bank.
 * @pre `ra8_usb_dcp_out_arm()` armed the DCP for this transfer.
 * @pre Caller observed the DCP BRDY interrupt (or polls it via the no-data return).
 * @post @p out_rx holds the host's packet length; @p buf holds min(DTLN, cap).
 * @post DCP BRDY is disabled + cleared and the DCP PID is left NAK.
 * @note Non-blocking past a bounded CFIFO FRDY wait; ISR-safe. Device-side only.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_dcp_out_read(ra8_usb_speed_t speed, uint8_t* buf, uint16_t cap, uint16_t* out_rx)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((buf == nullptr) || (out_rx == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  *out_rx = 0U;

  /* The BRDY ISR only calls this once the DCP BRDY latch is set; if it is
   * not, the OUT packet has not landed -- report no-data so the caller can
   * retry on the next IRQ rather than draining a stale or empty bank. */
  if ((reg->BRDYSTS & (uint16_t)k_ra8_usb_dcp_pipe0_bit) == 0U) {
    return k_ra8_err_no_data;
  }
  /* Disable the one-shot DCP BRDY and W0C-clear the latch before draining
   * (FSP order; protects DBLB bank edges and prevents an ISR re-storm).
   * HUM Ch 36.2.11 "BRDYENB" p 1982 / Ch 36.2.13 "BRDYSTS" p 1984. */
  reg->BRDYENB = (uint16_t)(reg->BRDYENB & (uint16_t) ~(uint16_t)k_ra8_usb_dcp_pipe0_bit);
  reg->BRDYSTS = (uint16_t) ~(uint16_t)k_ra8_usb_dcp_pipe0_bit;

  internal_select_cfifo(reg, 0U, false);
  if (internal_wait_frdy(reg) != k_ra8_ok) {
    internal_dcp_pid(reg, k_ra8_pid_nak);
    return k_ra8_err_hw_timeout;
  }
  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979 */
  const uint16_t dtln = (uint16_t)(reg->CFIFOCTR & k_ra8_fifoctr_dtln);
  if (dtln == 0U) {
    reg->CFIFOCTR = (uint16_t)k_ra8_fifoctr_bclr;
  } else {
    const uint16_t take = (dtln < cap) ? dtln : cap;
    internal_fifo_read(reg, buf, take);
  }
  *out_rx = dtln;
  internal_dcp_pid(reg, k_ra8_pid_nak);
  return k_ra8_ok;
}

/**
 * @brief Classify and execute the optional DATA stage of a host control transfer.
 *
 * @details Inspects the SETUP packet to determine the data-stage direction:
 * a non-zero wLength with bmRequestType bit 7 set (device-to-host) and a
 * non-NULL destination buffer indicates a control-read DATA-IN stage; a
 * non-zero wLength with bit 7 clear and a non-NULL source buffer indicates a
 * control-write DATA-OUT stage; otherwise there is no data stage. For each
 * case the function:
 *   1. Resolves the DCP max packet size from DCPMAXP.MXPS.
 *   2. Clamps the transfer size to min(setup->w_length, data_len).
 *   3. Dispatches to ::internal_host_ctrl_data_out (DATA-OUT) or
 *      ::internal_host_ctrl_data_in (DATA-IN) as appropriate, updating
 *      ::s_host_ctrl_stage before and after each.
 *   4. Records zero bytes received and out_is_read = false when no data
 *      stage runs (no-data control transfer), and returns k_ra8_ok.
 *
 * @param[in]  reg         Selected USB controller register block; must have
 *                         been returned by ::internal_pick (non-NULL).
 * @param[in]  setup       SETUP packet just delivered to the device; used for
 *                         bmRequestType direction bit and wLength.
 * @param[in,out] data     Buffer for the data stage: source bytes for DATA-OUT,
 *                         destination bytes for DATA-IN; may be NULL only when
 *                         there is no data stage (w_length == 0 or no buffer).
 * @param[in]  data_len    Capacity of @p data in bytes; clamps the transfer.
 * @param[out] out_rx      Receives the byte count actually read from the device
 *                         (DATA-IN only); set to 0 for DATA-OUT or no-data.
 * @param[out] out_is_read Set to true when a DATA-IN stage ran; false otherwise.
 *
 * @return ra8_err_t Transfer outcome.
 * @retval k_ra8_ok             No data stage ran, or the DATA stage completed.
 * @retval k_ra8_err_hw_timeout A DATA-IN or DATA-OUT packet stalled before the
 *                             deadline elapsed.
 * @retval k_ra8_err_hw_error   The device STALLed the endpoint during DATA-OUT.
 *
 * @pre @p reg is non-NULL and points at a powered, host-mode controller.
 * @pre The SETUP stage for @p setup has already completed (SUREQ self-cleared).
 * @pre @p out_rx and @p out_is_read are non-NULL output pointers.
 * @post @p out_rx holds the DATA-IN byte count (0 if no IN stage ran).
 * @post @p out_is_read reflects whether a DATA-IN stage ran.
 *
 * @note Not thread-safe; caller (::ra8_usb_host_control_xfer) serialises access.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_host_data_phase(volatile r_usb_regs_t* reg,
                                          const ra8_usb_setup_t* setup,
                                          uint8_t*               data,
                                          uint16_t               data_len,
                                          uint16_t*              out_rx,
                                          bool*                  out_is_read)
{
  *out_rx      = 0U;
  bool is_read = false;
  if (setup->w_length > 0U) {
    if ((setup->bm_request_type & (uint16_t)k_ra8_usb_setup_dir_in) != 0U) {
      if (data != nullptr) {
        is_read = true;
      }
    }
  }
  *out_is_read        = is_read;
  const uint16_t mxps = (uint16_t)(reg->DCPMAXP & (uint16_t)k_ra8_usb_dcpmaxp_mxps);
  uint16_t       want = setup->w_length;
  if (data_len < want) {
    want = data_len;
  }
  /* Control-OUT with a data stage (e.g. DFU_DNLOAD): drive the payload to the
   * device; the opposite-direction (IN) zero-length status stage follows. */
  if ((setup->w_length > 0U) && !is_read && (data != nullptr)) {
    s_host_ctrl_stage    = (uint8_t)k_ra8_usb_cs_data_out;
    const ra8_err_t oerr = internal_host_ctrl_data_out(reg, data, want, mxps);
    if (oerr == k_ra8_ok) {
      s_host_ctrl_stage = (uint8_t)k_ra8_usb_cs_data_done;
    }
    return oerr;
  }
  if (!is_read) {
    return k_ra8_ok;
  }
  s_host_ctrl_stage    = (uint8_t)k_ra8_usb_cs_data_in;
  const ra8_err_t derr = internal_host_ctrl_data_in(reg, data, want, mxps, out_rx);
  if (derr == k_ra8_ok) {
    s_host_ctrl_stage = (uint8_t)k_ra8_usb_cs_data_done;
  }
  return derr;
}

ra8_err_t ra8_usb_host_control_xfer(ra8_usb_speed_t        speed,
                                    const ra8_usb_setup_t* setup,
                                    uint8_t*               data,
                                    uint16_t               data_len,
                                    uint16_t*              out_received)
{
  RA8_CHECK_NULL_PTR(setup, s_tag, "host_control_xfer: setup");
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  s_host_ctrl_stage    = (uint8_t)k_ra8_usb_cs_begin;
  const ra8_err_t serr = internal_host_ctrl_setup(reg, setup);
  if (serr != k_ra8_ok) {
    return serr;
  }
  s_host_ctrl_stage       = (uint8_t)k_ra8_usb_cs_setup;
  uint16_t        rx      = 0U;
  bool            is_read = false;
  const ra8_err_t derr    = internal_host_data_phase(reg, setup, data, data_len, &rx, &is_read);
  if (derr != k_ra8_ok) {
    return derr;
  }
  if (out_received != nullptr) {
    *out_received = rx;
  }
  s_host_ctrl_stage     = (uint8_t)k_ra8_usb_cs_status;
  const ra8_err_t sterr = internal_host_ctrl_status(reg, is_read);
  if (sterr == k_ra8_ok) {
    s_host_ctrl_stage = (uint8_t)k_ra8_usb_cs_done;
  }
  return sterr;
}
