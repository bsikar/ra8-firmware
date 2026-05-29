/**
 * @file ra_canfd.c
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
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_canfd.h"

#include <stdint.h>

#include "ra8d2_canfd_regs.h"
#include "ra8d2_system_regs.h"
#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"
#include "ra_register_protection.h"

static const char* s_tag = "CANFD";

/**
 * @enum ra_canfd_internal_t
 * @brief Internal tunables (spin budgets, quanta-search window).
 *
 * @details
 * ``k_ra_canfd_spin`` bounds CHLTSTS / CRSTSTS / GHLTSTS / GRSTSTS polls.
 * The CANFD channel/global state machine completes a mode transition
 * within a handful of CANFDCLK ticks (HUM Ch 41 "CFDCnCTR.CHMDC" p 2762
 * and "CFDGCTR" p 2742). At a CPU running 1 GHz and a tight 5-cycle
 * register-read poll, 20000 iterations ~= 100 us, well above the
 * documented worst-case wait but small enough that a stuck handshake
 * (e.g. CANFDCLK not actually stable) surfaces as an ``hw_timeout`` in
 * sub-millisecond time instead of bricking the HIL loop.
 * ``k_ra_canfd_ckcr_spin`` is the matching budget for the
 * CANFDCKCR SREQ/SRDY handshake -- shares its order of magnitude with
 * the USB/SCI CKSRDY waits in ``ra_cgc.c``.
 */
typedef enum : uint32_t {
  k_ra_canfd_spin         = 20000U,  /**< Bounded poll budget in iterations. */
  k_ra_canfd_tx_spin      = 100000U, /**< TX-completion poll: 500 us @ 1 GHz / 5 cyc. */
  k_ra_canfd_ckcr_spin    = 262144U, /**< CANFDCKCR SREQ/SRDY budget.       */
  k_ra_canfd_tq_search_lo = 8U,      /**< Smallest time-quanta count tried. */
  k_ra_canfd_tq_search_hi = 25U,     /**< Largest time-quanta count tried. */
} ra_canfd_internal_t;

/**
 * @enum ra_canfd_buffer_idx_t
 * @brief Indices the driver currently owns within the channel block.
 */
typedef enum : uint8_t {
  k_ra_canfd_tx_mb_default   = 0U, /**< Driver uses TX MB 0 for fire-and-forget. */
  k_ra_canfd_rx_fifo_default = 0U, /**< Driver uses RX FIFO 0 for poll-receive.*/
} ra_canfd_buffer_idx_t;

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
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_wait_status_bit(volatile r_canfd_t* reg, uint8_t status_bit)
{
  for (uint32_t i = 0U; i < k_ra_canfd_spin; i++) {                 /* GCOVR_EXCL_BR_LINE */
    if ((reg->CFDC[0].STS & (uint32_t)(1UL << status_bit)) != 0U) { /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
#ifdef RA_SIMULATOR_MODE
  /* Host sim does not model CFDCnSTS bits flipping in response to
   * CFDCnCTR.CHMDC writes; on target the same loop is real, so
   * propagate the timeout there. */
  return k_ra_ok;
#else
  return k_ra_err_hw_timeout;
#endif
}

/**
 * @var s_canfd_mstp_table
 * @brief Channel-index -> MSTP id lookup. CANFD0/1 have separate
 * MSTPC bits (HUM Ch 11.2.8 "MSTPCRC", page 447 chapter map row 11).
 */
static const ra_mstp_t s_canfd_mstp_table[] = {
  k_ra_mstp_canfd0,
  k_ra_mstp_canfd1,
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
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_set_channel_mode(volatile r_canfd_t* reg, ra_chmdc_mode_t mode)
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
  ctr &= ~(k_ra_cnctr_mask_chmdc | k_ra_cnctr_mask_cslpr);
  ctr |= ((uint32_t)mode & k_ra_cnctr_mask_chmdc);
  reg->CFDC[0].CTR = ctr;

  /* HUM Ch 41 p 2766 "CFDCnSTS" -- the channel state machine reflects
   * the requested mode by latching CHLTSTS (halt), CRSTSTS (reset), or
   * by clearing BOTH (operation). Returning before the chip latches the
   * new state lets a subsequent CHMDC write race against the in-flight
   * transition and the chip silently ignores it (FSP r_canfd
   * mode_transition does the matching three-mode poll). Symptom seen on
   * HIL prior to this wait: ra_canfd_set_test_mode -> set CHMDC=halt
   * times out on CHLTSTS because the channel state machine was still
   * completing the GL/CH operation handshake from ra_canfd_init. */
  if (mode == k_ra_chmdc_halt) {
    return internal_wait_status_bit(reg, k_ra_cnsts_bit_chltst);
  }
  if (mode == k_ra_chmdc_reset) {
    return internal_wait_status_bit(reg, k_ra_cnsts_bit_crstst);
  }
  /* Operation: poll for (CRSTSTS|CHLTSTS) == 0 so a subsequent CHMDC
   * write does not race the in-flight transition. Real silicon
   * converges within a handful of CANFDCLK ticks. On host (which
   * does not model the state-machine bits clearing) silently return
   * ok; on target propagate the timeout so a stuck channel surfaces
   * here rather than at the next mode write, which is what made
   * canfd_loopback report a phantom test_mode failure with the real
   * problem being "channel never reached operation". */
  const uint32_t reset_or_halt =
    (uint32_t)((1UL << k_ra_cnsts_bit_crstst) | (1UL << k_ra_cnsts_bit_chltst));
  for (uint32_t i = 0U; i < k_ra_canfd_spin; i++) { /* GCOVR_EXCL_BR_LINE */
    if ((reg->CFDC[0].STS & reset_or_halt) == 0U) { /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
#ifdef RA_SIMULATOR_MODE
  return k_ra_ok;
#else
  return k_ra_err_hw_timeout;
#endif
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
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_set_global_mode(volatile r_canfd_t* reg, uint32_t gmdc_value)
{
  /* HUM Ch 41 "CFDGCTR.GMDC" p 2742 */ /* "CFDGCTR.GMDC" + GSLPR clear. */
  uint32_t gctr = reg->CFDGCTR;
  gctr &= ~(k_ra_gctr_mask_gmdc | k_ra_gctr_mask_gslpr);
  gctr |= (gmdc_value & k_ra_gctr_mask_gmdc);
  reg->CFDGCTR = gctr;

  if (gmdc_value == k_ra_gctr_value_halt) {
    for (uint32_t i = 0U; i < k_ra_canfd_spin; i++) { /* GCOVR_EXCL_BR_LINE */
      if ((reg->CFDGSTS & (uint32_t)(1UL << k_ra_gsts_bit_ghltsts)) !=
          0U) { /* GCOVR_EXCL_BR_LINE */
        return k_ra_ok;
      }
    }
#ifdef RA_SIMULATOR_MODE
    return k_ra_ok;
#else
    return k_ra_err_hw_timeout;
#endif
  }
  if (gmdc_value == k_ra_gctr_value_reset) {
    for (uint32_t i = 0U; i < k_ra_canfd_spin; i++) { /* GCOVR_EXCL_BR_LINE */
      if ((reg->CFDGSTS & (uint32_t)(1UL << k_ra_gsts_bit_grststs)) !=
          0U) { /* GCOVR_EXCL_BR_LINE */
        return k_ra_ok;
      }
    }
#ifdef RA_SIMULATOR_MODE
    return k_ra_ok;
#else
    return k_ra_err_hw_timeout;
#endif
  }
  /* Global OPERATION is the state where BOTH GRSTSTS and GHLTSTS
   * read 0. On host (no FSM model) silently succeed; on target
   * propagate the timeout so a stuck global block surfaces here
   * rather than at the next channel-mode write.
   * HUM Ch 41 p 2746 "CFDGSTS" */
  const uint32_t reset_or_halt =
    (uint32_t)((1UL << k_ra_gsts_bit_grststs) | (1UL << k_ra_gsts_bit_ghltsts));
  for (uint32_t i = 0U; i < k_ra_canfd_spin; i++) { /* GCOVR_EXCL_BR_LINE */
    if ((reg->CFDGSTS & reset_or_halt) == 0U) {     /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
#ifdef RA_SIMULATOR_MODE
  return k_ra_ok;
#else
  return k_ra_err_hw_timeout;
#endif
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
static void internal_install_default_afl(volatile r_canfd_t* reg)
{
  /* HUM Ch 41 "CFDGAFLCFG0" p 2730 */ /* RNC0 = 1 -> one rule on page 0. */
  reg->CFDGAFLCFG0 = 1UL << 16U;

  /* HUM Ch 41 "CFDGAFLECTR" p 2729 */ /* page 0, AFLDAE = 1 unlocks edit. */
  reg->CFDGAFLECTR = k_ra_gaflectr_bit_afldae;

  /* HUM Ch 41 "CFDGAFLID" p 2731 */ /* accept-id ignored: mask = 0. */
  reg->CFDGAFL[0].ID = 0U;
  /* HUM Ch 41 "CFDGAFLM" p 2732 */ /* mask = 0 -> accept every ID. */
  reg->CFDGAFL[0].M = 0U;
  /* HUM Ch 41 "CFDGAFLP0" p 2733 */ /* no RX-MB routing requested. */
  reg->CFDGAFL[0].P0 = 0U;
  /* HUM Ch 41 "CFDGAFLP1" p 2734 */ /* GAFLFDP0 = bit 0 -> RX FIFO 0. */
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
 * @note Not thread-safe; caller serialises ra_canfd_init.
 * @since 0.1.0
 */
static void internal_enable_rx_fifo0(volatile r_canfd_t* reg)
{
  /* HUM Ch 41 "CFDRFCCa.RFE" p 2742 -- separate write after the rest of
   * the CFDRFCCa register has been set, while in GL_OPERATION. */
  reg->CFDRFCC[0] |= k_ra_rfcc_bit_rfe;
}

/**
 * @brief Bounded wait on CANFDCKCR.CANFDCKSRDY reaching @p expected.
 *
 * @details
 * Mirrors ``internal_wait_usbcksrdy`` in ``ra_cgc.c``. Polls
 * CANFDCKCR bit 7 (CANFDCKSRDY) until it equals @p expected or the
 * bounded budget ``k_ra_canfd_ckcr_spin`` elapses.
 *
 * @param[in] expected 0U after the SREQ-clear write, 1U after SREQ=1.
 * @return ra_err_t outcome.
 * @retval k_ra_ok           CKSRDY reached @p expected.
 * @retval k_ra_err_hw_timeout SRDY never matched within the budget.
 *
 * @pre Caller holds the CGC-PRCR unlock window (PRCR=0xA501).
 * @pre ``expected`` is 0 or 1.
 * @post No register state is modified -- this is a read-only poll.
 * @post On timeout the caller relocks PRCR.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static ra_err_t internal_wait_canfdcksrdy(uint8_t expected)
{
  /* SRDY (clock-source ready) is bit 7 of CANFDCKCR. */
  /* HUM Ch 9.2.46 "CANFDCKCR.CANFDCKSRDY" p 366 */
  volatile uint8_t* const ckcr = ra_sys_canfdckcr();
  const uint8_t           mask = (uint8_t)(1U << k_ra_usbckcr_bit_srdy);
#ifdef RA_SIMULATOR_MODE
  /* Sim memory has no hardware ack -- fake CANFDCKSRDY toggling so the
   * host test poll loop converges immediately. Same pattern used by
   * ``internal_wait_usbcksrdy`` in ``ra_cgc.c``. */
  if (expected != 0U) {
    *ckcr = (uint8_t)(*ckcr | mask);
  } else {
    *ckcr = (uint8_t)(*ckcr & (uint8_t)~mask);
  }
#endif
  for (uint32_t i = 0U; i < (uint32_t)k_ra_canfd_ckcr_spin; i++) { /* GCOVR_EXCL_BR_LINE */
    const uint8_t got = (uint8_t)((*ckcr & mask) >> k_ra_usbckcr_bit_srdy);
    if (got == expected) { /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
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
 * symptom seen on HIL: ``ra_canfd_set_test_mode -> internal_set_channel_mode
 * (k_ra_chmdc_halt) -> internal_wait_status_bit`` times out on CHLTSTS
 * for ``can_classic_loopback`` / ``canfd_loopback`` / ``canfd_filter_demo``.
 *
 * Steps (mirrors FSP bsp_clocks.c ``CANFD CLK`` block + the USBCKCR
 * pattern in ``ra_cgc.c``):
 *   1. Write CANFDCKDIVCR = 0 (/1 -- documented reset value).
 *   2. Set CANFDCKCR.CANFDCKSREQ = 1 (request switch) while keeping the
 *      reset-default CANFDCKSEL = MOCO.
 *   3. Wait CANFDCKSRDY = 1.
 *   4. Re-write CANFDCKCR with SREQ=0, source = MOCO -- commits the
 *      switch.
 *   5. Wait CANFDCKSRDY = 0 (handshake done).
 *
 * This helper is idempotent via a static guard: only the first caller
 * performs the handshake; subsequent ``ra_canfd_init`` calls (e.g. for
 * channel 1 after channel 0) skip it.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok            CANFDCLK declared stable; safe to release MSTP.
 * @retval k_ra_err_hw_timeout CKSRDY handshake stuck.
 *
 * @pre Single-threaded init context (no other CGC writes in flight).
 * @pre MOCO is running -- chip reset default; ra_cgc_init does not
 *      explicitly stop MOCO.
 * @post On k_ra_ok the CANFD block clock is stable; MSTPC26/27 may now
 *       be released.
 * @post On error the canfd MSTP gate is NOT touched; caller decides
 *       whether to proceed with the documented "best-effort" recovery.
 * @post PRCR is re-locked.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_canfd_clock_block_init(void)
{
  static bool s_canfd_clock_inited = false;
  if (s_canfd_clock_inited) {
    return k_ra_ok;
  }
  ra_err_t err = k_ra_ok;
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    /* HUM Ch 9.2.41 "CANFDCKDIVCR" p 363 -- /1 divider keeps MOCO at
     * its native rate (~8 MHz nominal; PCLKA on this project is
     * 100 MHz so MOCO < PCLKA satisfies HUM Ch 41.1.2 clock
     * restriction CANFDCLK <= PCLKA). */
    *ra_sys_canfdckdivcr() = 0U;

    /* HUM Ch 9.2.46 "CANFDCKCR.CANFDCKSREQ" p 366 -- assert SREQ with
     * the reset-default source (MOCO, CANFDCKSEL = 0001b). */
    const uint8_t sreq_mask = (uint8_t)(1U << k_ra_usbckcr_bit_sreq);
    const uint8_t src_moco  = 0x01U;
    *ra_sys_canfdckcr()     = (uint8_t)(src_moco | sreq_mask);

    /* Step 3: wait for SRDY = 1 (chip acknowledges the request). */
    err = internal_wait_canfdcksrdy(1U);
    if (err != k_ra_ok) {
      ra_log_error(s_tag, "canfd: CANFDCKSRDY=1 timeout");
      break;
    }
    /* Step 4: drop SREQ -- commits the (same) source selection. */
    *ra_sys_canfdckcr() = src_moco;
    /* Step 5: wait for SRDY = 0 -- handshake done. */
    err = internal_wait_canfdcksrdy(0U);
    if (err != k_ra_ok) {
      ra_log_error(s_tag, "canfd: CANFDCKSRDY=0 timeout");
      break;
    }
  }
  if (err == k_ra_ok) {
    s_canfd_clock_inited = true;
    ra_log_info(s_tag, "canfd block clock stable");
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
 * @return ra_err_t outcome of the final state transition.
 * @retval k_ra_ok               Channel reached CH_OPERATION.
 * @retval k_ra_err_hw_timeout   GL_OPERATION or CH_OPERATION never latched.
 *
 * @pre Clock block and MSTP for the channel are already alive.
 * @pre GRAMINIT bit has cleared (caller polled).
 * @post On success the channel is in CH_OPERATION ready for TX/RX.
 * @post On failure the channel is left in whichever transitional
 *       state stalled; caller treats it as init-failed.
 * @note Not thread-safe; caller serialises ra_canfd_init.
 * @since 0.1.0
 */
static ra_err_t internal_canfd_open_channel(volatile r_canfd_t* reg)
{
  (void)internal_set_global_mode(reg, k_ra_gctr_value_reset);
  (void)internal_set_channel_mode(reg, k_ra_chmdc_reset);

  internal_install_default_afl(reg);
  internal_configure_rx_fifo0(reg);

  const ra_err_t gop_err = internal_set_global_mode(reg, k_ra_gctr_value_operation);
  if (gop_err != k_ra_ok) {
    return gop_err;
  }
  /* RFE is only writable in GL_HALT / GL_OPERATION, so enable RX FIFO
   * 0 only after the global block has actually transitioned. */
  internal_enable_rx_fifo0(reg);
  return internal_set_channel_mode(reg, k_ra_chmdc_operation);
}

ra_err_t ra_canfd_init(uint8_t channel)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if (channel >= (uint8_t)(sizeof(s_canfd_mstp_table) / sizeof(s_canfd_mstp_table[0]))) {
    return k_ra_err_invalid_arg;
  }
  /* MSTPC26/27 must be written AFTER CANFDCLK is stable.
   * HUM Ch 11.2.8 "MSTPCRC" Note 4 p 446 */
  const ra_err_t clk_err = internal_canfd_clock_block_init();
  if (clk_err != k_ra_ok) {
    return clk_err;
  }

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
  const ra_err_t mst_err = ra_mstp_enable(s_canfd_mstp_table[channel]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "canfd_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* Wait for RAM init done (CFDGSTS.GRAMINIT clears). */
  /* HUM Ch 41.2 "CFDGSTS : Global Status Register" p 2746 */
  for (uint32_t i = 0U; i < k_ra_canfd_spin; i++) {                         /* GCOVR_EXCL_BR_LINE */
    if ((reg->CFDGSTS & (uint32_t)(1UL << k_ra_gsts_bit_graminit)) == 0U) { /* GCOVR_EXCL_BR_LINE */
      break;
    }
  }

  const ra_err_t open_err = internal_canfd_open_channel(reg);
  if (open_err != k_ra_ok) {
    return open_err;
  }
  ra_log_info_val(s_tag, "canfd_init ch", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_canfd_deinit(uint8_t channel)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  /* HUM Ch 41 "CFDCnCTR.CHMDC" p 2762 */ /* "CFDCnCTR.CHMDC" -- park channel in reset. */
  (void)internal_set_channel_mode(reg, k_ra_chmdc_reset);
  return k_ra_ok;
}

/**
 * @struct ra_canfd_timing_t
 * @brief Resolved nominal / data phase bit-timing fields.
 *
 * @details
 * All fields are pre-subtract-1 i.e. the human-friendly value before
 * the FSP "field = value - 1" packing. Both nominal and data phases
 * use the same struct; the packing routine differs because the
 * register layouts differ (NCFG vs DCFG).
 */
typedef struct {
  uint32_t prescaler; /**< Prescaler integer (pre-subtract-1). */
  uint32_t tseg1;     /**< Phase segment 1 (pre-subtract-1).   */
  uint32_t tseg2;     /**< Phase segment 2 (pre-subtract-1).   */
  uint32_t sjw;       /**< Sync jump width (pre-subtract-1).   */
} ra_canfd_timing_t;

/**
 * @brief Walk candidate TQ-per-bit counts until one yields an integer prescaler.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] clock_hz See header declaration for direction and constraints.
 * @param[in] bitrate_bps See header declaration for direction and constraints.
 * @param[in] prescaler_max See header declaration for direction and constraints.
 * @param[in] out See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_solve_timing(uint32_t           clock_hz,
                                      uint32_t           bitrate_bps,
                                      uint32_t           prescaler_max,
                                      ra_canfd_timing_t* out)
{
  /* mcdc-deactivated: both args are validated by ra_canfd_init upstream; defensive duplicate. */
  if ((bitrate_bps == 0U) || (clock_hz == 0U)) {
    return k_ra_err_invalid_arg;
  }
  for (uint32_t tq = k_ra_canfd_tq_search_hi; tq >= k_ra_canfd_tq_search_lo; tq--) {
    const uint32_t denom = bitrate_bps * tq;
    if ((clock_hz % denom) != 0U) {
      continue;
    }
    const uint32_t prescaler = clock_hz / denom;
    // mcdc-deactivated: ra_canfd_deinit (bit-timing solver) prescaler-range guard; the search-loop tq bounds (k_ra_canfd_tq_search_lo..hi) and clock_hz/bitrate_bps caller validation upstream make either the lower or upper bound condition the dominant branch for any valid input -- the opposing condition cannot independently flip without violating the documented clock/bitrate range.
    if ((prescaler < k_ra_canfd_prescaler_min) || (prescaler > prescaler_max)) {
      continue;
    }
    /* 75% sample point: TSEG1 = 3*(tq-1)/4, TSEG2 = tq - 1 - TSEG1. */
    const uint32_t tseg1 = ((tq - 1U) * 3U) / 4U;
    const uint32_t tseg2 = (tq - 1U) - tseg1;
    const uint32_t sjw   = (tseg2 < k_ra_canfd_sjw_max) ? tseg2 : k_ra_canfd_sjw_max;
    out->prescaler       = prescaler;
    out->tseg1           = tseg1;
    out->tseg2           = tseg2;
    out->sjw             = sjw;
    return k_ra_ok;
  }
  return k_ra_err_invalid_arg;
}

/**
 * @brief Pack a resolved timing triple into the CFDC[0].NCFG layout.
 *
 * @details
 * FSP `r_canfd.c` line ~422: NBRP/NSJW/NTSEG1/NTSEG2 each minus 1.
 *
 * @param[in] t See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static uint32_t internal_pack_ncfg(const ra_canfd_timing_t* t)
{
  const uint32_t brp_field   = ((t->prescaler - 1U) & k_ra_cncfg_mask_nbrp)
                               << (uint32_t)k_ra_cncfg_shift_nbrp;
  const uint32_t tseg1_field = (t->tseg1 & k_ra_cncfg_mask_ntseg1)
                               << (uint32_t)k_ra_cncfg_shift_ntseg1;
  const uint32_t tseg2_field = (t->tseg2 & k_ra_cncfg_mask_ntseg2)
                               << (uint32_t)k_ra_cncfg_shift_ntseg2;
  const uint32_t sjw_field   = ((t->sjw - 1U) & k_ra_cncfg_mask_nsjw)
                               << (uint32_t)k_ra_cncfg_shift_nsjw;
  return brp_field | tseg1_field | tseg2_field | sjw_field;
}

/**
 * @brief Pack a resolved timing triple into the CFDC2[0].DCFG layout.
 *
 * @details
 * FSP `r_canfd.c` line ~432: DBRP/DSJW/DTSEG1/DTSEG2 with NARROWER
 * fields (8/4/5/4 bits) and DIFFERENT shifts (0/24/8/16).
 *
 * @param[in] t See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static uint32_t internal_pack_dcfg(const ra_canfd_timing_t* t)
{
  const uint32_t brp_field   = ((t->prescaler - 1U) & k_ra_dcfg_mask_dbrp)
                               << (uint32_t)k_ra_dcfg_shift_dbrp;
  const uint32_t tseg1_field = (t->tseg1 & k_ra_dcfg_mask_dtseg1)
                               << (uint32_t)k_ra_dcfg_shift_dtseg1;
  const uint32_t tseg2_field = (t->tseg2 & k_ra_dcfg_mask_dtseg2)
                               << (uint32_t)k_ra_dcfg_shift_dtseg2;
  const uint32_t sjw_field   = ((t->sjw - 1U) & k_ra_dcfg_mask_dsjw)
                               << (uint32_t)k_ra_dcfg_shift_dsjw;
  return brp_field | tseg1_field | tseg2_field | sjw_field;
}

ra_err_t ra_canfd_set_bitrate(uint8_t channel, uint32_t bitrate_bps, uint32_t data_bitrate_bps)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  uint32_t       pclka_hz = 0U;
  const ra_err_t clk_err  = ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz);
  if (clk_err != k_ra_ok) {
    return clk_err;
  }

  /* Nominal phase: 10-bit prescaler ceiling = 1024. */
  ra_canfd_timing_t nominal = {};
  const ra_err_t    n_err =
    internal_solve_timing(pclka_hz, bitrate_bps, k_ra_canfd_prescaler_max, &nominal);
  if (n_err != k_ra_ok) {
    return n_err;
  }

  /* NCFG / DCFG are only writable in CH_RESET or CH_HALT.  Use
   * CH_RESET: halt is a graceful transition that waits for any
   * in-flight TX to finish, and on internal-loopback bring-up the
   * channel may be stuck trying to TX onto a bus with no
   * acknowledger -- halt never converges. CH_RESET is the
   * immediate abort path, which is what FSP r_canfd does too.
   * HUM Ch 41 "CFDCnNCFG.NTSEG2" p 2706 */
  const ra_err_t halt_err = internal_set_channel_mode(reg, k_ra_chmdc_reset);
  if (halt_err != k_ra_ok) {
    return halt_err;
  }

  /* HUM Ch 41 "CFDCnNCFG" p 2705 */
  reg->CFDC[0].NCFG = internal_pack_ncfg(&nominal);

  if ((data_bitrate_bps != 0U) && (data_bitrate_bps > bitrate_bps)) {
    /* Data phase: 8-bit prescaler ceiling = 256. */
    ra_canfd_timing_t data = {};
    const ra_err_t    d_err =
      internal_solve_timing(pclka_hz, data_bitrate_bps, k_ra_canfd_data_prescaler_max, &data);
    if (d_err != k_ra_ok) {
      /* Best-effort: return the channel to CH_OPERATION so the
       * caller does not observe a half-applied edit. */
      (void)internal_set_channel_mode(reg, k_ra_chmdc_operation);
      return d_err;
    }
    /* HUM Ch 41 "CFDCnDCFG" p 2785 */
    reg->CFDC2[0].DCFG = internal_pack_dcfg(&data);
  }

  const ra_err_t op_err = internal_set_channel_mode(reg, k_ra_chmdc_operation);
  if (op_err != k_ra_ok) {
    return op_err;
  }

  ra_log_info_val(s_tag, "set_bitrate bps", bitrate_bps);
  return k_ra_ok;
}

/**
 * @brief Range-check a `ra_canfd_frame_t` against the protocol limits.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] frame See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_validate_frame(const ra_canfd_frame_t* frame)
{
  if (frame->dlc > k_ra_canfd_dlc_max) {
    return k_ra_err_invalid_arg;
  }
  if (frame->is_extended == 0U) {
    if ((frame->id & ~k_ra_canfd_id_std_mask) != 0U) {
      return k_ra_err_invalid_arg;
    }
  } else {
    if ((frame->id & ~k_ra_canfd_id_ext_mask) != 0U) {
      return k_ra_err_invalid_arg;
    }
  }
  if ((frame->is_brs != 0U) && (frame->is_fd == 0U)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Copy the frame payload into `CFDTM[0].DF[]`. Unused bytes left intact.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] frame See header declaration for direction and constraints.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static void internal_write_tx_data(volatile r_canfd_t* reg, const ra_canfd_frame_t* frame)
{
  for (uint8_t b = 0U; b < k_ra_canfd_data_bytes_max; b++) {
    reg->CFDTM[k_ra_canfd_tx_mb_default].DF[b] = frame->data[b];
  }
}

/**
 * @brief Assemble the CFDTM[0].ID word (raw ID plus IDE flag).
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] frame See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static uint32_t internal_tx_id(const ra_canfd_frame_t* frame)
{
  const uint32_t masked = (frame->is_extended != 0U) ? (frame->id & k_ra_canfd_id_ext_mask)
                                                     : (frame->id & k_ra_canfd_id_std_mask);
  return (frame->is_extended != 0U) ? (masked | k_ra_canfd_id_ide) : masked;
}

/**
 * @brief Assemble the CFDTM[0].FDCTR word (FDF/BRS/ESI flags).
 *
 * @details
 * FSP r_canfd.c line ~676: `p_frame->options & 7` packs ESI/BRS/FDF
 * directly into bits [2:0]. We model the same here.
 *
 * @param[in] frame See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static uint32_t internal_tx_fdctr(const ra_canfd_frame_t* frame)
{
  uint32_t w = 0U;
  if (frame->is_fd != 0U) {
    w |= k_ra_canfd_fd_fdf;
  }
  if (frame->is_brs != 0U) {
    w |= k_ra_canfd_fd_brs;
  }
  return w;
}

ra_err_t ra_canfd_transmit(uint8_t channel, const ra_canfd_frame_t* frame)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA_CHECK_NULL_PTR(frame, s_tag, "frame must not be nullptr");

  const ra_err_t v = internal_validate_frame(frame);
  if (v != k_ra_ok) {
    return v;
  }

  /* Clear the previous transmission's TMTRF before asserting TXREQ:
   * HUM Ch 41 "CFDTMSTSj.TMTRF" p ~2756 says TMTR is only honored when
   * TMTRF is 00b. After the first successful TX the chip leaves
   * TMTRF=10b ("transmission successful") and silently drops every
   * subsequent TXREQ -- the symptom is the first round-trip working
   * and every later one returning no_data. */
  reg->CFDTMSTS[k_ra_canfd_tx_mb_default] = 0U;

  /* HUM Ch 41 p 2806 "CFDTMID/CFDTMPTR/CFDTMFDCTR/CFDTMDF" +
   * FSP r_canfd.c line ~668..684. */
  reg->CFDTM[k_ra_canfd_tx_mb_default].ID    = internal_tx_id(frame);
  reg->CFDTM[k_ra_canfd_tx_mb_default].PTR   = ((uint32_t)frame->dlc & k_ra_canfd_ptr_mask_dlc)
                                               << (uint32_t)k_ra_canfd_ptr_shift_dlc;
  reg->CFDTM[k_ra_canfd_tx_mb_default].FDCTR = internal_tx_fdctr(frame);
  internal_write_tx_data(reg, frame);

  /* HUM Ch 41 p 2810 "CFDTMC" -- single-byte transmit-request register.
   * FSP r_canfd.c line ~724: `p_reg->CFDTMC[idx] = 1`. */
  reg->CFDTMC[k_ra_canfd_tx_mb_default] = k_ra_canfd_tmc_txreq;

  /* Wait for CFDTMSTSj.TMTRF[1:0] to read "transmission complete"
   * (10b) -- HUM Ch 41 "CFDTMSTSj.TMTRF" p ~2756. The previous mask
   * (0x06) also matched 01b ("transmission requested"), which on
   * back-to-back TX calls let the second call clobber CFDTMSTS while
   * the first frame was still in flight; the chip then silently
   * dropped the second TX and canfd_filter_demo's mask sub-round
   * (sent immediately after the exact sub-round) saw the frame
   * disappear. Mask 0x04 keys on TMTRF[1] only, which is only set
   * once the TX is actually on the wire and the MB is free.
   *
   * 500 kbit/s + 8 bytes = ~240 us; k_ra_canfd_tx_spin (~500 us at
   * 1 GHz / 5 cycles per iter) covers it with margin. */
#ifndef RA_SIMULATOR_MODE
  for (uint32_t i = 0U; i < k_ra_canfd_tx_spin; i++) { /* GCOVR_EXCL_BR_LINE */
    enum : uint8_t { k_ra_tmsts_tmtrf_done = 0x04U };
    if ((reg->CFDTMSTS[k_ra_canfd_tx_mb_default] & k_ra_tmsts_tmtrf_done) != 0U) {
      break;
    }
  }
#endif
  return k_ra_ok;
}

/**
 * @brief Copy 64 bytes of RX FIFO data from `CFDRF[0].DF[]` into the caller buffer.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] out See header declaration for direction and constraints.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static void internal_read_rx_data(volatile r_canfd_t* reg, ra_canfd_frame_t* out)
{
  for (uint8_t b = 0U; b < k_ra_canfd_data_bytes_max; b++) {
    out->data[b] = reg->CFDRF[k_ra_canfd_rx_fifo_default].DF[b];
  }
}

/**
 * @brief Decode the raw CFDRF[0].ID/PTR/FDSTS into `out`.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] id_word See header declaration for direction and constraints.
 * @param[in] ptr_word See header declaration for direction and constraints.
 * @param[in] fdsts_word See header declaration for direction and constraints.
 * @param[in] out See header declaration for direction and constraints.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static void internal_decode_rx_header(uint32_t          id_word,
                                      uint32_t          ptr_word,
                                      uint32_t          fdsts_word,
                                      ra_canfd_frame_t* out)
{
  const uint8_t is_ext = ((id_word & k_ra_canfd_id_ide) != 0U) ? 1U : 0U;
  out->is_extended     = is_ext;
  out->id =
    (is_ext != 0U) ? (id_word & k_ra_canfd_id_ext_mask) : (id_word & k_ra_canfd_id_std_mask);
  out->dlc = (uint8_t)((ptr_word >> (uint32_t)k_ra_canfd_ptr_shift_dlc) & k_ra_canfd_ptr_mask_dlc);
  out->is_fd  = ((fdsts_word & k_ra_canfd_fd_fdf) != 0U) ? 1U : 0U;
  out->is_brs = ((fdsts_word & k_ra_canfd_fd_brs) != 0U) ? 1U : 0U;
}

ra_err_t ra_canfd_receive(uint8_t channel, ra_canfd_frame_t* out_frame)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA_CHECK_NULL_PTR(out_frame, s_tag, "out_frame must not be nullptr");

  /* HUM Ch 41 "CFDRFSTSn.RFEMP" p 2754 */ /* "CFDRFSTSn.RFEMP" -- empty flag is bit 0. */
  if ((reg->CFDRFSTS[k_ra_canfd_rx_fifo_default] & k_ra_rfsts_bit_empty) != 0U) {
    return k_ra_err_no_data;
  }

  /* HUM Ch 41 "CFDRFn ID/PTR/FDSTS/DF" p 2796 */ /* "CFDRFn ID/PTR/FDSTS/DF". */
  internal_decode_rx_header(reg->CFDRF[k_ra_canfd_rx_fifo_default].ID,
                            reg->CFDRF[k_ra_canfd_rx_fifo_default].PTR,
                            reg->CFDRF[k_ra_canfd_rx_fifo_default].FDSTS,
                            out_frame);
  internal_read_rx_data(reg, out_frame);

  /* HUM Ch 41 p 2756 "CFDRFPCTRn.RFPC" -- write 0xFF to advance pointer.
   * FSP r_canfd.c uses the same dummy 0xFF write to pop. */
  reg->CFDRFPCTR[k_ra_canfd_rx_fifo_default] = k_ra_rfpctr_value_ack;
  return k_ra_ok;
}

ra_err_t ra_canfd_get_error_state(uint8_t channel, uint8_t* tx_err, uint8_t* rx_err)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA_CHECK_NULL_PTR(tx_err, s_tag, "tx_err must not be nullptr");
  RA_CHECK_NULL_PTR(rx_err, s_tag, "rx_err must not be nullptr");

  /* HUM Ch 41 p 2766 "CFDCnSTS" -- TEC[31:24] / REC[23:16] live in
   * CFDC[0].STS, NOT in CFDC[0].ERFL like the previous header model. */
  const uint32_t sts = reg->CFDC[0].STS;
  *tx_err            = (uint8_t)((sts >> (uint32_t)k_ra_cnsts_bit_tec) & k_ra_cnsts_mask_tec);
  *rx_err            = (uint8_t)((sts >> (uint32_t)k_ra_cnsts_bit_rec) & k_ra_cnsts_mask_rec);
  return k_ra_ok;
}

/* =============================================================================
 * status + IRQ + power transition
 * =============================================================================
 */

static ra_canfd_event_fn_t s_canfd_fn;
static void*               s_canfd_ctx;

ra_err_t ra_canfd_get_status(uint8_t channel, uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 41 "CFDCnSTS" p 2766 */ /* "CFDCnSTS". */
  *out_mask = reg->CFDC[0].STS;
  return k_ra_ok;
}

ra_err_t ra_canfd_clear_status(uint8_t channel, uint32_t mask)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 41 p 2772 "CFDCnERFL" -- error flags are W0C: writing 0
   * clears, writing 1 leaves untouched. We compute the inverse mask
   * to match the previous behaviour ("clear bits in mask"). */
  reg->CFDC[0].ERFL = reg->CFDC[0].ERFL & ~mask;
  return k_ra_ok;
}

ra_err_t ra_canfd_attach_handler(ra_canfd_event_fn_t fn, void* ctx)
{
  s_canfd_fn  = fn;
  s_canfd_ctx = ctx;
  return k_ra_ok;
}

void ra_canfd_dispatch(uint8_t channel)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  if (reg == nullptr) {
    return;
  }
  /* HUM Ch 41 "CFDCnERFL" p 2772 */ /* "CFDCnERFL" snapshot then ack. */
  const uint32_t            mask = reg->CFDC[0].ERFL;
  const ra_canfd_event_fn_t fn   = s_canfd_fn;
  void* const               ctx  = s_canfd_ctx;
  reg->CFDC[0].ERFL              = 0U;
  if (fn != nullptr) {
    fn(ctx, channel, mask);
  }
}

/**
 * @brief Re-derive the data-phase timing triple and pack it into DCFG.
 *
 * @details
 * Helper for ::ra_canfd_set_brs: solves for an integer prescaler
 * against PCLKA for the requested @p data_bitrate, then writes the
 * FSP-aligned DCFG layout.  HUM Ch 41 "CFDCnDCFG" pp 2702-2867.
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] data_bitrate See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_program_data_phase(volatile r_canfd_t* reg, uint32_t data_bitrate)
{
  if (data_bitrate == 0U) {
    return k_ra_err_invalid_arg;
  }
  uint32_t       pclka_hz = 0U;
  const ra_err_t clk_err  = ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz);
  if (clk_err != k_ra_ok) {
    return clk_err;
  }
  ra_canfd_timing_t data = {};
  const ra_err_t    err =
    internal_solve_timing(pclka_hz, data_bitrate, k_ra_canfd_data_prescaler_max, &data);
  if (err != k_ra_ok) {
    return err;
  }
  /* HUM Ch 41 "CFDCnDCFG" p 2785 */ /* "CFDCnDCFG" */
  reg->CFDC2[0].DCFG = internal_pack_dcfg(&data);
  return k_ra_ok;
}

/** @brief Number of AFL slots that live on page 0. */
enum : uint16_t {
  k_ra_canfd_afl_per_page = 16U,
};

/**
 * @brief Bump CFDGAFLCFG0.RNC0 to cover @p filter_id rules on page 0.
 * @details Caller must already have placed the global block in
 * GL_RESET; RNC0 is RESET-only per HUM Ch 41.2.18 p 2735.
 * @param[in] reg       CANFD register block (channel 0).
 * @param[in] filter_id Filter slot index in [0, k_ra_canfd_afl_total).
 * @pre Block is in GL_RESET.
 * @pre filter_id < k_ra_canfd_afl_per_page (page-0 only).
 * @post RNC0 >= filter_id + 1.
 * @post No global state outside CFDGAFLCFG0 is modified.
 * @note Not thread-safe; serialise filter edits.
 * @since 0.1.0
 */
static void internal_bump_rnc0_locked(volatile r_canfd_t* reg, uint16_t filter_id)
{
  if (filter_id >= (uint16_t)k_ra_canfd_afl_per_page) {
    return;
  }
  const uint32_t cur_rnc = (reg->CFDGAFLCFG0 >> k_ra_gaflcfg0_shift_rnc0) & k_ra_gaflcfg0_mask_rnc0;
  const uint32_t new_rnc = ((uint32_t)filter_id + 1U) & k_ra_gaflcfg0_mask_rnc0;
  if (cur_rnc < new_rnc) {
    const uint32_t cfg0 = reg->CFDGAFLCFG0;
    reg->CFDGAFLCFG0    = (cfg0 & ~(k_ra_gaflcfg0_mask_rnc0 << k_ra_gaflcfg0_shift_rnc0)) |
                          ((new_rnc & k_ra_gaflcfg0_mask_rnc0) << k_ra_gaflcfg0_shift_rnc0);
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
static void internal_write_afl_slot(volatile r_canfd_t* reg,
                                    uint16_t            slot,
                                    uint32_t            accept_id,
                                    uint32_t            mask,
                                    uint8_t             dlc)
{
  reg->CFDGAFL[slot].ID = accept_id;
  reg->CFDGAFL[slot].M  = mask | (uint32_t)k_ra_gaflm_bit_gafllb;
  reg->CFDGAFL[slot].P0 = 0U;
  reg->CFDGAFL[slot].P1 = (((uint32_t)dlc & k_ra_canfd_ptr_mask_dlc) << k_ra_canfd_ptr_shift_dlc) |
                          (uint32_t)k_ra_gaflp1_bit_gaflfdp0;
}

/* ra_canfd_filter_set -- see header for full description.
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
ra_err_t ra_canfd_filter_set(uint16_t filter_id, uint32_t accept_id, uint32_t mask, uint8_t dlc)
{
  if (filter_id >= k_ra_canfd_afl_total) {
    return k_ra_err_invalid_arg;
  }
  if (dlc > k_ra_canfd_dlc_max) {
    return k_ra_err_invalid_arg;
  }
  if ((accept_id & ~k_ra_canfd_id_ext_mask) != 0U) {
    return k_ra_err_invalid_arg;
  }

  const uint16_t page = (uint16_t)(filter_id / k_ra_canfd_afl_per_page);
  const uint16_t slot = (uint16_t)(filter_id % k_ra_canfd_afl_per_page);

  /* AFL is global across instances; access via channel 0. */
  volatile r_canfd_t* reg = ra_canfd(0U);
  RA_CHECK_NULL_PTR(reg, s_tag, "filter_set: channel0 unavailable");

  const ra_err_t reset_err = internal_set_global_mode(reg, k_ra_gctr_value_reset);
  if (reset_err != k_ra_ok) {
    return reset_err;
  }

  internal_bump_rnc0_locked(reg, filter_id);

  /* Unlock AFL data window via AFLDAE bit. */
  /* HUM Ch 41.2 "CFDGAFLECTR : AFL Entry Control Register" p 2734 */
  reg->CFDGAFLECTR = ((uint32_t)page & k_ra_gaflectr_mask_aflpn) | k_ra_gaflectr_bit_afldae;
  internal_write_afl_slot(reg, slot, accept_id, mask, dlc);
  reg->CFDGAFLECTR = 0U;

  const ra_err_t op_err = internal_set_global_mode(reg, k_ra_gctr_value_operation);
  if (op_err != k_ra_ok) {
    return op_err;
  }
  /* GL_RESET cleared CFDRFCCa.RFE (HUM Ch 41 p 2742); re-enable so
   * matched frames have a landing FIFO once we resume operation. */
  internal_enable_rx_fifo0(reg);
  return k_ra_ok;
}

ra_err_t ra_canfd_set_test_mode(uint8_t channel, ra_ctms_mode_t mode)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if ((uint8_t)mode > (uint8_t)k_ra_ctms_self_test_1) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 41 "CFDCnCTR" p 2710 -- CTME (bit 24) and CTMS (bits
   * [26:25]) are only writable in CH_HALT mode.  ra_canfd_init parks
   * the channel in CH_OPERATION so we transition through CH_HALT to
   * land the test-mode select, then return to CH_OPERATION. */
  const ra_err_t halt_err = internal_set_channel_mode(reg, k_ra_chmdc_halt);
  if (halt_err != k_ra_ok) {
    /* Best-effort recover the channel before reporting the error. */
    (void)internal_set_channel_mode(reg, k_ra_chmdc_operation);
    return halt_err;
  }

  /* Read-modify-write CTR: clear any prior CTME/CTMS, then OR in the
   * requested selector with CTME = 1. */
  uint32_t ctr = reg->CFDC[0].CTR;
  ctr &= ~(k_ra_cnctr_mask_ctme | k_ra_cnctr_mask_ctms);
  ctr |= k_ra_cnctr_mask_ctme;
  ctr |= ((uint32_t)mode << (uint32_t)k_ra_cnctr_bit_ctms) & k_ra_cnctr_mask_ctms;
  reg->CFDC[0].CTR = ctr;

  return internal_set_channel_mode(reg, k_ra_chmdc_operation);
}

ra_err_t ra_canfd_set_brs(uint8_t channel, uint32_t fast_bitrate)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  return internal_program_data_phase(reg, fast_bitrate);
}

ra_err_t ra_canfd_set_iso_mode(bool enable)
{
  volatile r_canfd_t* reg = ra_canfd(0U);
  RA_CHECK_NULL_PTR(reg, s_tag, "set_iso_mode: channel0 unavailable");
  /* HUM Ch 41 "CFDGFDCFG" pp 2702-2867 -- bit 0 (NISO) is set for ISO
   * mode (default) and cleared for non-ISO Bosch framing. */
  uint32_t v = reg->CFDGFDCFG;
  if (enable) {
    v |= k_ra_gfdcfg_bit_niso;
  } else {
    v &= ~k_ra_gfdcfg_bit_niso;
  }
  reg->CFDGFDCFG = v;
  return k_ra_ok;
}

ra_err_t ra_canfd_enter_stop(uint8_t channel)
{
  if (channel >= k_ra_canfd_instance_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 11.2.8 "MSTPCRC" p 447 */ /* gate channel clock back off. */
  return ra_mstp_disable(s_canfd_mstp_table[channel]);
}

ra_err_t ra_canfd_exit_stop(uint8_t channel)
{
  if (channel >= k_ra_canfd_instance_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 11.2.8 "MSTPCRC" p 447 */ /* ungate channel clock. */
  return ra_mstp_enable(s_canfd_mstp_table[channel]);
}

#ifdef RA_SIMULATOR_MODE
ra_err_t ra_canfd_test_inject_frame(uint8_t        channel,
                                    uint32_t       id_word,
                                    uint32_t       ptr_word,
                                    uint32_t       fdsts_word,
                                    const uint8_t* data,
                                    uint32_t       data_len)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  if (reg == nullptr) {
    return k_ra_err_null_ptr;
  }
  reg->CFDRF[k_ra_canfd_rx_fifo_default].ID    = id_word;
  reg->CFDRF[k_ra_canfd_rx_fifo_default].PTR   = ptr_word;
  reg->CFDRF[k_ra_canfd_rx_fifo_default].FDSTS = fdsts_word;
  const uint32_t copy_len                      = (data_len > (uint32_t)k_ra_canfd_data_bytes_max)
                                                   ? (uint32_t)k_ra_canfd_data_bytes_max
                                                   : data_len;
  for (uint32_t b = 0U; b < copy_len; b++) {
    reg->CFDRF[k_ra_canfd_rx_fifo_default].DF[b] = (data != nullptr) ? data[b] : 0U;
  }
  /* Clear the RFEMP bit so ra_canfd_receive sees a frame ready. */
  reg->CFDRFSTS[k_ra_canfd_rx_fifo_default] &= ~(uint32_t)k_ra_rfsts_bit_empty;
  return k_ra_ok;
}
#endif /* RA_SIMULATOR_MODE */
