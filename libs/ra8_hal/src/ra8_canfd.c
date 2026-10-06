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
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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

static const char* const s_tag = "CANFD";

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
 * @var s_canfd_mstp_table
 * @brief Channel-index -> MSTP id lookup. CANFD0/1 have separate
 * MSTPC bits (HUM Ch 11.2.8 "MSTPCRC", page 447 chapter map row 11).
 */
static const ra8_mstp_t s_canfd_mstp_table[] = {
  k_ra8_mstp_canfd0,
  k_ra8_mstp_canfd1,
};

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
  volatile const uint8_t* const ckcr = ra8_sys_canfdckcr();
  const uint8_t                 mask = (uint8_t)(1U << k_ra8_usbckcr_bit_srdy);
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
 * symptom seen on HIL: ``ra8_canfd_set_test_mode -> priv_ra8_canfd_internal_set_channel_mode
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
  /* GCOVR_EXCL_BR_START -- MSTP HW readback */
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "canfd_init: mstp enable");
  /* GCOVR_EXCL_BR_STOP */

  /* Wait for RAM init done (CFDGSTS.GRAMINIT clears). */
  /* HUM Ch 41.2 "CFDGSTS : Global Status Register" p 2730 */
  for (uint32_t i = 0U; i < k_ra8_canfd_spin; i++) {
    if ((reg->CFDGSTS & (uint32_t)(1UL << k_ra8_gsts_bit_graminit)) == 0U) {
      break;
    }
  }

  const ra8_err_t open_err = priv_ra8_canfd_internal_open_channel(reg);
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
  (void)priv_ra8_canfd_internal_set_channel_mode(reg, k_ra8_chmdc_reset);
  return k_ra8_ok;
}
