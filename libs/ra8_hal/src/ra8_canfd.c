/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_canfd.c
 * @brief CAN with Flexible Data-rate driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 CANFD block. Mirrors the FSP `r_canfd.c`
 * Open / Close / Write / Read / ModeTransition / InfoGet flow:
 *
 *  - Cancel global sleep, wait for `CFDGSTS.GRSTSTS`/`GRAMINIT`,
 *    program `CFDGCFG`, AFL rule counts in `CFDGAFLCFG0`,
 *    `CFDGFDCFG`, `CFDRMNB`, `CFDRFCC[]`.
 *  - Cancel channel sleep via `CFDC[0].CTR.CHMDC`, programme
 *    `CFDC[0].NCFG` (nominal bit timing) and `CFDC2[0].DCFG`
 *    + `CFDC2[0].FDCFG` (data-phase timing + FD config).
 *  - Transition global mode then channel mode to operation.
 *  - Queue a frame into TX message buffer 0 by writing
 *    `CFDTM[0].ID`, `CFDTM[0].PTR`, `CFDTM[0].FDCTR`,
 *    `CFDTM[0].DF[]`, then asserting `CFDTMC[0].TMTR`.
 *  - Pop a frame from RX FIFO 0 by polling `CFDRFSTS[0].RFEMP`,
 *    reading `CFDRF[0].ID/PTR/FDSTS/DF[]`, and writing
 *    `CFDRFPCTR[0]` to advance the pointer.
 *
 * Every register access carries a HUM Ch 41 "CAN with Flexible
 * Data-rate (CANFD)" citation (pages 2702..2867, chapter map row 41)
 * or an FSP `r_canfd.c` line citation when the bit semantics come
 * from the reference driver.
 */

#include "ra8_canfd.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_canfd_regs.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hal_internal.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_register_protection.h"
#include "ra8_system_regs.h"

static const char* s_tag = "CANFD";

/**
 * @enum ra8_canfd_internal_t
 * @brief Internal tunables (mode-transition + clock-handshake spin budgets).
 *
 * @details
 * ``k_ra8_canfd_spin`` bounds CHLTSTS / CRSTSTS / GHLTSTS / GRSTSTS polls.
 * The CANFD channel/global state machine completes a mode transition
 * within a handful of CANFDCLK ticks (HUM Ch 41 "CFDCnCTR.CHMDC" p 2762
 * and "CFDGCTR" p 2742). At a CPU running 1 GHz and a tight 5-cycle
 * register-read poll, 20000 iterations ~= 100 us, well above the
 * documented worst-case wait but small enough that a stuck handshake
 * (e.g. CANFDCLK not actually stable) surfaces as an ``hw_timeout`` in
 * sub-millisecond time instead of bricking the HIL loop.
 * ``k_ra8_canfd_ckcr_spin`` is the matching budget for the
 * CANFDCKCR SREQ/SRDY handshake -- shares its order of magnitude with
 * the USB/SCI CKSRDY waits in ``ra8_cgc.c``.
 */
typedef enum : uint32_t {
  k_ra8_canfd_spin      = 20000U,  /**< Bounded poll budget in iterations. */
  k_ra8_canfd_ckcr_spin = 262144U, /**< CANFDCKCR SREQ/SRDY budget.        */
} ra8_canfd_internal_t;

/**
 * @brief Bounded wait on a `CFDC[0].STS` flag (reset/halt/operation ack).
 *
 * @details
 * HUM Ch 41 p 2766 "CFDCnSTS" -- after CHMDC is written the matching
 * status bit (CRSTSTS / CHLTSTS / etc) latches once the channel state
 * machine reaches the requested mode.
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] status_bit See header declaration for direction and constraints.
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
RA8_INTERNAL
static ra8_err_t internal_wait_status_bit(volatile r_canfd_t* reg, uint8_t status_bit)
{
  /* HUM Ch 41 p 2766 "CFDCnSTS" -- wait for the requested mode-ack bit
   * (CRSTSTS / CHLTSTS / ...) to latch. The host unit-test build runs this
   * same loop through the ra8_fake_mmio seam (ra8_hw_err.h), so a stuck
   * channel surfaces here as an hw_timeout instead of short-circuiting. */
  return ra8_hw_wait_flag_set32(&reg->CFDC[0].STS,
                                (uint32_t)(1UL << status_bit),
                                (uint32_t)k_ra8_canfd_spin);
}

/**
 * @var s_canfd_mstp_table
 * @brief Channel-index -> MSTP id lookup. CANFD0/1 have separate
 * MSTPC bits (HUM Ch 11.2.8 "MSTPCRC", page 447 chapter map row 11).
 */
static const ra8_mstp_t s_canfd_mstp_table[] = {
  k_ra8_mstp_canfd0,
  k_ra8_mstp_canfd1,
};

/**
 * @brief Drive `CFDC[0].CTR.CHMDC` and wait for the matching status bit.
 *
 * @details
 * Mirrors `r_canfd_mode_ctr_set` + `r_canfd_mode_transition` from FSP
 * `r_canfd.c`. The CHMDC field is bits [1:0] of CTR (FSP
 * `R_CANFD_CFDC_CTR_b.CHMDC`). After a write the channel state
 * machine drives CRSTSTS / CHLTSTS / CSLPSTS in CFDC[0].STS.
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] mode See header declaration for direction and constraints.
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
ra8_err_t ra8_canfd_internal_set_channel_mode(volatile r_canfd_t* reg, ra8_chmdc_mode_t mode)
{
  /* Read-modify-write the CTR register, mask CHMDC to bits [1:0],
   * stamp the requested mode. Also clear CSLPR (bit 2): the channel
   * comes out of reset with CSLPR=1 (sleep request), and CHMDC
   * writes are silently ignored while the channel is asleep -- the
   * state machine stays in CH_RESET, status bits never flip, and
   * the next mode write times out. JTAG dump after the original
   * init showed CTR=0x05 / STS=0x05 (CHMDC=01 RESET + CSLPR=1 +
   * CRSTSTS=1 + CSLPSTS=1) confirming the channel never woke.
   * Mirrors the GSLPR clear in internal_set_global_mode.
   * HUM Ch 41 p 2762 "CFDCnCTR.CHMDC" + "CFDCnCTR.CSLPR" */
  uint32_t ctr = reg->CFDC[0].CTR;
  ctr &= ~(k_ra8_cnctr_mask_chmdc | k_ra8_cnctr_mask_cslpr);
  ctr |= ((uint32_t)mode & k_ra8_cnctr_mask_chmdc);
  reg->CFDC[0].CTR = ctr;

  /* HUM Ch 41 p 2766 "CFDCnSTS" -- the channel state machine reflects
   * the requested mode by latching CHLTSTS (halt), CRSTSTS (reset), or
   * by clearing BOTH (operation). Returning before the chip latches the
   * new state lets a subsequent CHMDC write race against the in-flight
   * transition and the chip silently ignores it (FSP r_canfd
   * mode_transition does the matching three-mode poll). Symptom seen on
   * HIL prior to this wait: ra8_canfd_set_test_mode -> set CHMDC=halt
   * times out on CHLTSTS because the channel state machine was still
   * completing the GL/CH operation handshake from ra8_canfd_init. */
  if (mode == k_ra8_chmdc_halt) {
    return internal_wait_status_bit(reg, k_ra8_cnsts_bit_chltst);
  }
  if (mode == k_ra8_chmdc_reset) {
    return internal_wait_status_bit(reg, k_ra8_cnsts_bit_crstst);
  }
  /* Operation: poll for (CRSTSTS|CHLTSTS) == 0 so a subsequent CHMDC
   * write does not race the in-flight transition. Real silicon
   * converges within a handful of CANFDCLK ticks; the host unit-test
   * build drives this same loop through the ra8_fake_mmio seam
   * (ra8_hw_err.h) so a stuck channel surfaces here as an hw_timeout
   * rather than at the next mode write, which is what made
   * canfd_loopback report a phantom test_mode failure with the real
   * problem being "channel never reached operation". */
  const uint32_t reset_or_halt =
    (uint32_t)((1UL << k_ra8_cnsts_bit_crstst) | (1UL << k_ra8_cnsts_bit_chltst));
  return ra8_hw_wait_flag_clear32(&reg->CFDC[0].STS, reset_or_halt, (uint32_t)k_ra8_canfd_spin);
}

/**
 * @brief Drive `CFDGCTR.GMDC` and wait for matching `CFDGSTS` bit.
 *
 * @details
 * Mirrors FSP global-mode transition. GMDC is bits [1:0] of CFDGCTR
 * and clearing GSLPR (bit 2) is required to leave global sleep
 * (HUM Ch 41 p 2742 "CFDGCTR").
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] gmdc_value See header declaration for direction and constraints.
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
RA8_INTERNAL
static ra8_err_t internal_set_global_mode(volatile r_canfd_t* reg, uint32_t gmdc_value)
{
  /* HUM Ch 41 "CFDGCTR.GMDC" p 2742 */ /* "CFDGCTR.GMDC" + GSLPR clear. */
  uint32_t gctr = reg->CFDGCTR;
  gctr &= ~(k_ra8_gctr_mask_gmdc | k_ra8_gctr_mask_gslpr);
  gctr |= (gmdc_value & k_ra8_gctr_mask_gmdc);
  reg->CFDGCTR = gctr;

  if (gmdc_value == k_ra8_gctr_value_halt) {
    return ra8_hw_wait_flag_set32(&reg->CFDGSTS,
                                  (uint32_t)(1UL << k_ra8_gsts_bit_ghltsts),
                                  (uint32_t)k_ra8_canfd_spin);
  }
  if (gmdc_value == k_ra8_gctr_value_reset) {
    return ra8_hw_wait_flag_set32(&reg->CFDGSTS,
                                  (uint32_t)(1UL << k_ra8_gsts_bit_grststs),
                                  (uint32_t)k_ra8_canfd_spin);
  }
  /* Global OPERATION is the state where BOTH GRSTSTS and GHLTSTS
   * read 0. The host unit-test build drives this same loop through the
   * ra8_fake_mmio seam (ra8_hw_err.h) so a stuck global block surfaces
   * here as an hw_timeout rather than at the next channel-mode write.
   * HUM Ch 41 p 2746 "CFDGSTS" */
  const uint32_t reset_or_halt =
    (uint32_t)((1UL << k_ra8_gsts_bit_grststs) | (1UL << k_ra8_gsts_bit_ghltsts));
  return ra8_hw_wait_flag_clear32(&reg->CFDGSTS, reset_or_halt, (uint32_t)k_ra8_canfd_spin);
}

/**
 * @brief Program a default pass-all AFL rule routed into RX FIFO 0.
 *
 * @details
 * Without at least one Acceptance-Filter-List rule active, every
 * received frame is dropped by the controller before it reaches a
 * destination buffer (HUM Ch 41 "Acceptance Filter List" p 2731-2740:
 * routing is governed by the GAFLFDP0/1/8 bits in CFDGAFLP1, and
 * CFDGAFLCFG0.RNC0 must be non-zero for any rule to be considered).
 *
 * This helper installs rule 0 with mask = 0 (every ID matches) and
 * GAFLFDP0 = 1 (route into RX FIFO 0).  It must run while the global
 * block is in GL_RESET -- the documented edit window for AFL data and
 * for CFDGAFLCFG0 (HUM Ch 41 "CFDGAFLCFG0" p 2730 and "CFDGAFLECTR"
 * p 2729).
 *
 * @param[in] reg  Pointer to the CANFD channel register block.
 *
 * @pre  Global block is in GL_RESET.
 * @pre  ``reg`` is non-NULL and points to a CANFD instance.
 * @post Rule 0 accepts every ID and routes it to RX FIFO 0.
 * @post CFDGAFLECTR is re-locked (AFLDAE clear) on return.
 *
 * @note Internal helper, not exported.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_install_default_afl(volatile r_canfd_t* reg)
{
  /* HUM Ch 41 "CFDGAFLCFG0" p 2730 */ /* RNC0 = 1 -> one rule on page 0. */
  reg->CFDGAFLCFG0 = 1UL << 16U;

  /* HUM Ch 41 "CFDGAFLECTR" p 2734 */ /* page 0, AFLDAE = 1 unlocks edit. */
  reg->CFDGAFLECTR = k_ra8_gaflectr_bit_afldae;

  /* HUM Ch 41 "CFDGAFLID" p 2735 */ /* accept-id ignored: mask = 0. */
  reg->CFDGAFL[0].ID = 0U;
  /* HUM Ch 41 "CFDGAFLM" p 2736 */ /* mask = 0 -> accept every ID. */
  reg->CFDGAFL[0].M = 0U;
  /* HUM Ch 41 "CFDGAFLP0" p 2738 */ /* no RX-MB routing requested. */
  reg->CFDGAFL[0].P0 = 0U;
  /* HUM Ch 41 "CFDGAFLP1" p 2740 */ /* GAFLFDP0 = bit 0 -> RX FIFO 0. */
  reg->CFDGAFL[0].P1 = 1UL << 0U;

  /* Re-lock the AFL data window. */
  reg->CFDGAFLECTR = 0U;
}

/**
 * @brief Enable RX FIFO 0 with a sensible default depth and payload.
 *
 * @details
 * HUM Ch 41 "CFDRFCCa" p 2741: RFE is the last bit set in the register,
 * RFDC selects FIFO depth, RFPLS selects per-entry payload bytes.
 * Writing RFE = 1 while RFDC = 0 silently fails ("This bit can only be
 * set if the configured FIFO depth is greater than 0x000").
 *
 * Programmed values:
 *   - RFDC[10:8]   = 001b (4 messages)
 *   - RFPLS[6:4]   = 111b (64-byte CAN-FD payload)
 *   - RFE          = 1    (FIFO enabled)
 *
 * @param[in] reg Pointer to the CANFD channel register block.
 *
 * @pre  Global block is in GL_RESET (RFDC / RFPLS only writable here).
 * @pre  ``reg`` is non-NULL.
 * @post CFDRFCC[0] reflects depth=4 / payload=64 / RFE=1.
 * @post CFDRFSTS[0].RFEMP will clear when frames arrive.
 *
 * @note Internal helper, not exported.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_configure_rx_fifo0(volatile r_canfd_t* reg)
{
  /* Programme depth + payload in GL_RESET (RFE bit left zero -- it
   * is only writable in GL_HALT or GL_OPERATION per HUM Ch 41
   * "CFDRFCCa" p 2741. Setting RFE here would silently no-op, the
   * FIFO would stay disabled, and every loopback frame would land
   * in /dev/null instead of the RX FIFO.) */
  enum : uint32_t {
    k_rfcc_rfdc_4msgs = 1UL << 8U, /**< RFDC = 001b -> 4 entries.       */
    k_rfcc_rfpls_64   = 7UL << 4U, /**< RFPLS = 111b -> 64-byte CAN-FD. */
  };
  reg->CFDRFCC[0] = k_rfcc_rfdc_4msgs | k_rfcc_rfpls_64;
}

/**
 * @brief Enable RX FIFO 0 (RFE=1) -- must run in GL_HALT or GL_OPERATION.
 *
 * @details Asserts CFDRFCCa.RFE on RX FIFO 0 to bring the FIFO out of
 * the disabled / read-and-write-pointer-cleared state.
 *
 * @param[in] reg CANFD register block.
 *
 * @pre Global block is in GL_OPERATION (or GL_HALT).
 * @pre internal_configure_rx_fifo0 already programmed depth/payload.
 * @post CFDRFCC[0].RFE = 1; FIFO accepts inbound frames.
 * @post No other CFDRFCCa fields are touched (RMW preserves RFDC/RFPLS).
 * @note Not thread-safe; caller serialises ra8_canfd_init.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_enable_rx_fifo0(volatile r_canfd_t* reg)
{
  /* HUM Ch 41 "CFDRFCCa.RFE" p 2742 -- separate write after the rest of
   * the CFDRFCCa register has been set, while in GL_OPERATION. */
  reg->CFDRFCC[0] |= k_ra8_rfcc_bit_rfe;
}

/**
 * @brief Bounded wait on CANFDCKCR.CANFDCKSRDY reaching @p expected.
 *
 * @details
 * Mirrors ``internal_wait_usbcksrdy`` in ``ra8_cgc.c``. Polls
 * CANFDCKCR bit 7 (CANFDCKSRDY) until it equals @p expected or the
 * bounded budget ``k_ra8_canfd_ckcr_spin`` elapses.
 *
 * @param[in] expected 0U after the SREQ-clear write, 1U after SREQ=1.
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok           CKSRDY reached @p expected.
 * @retval k_ra8_err_hw_timeout SRDY never matched within the budget.
 *
 * @pre Caller holds the CGC-PRCR unlock window (PRCR=0xA501).
 * @pre ``expected`` is 0 or 1.
 * @post No register state is modified -- this is a read-only poll.
 * @post On timeout the caller relocks PRCR.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_wait_canfdcksrdy(uint8_t expected)
{
  /* SRDY (clock-source ready) is bit 7 of CANFDCKCR. @p expected is
   * always 0 or 1, so this reduces to a single-bit set/clear wait; the
   * host unit-test build runs the same loop through the ra8_fake_mmio
   * seam (ra8_hw_err.h) instead of faking the ack write. */
  /* HUM Ch 9.2.46 "CANFDCKCR.CANFDCKSRDY" p 366 */
  volatile uint8_t* const ckcr = ra8_sys_canfdckcr();
  const uint8_t           mask = (uint8_t)(1U << k_ra8_usbckcr_bit_srdy);
  if (expected != 0U) {
    return ra8_hw_wait_flag_set8(ckcr, mask, (uint32_t)k_ra8_canfd_ckcr_spin);
  }
  return ra8_hw_wait_flag_clear8(ckcr, mask, (uint32_t)k_ra8_canfd_ckcr_spin);
}

/**
 * @brief Block-level CANFD clock init -- run BEFORE the first MSTP release.
 *
 * @details
 * HUM Ch 11.2.8 "MSTPCRC" Note 4 (p 446) states that MSTPC26 / MSTPC27
 * (the per-channel CANFD module-stop bits) must be written AFTER the
 * CANFDCLK is stable. CANFDCKCR resets to ``0x01`` (CANFDCKSEL = MOCO,
 * CANFDCKSREQ = 0, CANFDCKSRDY = 0). MOCO is on at reset, but the
 * RA8D2 CGC requires an explicit SREQ -> SRDY -> SREQ-clear handshake
 * before the chip raises ``CANFDCKSRDY`` and declares the clock stable.
 * Without that handshake the canfd block's internal state machine
 * cannot reach CH_HALT after the first ``CFDCnCTR.CHMDC`` write --
 * symptom seen on HIL: ``ra8_canfd_set_test_mode -> ra8_canfd_internal_set_channel_mode
 * (k_ra8_chmdc_halt) -> internal_wait_status_bit`` times out on CHLTSTS
 * for ``can_classic_loopback`` / ``canfd_loopback`` / ``canfd_filter_demo``.
 *
 * Steps (mirrors FSP bsp_clocks.c ``CANFD CLK`` block + the USBCKCR
 * pattern in ``ra8_cgc.c``):
 *   1. Write CANFDCKDIVCR = 0 (/1 -- documented reset value).
 *   2. Set CANFDCKCR.CANFDCKSREQ = 1 (request switch) while keeping the
 *      reset-default CANFDCKSEL = MOCO.
 *   3. Wait CANFDCKSRDY = 1.
 *   4. Re-write CANFDCKCR with SREQ=0, source = MOCO -- commits the
 *      switch.
 *   5. Wait CANFDCKSRDY = 0 (handshake done).
 *
 * This helper is idempotent via a static guard: only the first caller
 * performs the handshake; subsequent ``ra8_canfd_init`` calls (e.g. for
 * channel 1 after channel 0) skip it.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok            CANFDCLK declared stable; safe to release MSTP.
 * @retval k_ra8_err_hw_timeout CKSRDY handshake stuck.
 *
 * @pre Single-threaded init context (no other CGC writes in flight).
 * @pre MOCO is running -- chip reset default; ra8_cgc_init does not
 *      explicitly stop MOCO.
 * @post On k_ra8_ok the CANFD block clock is stable; MSTPC26/27 may now
 *       be released.
 * @post On error the canfd MSTP gate is NOT touched; caller decides
 *       whether to proceed with the documented "best-effort" recovery.
 * @post PRCR is re-locked.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_canfd_clock_block_init(void)
{
  static bool s_canfd_clock_inited = false;
  if (s_canfd_clock_inited) {
    return k_ra8_ok;
  }
  ra8_err_t err = k_ra8_ok;
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    /* HUM Ch 9.2.41 "CANFDCKDIVCR" p 357 -- /1 divider keeps MOCO at
     * its native rate (~8 MHz nominal; PCLKA on this project is
     * 100 MHz so MOCO < PCLKA satisfies HUM Ch 41.1.2 clock
     * restriction CANFDCLK <= PCLKA). */
    *ra8_sys_canfdckdivcr() = 0U;

    /* HUM Ch 9.2.46 "CANFDCKCR.CANFDCKSREQ" p 366 -- assert SREQ with
     * the reset-default source (MOCO, CANFDCKSEL = 0001b). */
    const uint8_t sreq_mask = (uint8_t)(1U << k_ra8_usbckcr_bit_sreq);
    const uint8_t src_moco  = 0x01U;
    *ra8_sys_canfdckcr()    = (uint8_t)(src_moco | sreq_mask);

    /* Step 3: wait for SRDY = 1 (chip acknowledges the request). */
    err = internal_wait_canfdcksrdy(1U);
    if (err != k_ra8_ok) {
      ra8_log_error(s_tag, "canfd: CANFDCKSRDY=1 timeout");
      break;
    }
    /* Step 4: drop SREQ -- commits the (same) source selection. */
    *ra8_sys_canfdckcr() = src_moco;
    /* Step 5: wait for SRDY = 0 -- handshake done. */
    err = internal_wait_canfdcksrdy(0U);
    if (err != k_ra8_ok) {
      ra8_log_error(s_tag, "canfd: CANFDCKSRDY=0 timeout");
      break;
    }
  }
  if (err == k_ra8_ok) {
    s_canfd_clock_inited = true;
    ra8_log_info(s_tag, "canfd block clock stable");
  }
  return err;
}

/**
 * @brief Push the CANFD global+channel state machines into OPERATION.
 *
 * @details Mirrors FSP r_canfd.c lines ~311..417: cancel global sleep
 * via GL_RESET, cancel channel sleep via CH_RESET, install a pass-all
 * AFL + RX FIFO 0 while in GL_RESET (HUM Ch 41 p 2729..2742), then
 * transition into GL_OPERATION and CH_OPERATION.
 *
 * @param[in] reg CANFD register block for the target channel.
 *
 * @return ra8_err_t outcome of the final state transition.
 * @retval k_ra8_ok               Channel reached CH_OPERATION.
 * @retval k_ra8_err_hw_timeout   GL_OPERATION or CH_OPERATION never latched.
 *
 * @pre Clock block and MSTP for the channel are already alive.
 * @pre GRAMINIT bit has cleared (caller polled).
 * @post On success the channel is in CH_OPERATION ready for TX/RX.
 * @post On failure the channel is left in whichever transitional
 *       state stalled; caller treats it as init-failed.
 * @note Not thread-safe; caller serialises ra8_canfd_init.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_canfd_open_channel(volatile r_canfd_t* reg)
{
  (void)internal_set_global_mode(reg, k_ra8_gctr_value_reset);
  (void)ra8_canfd_internal_set_channel_mode(reg, k_ra8_chmdc_reset);

  internal_install_default_afl(reg);
  internal_configure_rx_fifo0(reg);

  const ra8_err_t gop_err = internal_set_global_mode(reg, k_ra8_gctr_value_operation);
  if (gop_err != k_ra8_ok) {
    return gop_err;
  }
  /* RFE is only writable in GL_HALT / GL_OPERATION, so enable RX FIFO
   * 0 only after the global block has actually transitioned. */
  internal_enable_rx_fifo0(reg);
  return ra8_canfd_internal_set_channel_mode(reg, k_ra8_chmdc_operation);
}

ra8_err_t ra8_canfd_init(uint8_t channel)
{
  volatile r_canfd_t* reg = ra8_canfd(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if (channel >= (uint8_t)(sizeof(s_canfd_mstp_table) / sizeof(s_canfd_mstp_table[0]))) {
    return k_ra8_err_invalid_arg;
  }
  /* MSTPC26/27 must be written AFTER CANFDCLK is stable.
   * HUM Ch 11.2.8 "MSTPCRC" Note 4 p 446 */
  const ra8_err_t clk_err = internal_canfd_clock_block_init();
  if (clk_err != k_ra8_ok) {
    return clk_err;
  }

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
  const ra8_err_t mst_err = ra8_mstp_enable(s_canfd_mstp_table[channel]);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "canfd_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* Wait for RAM init done (CFDGSTS.GRAMINIT clears). */
  /* HUM Ch 41.2 "CFDGSTS : Global Status Register" p 2730 */
  for (uint32_t i = 0U; i < k_ra8_canfd_spin; i++) { /* GCOVR_EXCL_BR_LINE */
    if ((reg->CFDGSTS & (uint32_t)(1UL << k_ra8_gsts_bit_graminit)) ==
        0U) { /* GCOVR_EXCL_BR_LINE */
      break;
    }
  }

  const ra8_err_t open_err = internal_canfd_open_channel(reg);
  if (open_err != k_ra8_ok) {
    return open_err;
  }
  ra8_log_info_val(s_tag, "canfd_init ch", (uint32_t)channel);
  return k_ra8_ok;
}

ra8_err_t ra8_canfd_deinit(uint8_t channel)
{
  volatile r_canfd_t* reg = ra8_canfd(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  /* HUM Ch 41 "CFDCnCTR.CHMDC" p 2762 */ /* "CFDCnCTR.CHMDC" -- park channel in reset. */
  (void)ra8_canfd_internal_set_channel_mode(reg, k_ra8_chmdc_reset);
  return k_ra8_ok;
}

/* =============================================================================
 * status + IRQ + power transition
 * =============================================================================
 */

static ra8_canfd_event_fn_t s_canfd_fn;
static void*                s_canfd_ctx;

ra8_err_t ra8_canfd_get_status(uint8_t channel, uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_canfd_t* reg = ra8_canfd(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 41 "CFDCnSTS" p 2766 */ /* "CFDCnSTS". */
  *out_mask = reg->CFDC[0].STS;
  return k_ra8_ok;
}

ra8_err_t ra8_canfd_clear_status(uint8_t channel, uint32_t mask)
{
  volatile r_canfd_t* reg = ra8_canfd(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 41 p 2772 "CFDCnERFL" -- error flags are W0C: writing 0
   * clears, writing 1 leaves untouched. We compute the inverse mask
   * to match the previous behaviour ("clear bits in mask"). */
  reg->CFDC[0].ERFL = reg->CFDC[0].ERFL & ~mask;
  return k_ra8_ok;
}

ra8_err_t ra8_canfd_attach_handler(ra8_canfd_event_fn_t fn, void* ctx)
{
  s_canfd_fn  = fn;
  s_canfd_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_canfd_dispatch(uint8_t channel)
{
  volatile r_canfd_t* reg = ra8_canfd(channel);
  if (reg == nullptr) {
    return;
  }
  /* HUM Ch 41 "CFDCnERFL" p 2772 */ /* "CFDCnERFL" snapshot then ack. */
  const uint32_t             mask = reg->CFDC[0].ERFL;
  const ra8_canfd_event_fn_t fn   = s_canfd_fn;
  void* const                ctx  = s_canfd_ctx;
  reg->CFDC[0].ERFL               = 0U;
  if (fn != nullptr) {
    fn(ctx, channel, mask);
  }
}

/** @brief Number of AFL slots that live on page 0. */
enum : uint16_t {
  k_ra8_canfd_afl_per_page = 16U, /**< RA8 CANFD afl per page. */
};

/**
 * @brief Bump CFDGAFLCFG0.RNC0 to cover @p filter_id rules on page 0.
 * @details Caller must already have placed the global block in
 * GL_RESET; RNC0 is RESET-only per HUM Ch 41.2.18 p 2735.
 * @param[in] reg       CANFD register block (channel 0).
 * @param[in] filter_id Filter slot index in [0, k_ra8_canfd_afl_total).
 * @pre Block is in GL_RESET.
 * @pre filter_id < k_ra8_canfd_afl_per_page (page-0 only).
 * @post RNC0 >= filter_id + 1.
 * @post No global state outside CFDGAFLCFG0 is modified.
 * @note Not thread-safe; serialise filter edits.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_bump_rnc0_locked(volatile r_canfd_t* reg, uint16_t filter_id)
{
  if (filter_id >= (uint16_t)k_ra8_canfd_afl_per_page) {
    return;
  }
  const uint32_t cur_rnc =
    (reg->CFDGAFLCFG0 >> k_ra8_gaflcfg0_shift_rnc0) & k_ra8_gaflcfg0_mask_rnc0;
  const uint32_t new_rnc = ((uint32_t)filter_id + 1U) & k_ra8_gaflcfg0_mask_rnc0;
  if (cur_rnc < new_rnc) {
    const uint32_t cfg0 = reg->CFDGAFLCFG0;
    reg->CFDGAFLCFG0    = (cfg0 & ~(k_ra8_gaflcfg0_mask_rnc0 << k_ra8_gaflcfg0_shift_rnc0)) |
                          ((new_rnc & k_ra8_gaflcfg0_mask_rnc0) << k_ra8_gaflcfg0_shift_rnc0);
  }
}

/**
 * @brief Write one CFDGAFL slot (ID/M/P0/P1) with the loopback +
 * FIFO-0 routing bits set.
 * @details Mask register OR-includes GAFLLB (bit 29) so the entry is
 * valid under Self-test mode 0/1 (HUM Ch 41.5.5 Table 41.22); P1
 * OR-includes GAFLFDP0 so matched frames land in RX FIFO 0 (HUM Ch
 * 41.2.22 p 2740).
 * @param[in] reg       CANFD register block.
 * @param[in] slot      Slot index inside the unlocked page (0..15).
 * @param[in] accept_id 11/29-bit accept ID.
 * @param[in] mask      11/29-bit ID mask (caller-provided bits only).
 * @param[in] dlc       DLC value 0..15.
 * @pre Global block is in GL_RESET (CFDGAFL entries are RESET-only).
 * @pre Caller holds the AFL data window unlock (CFDGAFLECTR.AFLDAE=1).
 * @post CFDGAFL[slot] reflects the caller's accept_id / mask / dlc
 *       with GAFLLB and GAFLFDP0 set.
 * @post No other AFL slot is modified.
 * @note Not thread-safe; caller serialises AFL edits.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_write_afl_slot(volatile r_canfd_t* reg,
                                    uint16_t            slot,
                                    uint32_t            accept_id,
                                    uint32_t            mask,
                                    uint8_t             dlc)
{
  reg->CFDGAFL[slot].ID = accept_id;
  reg->CFDGAFL[slot].M  = mask | (uint32_t)k_ra8_gaflm_bit_gafllb;
  reg->CFDGAFL[slot].P0 = 0U;
  reg->CFDGAFL[slot].P1 =
    (((uint32_t)dlc & k_ra8_canfd_ptr_mask_dlc) << k_ra8_canfd_ptr_shift_dlc) |
    (uint32_t)k_ra8_gaflp1_bit_gaflfdp0;
}

/* ra8_canfd_filter_set -- see header for full description.
 *
 * HUM Ch 41.2.18 / 41.2.20 / 41.2.22 p 2735-2742: CFDGAFLCFG0.RNC0
 * and the per-slot CFDGAFL.{ID,M,P0,P1} registers are only writable
 * while the global block is in GL_RESET. Writing them in
 * GL_OPERATION lands the bits in the register file but the AFL
 * lookup engine never resamples them, so live filtering keeps using
 * the snapshot taken at the most recent GL_RESET -> GL_OPERATION
 * transition. install_default_afl in internal_canfd_open_channel
 * runs in GL_RESET for that reason; we mirror the same RESET ->
 * write -> OPERATION transaction here so updates take effect. */
ra8_err_t ra8_canfd_filter_set(uint16_t filter_id, uint32_t accept_id, uint32_t mask, uint8_t dlc)
{
  if (filter_id >= k_ra8_canfd_afl_total) {
    return k_ra8_err_invalid_arg;
  }
  if (dlc > k_ra8_canfd_dlc_max) {
    return k_ra8_err_invalid_arg;
  }
  if ((accept_id & ~k_ra8_canfd_id_ext_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }

  const uint16_t page = (uint16_t)(filter_id / k_ra8_canfd_afl_per_page);
  const uint16_t slot = (uint16_t)(filter_id % k_ra8_canfd_afl_per_page);

  /* AFL is global across instances; access via channel 0. */
  volatile r_canfd_t* reg = ra8_canfd(0U);
  RA8_CHECK_NULL_PTR(reg, s_tag, "filter_set: channel0 unavailable");

  const ra8_err_t reset_err = internal_set_global_mode(reg, k_ra8_gctr_value_reset);
  if (reset_err != k_ra8_ok) {
    return reset_err;
  }

  internal_bump_rnc0_locked(reg, filter_id);

  /* Unlock AFL data window via AFLDAE bit. */
  /* HUM Ch 41.2 "CFDGAFLECTR : AFL Entry Control Register" p 2734 */
  reg->CFDGAFLECTR = ((uint32_t)page & k_ra8_gaflectr_mask_aflpn) | k_ra8_gaflectr_bit_afldae;
  internal_write_afl_slot(reg, slot, accept_id, mask, dlc);
  reg->CFDGAFLECTR = 0U;

  const ra8_err_t op_err = internal_set_global_mode(reg, k_ra8_gctr_value_operation);
  if (op_err != k_ra8_ok) {
    return op_err;
  }
  /* GL_RESET cleared CFDRFCCa.RFE (HUM Ch 41 p 2742); re-enable so
   * matched frames have a landing FIFO once we resume operation. */
  internal_enable_rx_fifo0(reg);
  return k_ra8_ok;
}

ra8_err_t ra8_canfd_set_test_mode(uint8_t channel, ra8_ctms_mode_t mode)
{
  volatile r_canfd_t* reg = ra8_canfd(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if ((uint8_t)mode > (uint8_t)k_ra8_ctms_self_test_1) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 41 "CFDCnCTR" p 2710 -- CTME (bit 24) and CTMS (bits
   * [26:25]) are only writable in CH_HALT mode.  ra8_canfd_init parks
   * the channel in CH_OPERATION so we transition through CH_HALT to
   * land the test-mode select, then return to CH_OPERATION. */
  const ra8_err_t halt_err = ra8_canfd_internal_set_channel_mode(reg, k_ra8_chmdc_halt);
  if (halt_err != k_ra8_ok) {
    /* Best-effort recover the channel before reporting the error. */
    (void)ra8_canfd_internal_set_channel_mode(reg, k_ra8_chmdc_operation);
    return halt_err;
  }

  /* Read-modify-write CTR: clear any prior CTME/CTMS, then OR in the
   * requested selector with CTME = 1. */
  uint32_t ctr = reg->CFDC[0].CTR;
  ctr &= ~(k_ra8_cnctr_mask_ctme | k_ra8_cnctr_mask_ctms);
  ctr |= k_ra8_cnctr_mask_ctme;
  ctr |= ((uint32_t)mode << (uint32_t)k_ra8_cnctr_bit_ctms) & k_ra8_cnctr_mask_ctms;
  reg->CFDC[0].CTR = ctr;

  return ra8_canfd_internal_set_channel_mode(reg, k_ra8_chmdc_operation);
}

ra8_err_t ra8_canfd_set_iso_mode(bool enable)
{
  volatile r_canfd_t* reg = ra8_canfd(0U);
  RA8_CHECK_NULL_PTR(reg, s_tag, "set_iso_mode: channel0 unavailable");
  /* HUM Ch 41 "CFDGFDCFG" pp 2702-2867 -- bit 0 (NISO) is set for ISO
   * mode (default) and cleared for non-ISO Bosch framing. */
  uint32_t v = reg->CFDGFDCFG;
  if (enable) {
    v |= k_ra8_gfdcfg_bit_niso;
  } else {
    v &= ~k_ra8_gfdcfg_bit_niso;
  }
  reg->CFDGFDCFG = v;
  return k_ra8_ok;
}

ra8_err_t ra8_canfd_enter_stop(uint8_t channel)
{
  if (channel >= k_ra8_canfd_instance_count) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 11.2.8 "MSTPCRC" p 447 */ /* gate channel clock back off. */
  return ra8_mstp_disable(s_canfd_mstp_table[channel]);
}

ra8_err_t ra8_canfd_exit_stop(uint8_t channel)
{
  if (channel >= k_ra8_canfd_instance_count) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 11.2.8 "MSTPCRC" p 447 */ /* ungate channel clock. */
  return ra8_mstp_enable(s_canfd_mstp_table[channel]);
}
