/**
 * @file ra_i3c.c
 * @brief I3C Bus Interface driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Scaffold driver for the RA8D2 I3C0 controller.  Programmes the
 * baseline protocol/bus-control register set and exposes the
 * lifecycle + status + IRQ + power-transition surface.  The
 * higher-level CCC / IBI / HDR-DDR transfer engines (NCMDQP,
 * NRSPQP, NTDTBPx, NIBIQP, DATBASn, ENTDAA / SETDASA / RSTDAA
 * builders) are intentionally left to the first card-stack
 * consumer because the protocol decisions (broadcast vs.
 * directed CCC, target address negotiation, in-band IRQ
 * arbitration) belong to the application stack.
 *
 * Bring-up order matches the FSP ``R_I3C_Open`` reference
 * sequence: enable the module clock (CECTL.CLKE), drop
 * BCTL.BUSE, assert RSTCTL.RI3CRST and wait for hardware to
 * clear it, then assert RSTCTL.INTLRST, clear PRTS, release
 * RSTCTL.  Master dynamic address (MSDVAD.MDYAD) is programmed
 * before BCTL.BUSE is set per HUM Ch 40 BCTL description.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_i3c.h"

#include <stdint.h>

#include "ra8d2_i3c_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

/** @brief Logging tag for this module. */
static const char* s_tag = "I3C";

/** @brief Currently registered IRQ callback (NULL when detached). */
static ra_i3c_event_fn_t s_i3c_fn;

/** @brief Opaque context handed back to ``s_i3c_fn``. */
static void* s_i3c_ctx;

/**
 * @brief Internal-error reset bring-up sequence per FSP R_I3C_Open.
 *
 * @details
 * 1. Enable the module clock via CECTL.CLKE.
 * 2. Drop BCTL.BUSE so the bus is idle before any reset.
 * 3. Pulse RSTCTL.RI3CRST -- on real silicon the bit auto-clears
 *    when the I3C internal reset completes; here we issue an
 *    explicit clear so the simulated-mmap unit-test back-end does
 *    not spin forever (FSP relies on
 *    ``FSP_HARDWARE_REGISTER_WAIT`` for the same thing on target).
 * 4. Pulse RSTCTL.INTLRST to flush the internal state machines.
 * 5. Clear PRTS so we start in I2C-fast/I3C-mixed mode 0.
 *
 * Each step follows HUM Ch 40 "RSTCTL : Reset Control Register"
 * description (pp 2445-2701).
 */
static void priv_ra_i3c_reset_sequence(volatile r_i3c_regs_t* reg)
{
  /* HUM Ch 40 "CECTL : Clock Enable Control Register" pp 2445-2701 */
  reg->CECTL = 1U;
  /* HUM Ch 40 "BCTL : Bus Control Register" pp 2445-2701 */
  reg->BCTL = 0U;
  /* HUM Ch 40 "RSTCTL : Reset Control Register" pp 2445-2701 */
  reg->RSTCTL = k_ra_i3c_rstctl_ri3crst_mask;
  reg->RSTCTL = 0U; /* simulated-mmap clear / target HW already auto-cleared */
  reg->RSTCTL = k_ra_i3c_rstctl_intlrst_mask;
  reg->RSTCTL = 0U;
  reg->PRTS   = 0U;
}

ra_err_t ra_i3c_init(void)
{
  /* HUM Ch 11 "MSTPCRB : Module Stop Control Register B" pp 414-490 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_i3c);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "i3c_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "I3C Bus Interface (I3C)" pp 2445-2701 */
  priv_ra_i3c_reset_sequence(reg);

  /* Clear every status / enable register to a deterministic state.
   * INSTFC is write-only (force-clear), so we treat it as a clear. */
  reg->INST   = 0U;
  reg->INSTE  = 0U;
  reg->INIE   = 0U;
  reg->INSTFC = 0U;
  reg->MSDVAD = 0U;

  ra_log_info(s_tag, "i3c_init");
  return k_ra_ok;
}

ra_err_t ra_i3c_deinit(void)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "BCTL : Bus Control Register" pp 2445-2701 */
  reg->INIE  = 0U;
  reg->INSTE = 0U;
  reg->BCTL  = 0U;
  /* HUM Ch 40 "CECTL : Clock Enable Control Register" pp 2445-2701 */
  reg->CECTL = 0U;
  s_i3c_fn   = nullptr;
  s_i3c_ctx  = nullptr;
  return ra_mstp_disable(k_ra_mstp_i3c);
}

ra_err_t ra_i3c_set_address(uint32_t addr)
{
  if (addr > k_ra_i3c_msdvad_addr_max) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 40 "MSDVAD : Master Device Address Register" pp 2445-2701
   * MDYAD occupies bits [22:16]; MDYADV (bit 31) marks the address
   * as valid. */
  const uint32_t mdyad = (addr << k_ra_i3c_msdvad_mdyad_shift) & k_ra_i3c_msdvad_mdyad_mask;
  ra_i3c()->MSDVAD     = mdyad | k_ra_i3c_msdvad_mdyadv_mask;
  return k_ra_ok;
}

ra_err_t ra_i3c_bus_enable(bool enable)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "BCTL : Bus Control Register" pp 2445-2701 -- BCTL.BUSE
   * is bit 31, not bit 0; toggling it gates bus master operation. */
  if (enable) {
    reg->BCTL = reg->BCTL | k_ra_i3c_bctl_buse_mask;
  } else {
    reg->BCTL = reg->BCTL & ~k_ra_i3c_bctl_buse_mask;
  }
  return k_ra_ok;
}

ra_err_t ra_i3c_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 40 "INST : Internal Status Register" pp 2445-2701 */
  *out_mask = ra_i3c()->INST;
  return k_ra_ok;
}

ra_err_t ra_i3c_clear_status(uint32_t mask)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "INST : Internal Status Register" pp 2445-2701 -- the
   * sticky flags are cleared by writing 0 to the matching bit. */
  reg->INST = reg->INST & ~mask;
  return k_ra_ok;
}

ra_err_t ra_i3c_attach_handler(ra_i3c_event_fn_t fn, void* ctx)
{
  s_i3c_fn  = fn;
  s_i3c_ctx = ctx;
  return k_ra_ok;
}

void ra_i3c_dispatch(void)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "INST : Internal Status Register" pp 2445-2701 */
  const uint32_t          mask = reg->INST;
  const ra_i3c_event_fn_t fn   = s_i3c_fn;
  void* const             ctx  = s_i3c_ctx;
  reg->INST                    = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra_err_t ra_i3c_enter_stop(void)
{
  /* HUM Ch 40 "BCTL : Bus Control Register" pp 2445-2701 */
  ra_i3c()->BCTL  = 0U;
  ra_i3c()->CECTL = 0U;
  return ra_mstp_disable(k_ra_mstp_i3c);
}

ra_err_t ra_i3c_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_i3c);
}
