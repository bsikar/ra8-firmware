/**
 * @file ra8_poeg.c
 * @brief Port Output Enable for GPT (POEG) driver implementation
 *
 * @details
 * new driver. See ``libs/ra8_hal/inc/ra8_poeg.h`` for the
 * public surface and HUM Ch 21 "Port Output Enable for GPT
 * (POEG)" (p 871..877) for register semantics.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_poeg.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_poeg_regs.h"

static const char* s_tag = "POEG";

/**
 * @var s_poeg_mstp_table
 * @brief Per-group MSTP id lookup. Group 0 == POEGA, ... 3 == POEGD.
 */
static const ra8_mstp_t s_poeg_mstp_table[k_ra8_poeg_group_count] = {
  k_ra8_mstp_poeg_a,
  k_ra8_mstp_poeg_b,
  k_ra8_mstp_poeg_c,
  k_ra8_mstp_poeg_d,
};

/**
 * @enum ra8_poeg_bits_t
 * @brief POEGG status/control bit positions and combined masks.
 */
typedef enum : uint32_t {
  k_ra8_poeg_status_all = k_ra8_poeg_status_pidf | k_ra8_poeg_status_iocf | k_ra8_poeg_status_ovrf |
                          k_ra8_poeg_status_ssf | k_ra8_poeg_status_st, /**< RA8 poeg status all. */
} ra8_poeg_bits_t;

/**
 * @struct ra8_poeg_state_t
 * @brief Per-group runtime state.
 */
typedef struct {
  ra8_poeg_event_fn_t fn;  /**< Fn.  */
  void*               ctx; /**< Ctx. */
} ra8_poeg_state_t;

static ra8_poeg_state_t s_poeg_state[k_ra8_poeg_group_count];

/**
 * @brief Translate a public ::ra8_poeg_cfg_t into a POEGG bitfield.
 *
 * @details
 * Folds the four boolean enables in ``cfg`` into the POEGG control bits
 * documented at HUM Ch 21.2.1 "POEGG : POEG Group n Setting Register"
 * (p 872): PIDE (pin), IOCEN (output-disable on output-IOC), OSTEN (OSC
 * stop), and INV (input-polarity invert).
 *
 * @param[in] cfg Non-NULL pointer to a populated config struct.
 *
 * @return 32-bit value ready for assignment to ``POEGG``.
 * @retval 0..k_ra8_poeg_en_all Bitwise-OR of the four control enables.
 *
 * @pre ``cfg`` is non-NULL (caller validates).
 * @pre Caller has serialised access to the POEGG write window.
 *
 * @post Hardware state is unchanged (pure conversion helper).
 * @post Returned value reflects the four ``cfg->enable_*`` booleans.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static uint32_t internal_cfg_to_poegg(const ra8_poeg_cfg_t* cfg)
{
  uint32_t v = 0U;
  if (cfg->enable_pin) {
    v |= k_ra8_poeg_en_pide;
  }
  if (cfg->enable_ioc) {
    v |= k_ra8_poeg_en_iocen;
  }
  if (cfg->enable_osc_stop) {
    v |= k_ra8_poeg_en_osten;
  }
  if (cfg->invert_input) {
    v |= k_ra8_poeg_en_inv;
  }
  return v;
}

/**
 * @brief Bring up one POEG group with the given configuration.
 *
 * @details
 * Opens the MSTP gate for the group (HUM Ch 11.2.9 "MSTPCRD", p 448),
 * then writes ``POEGG`` (HUM Ch 21.2.1, p 872) so the requested
 * trigger sources arm the GPT-output-disable circuitry.
 *
 * @param[in] group POEG group 0..3 (POEGA..POEGD).
 * @param[in] cfg   Non-NULL configuration block.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Group powered, POEGG written.
 * @retval k_ra8_err_null_ptr       ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg    ``group`` >= 4.
 * @retval k_ra8_err_invalid_state  MSTP enable refused.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Companion GPT module power has already been brought up.
 *
 * @post POEGG reflects the bitfield computed from ``cfg``.
 * @post MSTP reference for the group has been incremented.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_poeg_init(uint8_t group, const ra8_poeg_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  volatile r_poeg_regs_t* reg = ra8_poeg(group);
  RA8_CHECK_NULL_PTR(reg, s_tag, "group out of range");

  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 448 */
  const ra8_err_t mst_err = ra8_mstp_enable(s_poeg_mstp_table[group]);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "poeg_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 21.2.1 "POEGG : POEG Group n Setting Register", p 872 */
  reg->POEGG = internal_cfg_to_poegg(cfg);
  ra8_log_info_val(s_tag, "init group", (uint32_t)group);
  return k_ra8_ok;
}

/**
 * @brief Tear down a POEG group and drop its MSTP reference.
 *
 * @details
 * Clears POEGG (HUM Ch 21.2.1, p 872) so all trigger sources are
 * disarmed, drops the per-group callback slot, then releases the
 * MSTP reference (HUM Ch 11.2.9 "MSTPCRD", p 448).
 *
 * @param[in] group POEG group 0..3.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Group disabled, MSTP released.
 * @retval k_ra8_err_invalid_arg    ``group`` >= 4 (no register window).
 *
 * @pre IRQs masked or single-threaded shutdown context.
 * @pre ``ra8_poeg_init`` was previously called for ``group``.
 *
 * @post POEGG reads as 0.
 * @post Per-group callback slot is cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_poeg_deinit(uint8_t group)
{
  volatile r_poeg_regs_t* reg = ra8_poeg(group);
  RA8_CHECK_NULL_PTR(reg, s_tag, "group out of range");

  reg->POEGG              = 0U;
  s_poeg_state[group].fn  = nullptr;
  s_poeg_state[group].ctx = nullptr;
  (void)ra8_mstp_disable(s_poeg_mstp_table[group]);
  return k_ra8_ok;
}

/**
 * @brief Manually fire the software-stop trigger (POEGG.SSF = 1).
 *
 * @details
 * Sets the SSF bit in POEGG (HUM Ch 21.2.1, p 872), which forces the
 * GPT outputs of the matched group to the disabled level. Used for
 * fault-injection tests and emergency stop paths.
 *
 * @param[in] group POEG group 0..3.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 SSF asserted.
 * @retval k_ra8_err_invalid_arg    ``group`` >= 4.
 *
 * @pre ``ra8_poeg_init`` was previously called for ``group``.
 * @pre Caller is prepared for GPT outputs to be disabled.
 *
 * @post POEGG.SSF reads as 1.
 * @post Other POEGG bits are unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_poeg_trigger_stop(uint8_t group)
{
  volatile r_poeg_regs_t* reg = ra8_poeg(group);
  RA8_CHECK_NULL_PTR(reg, s_tag, "group out of range");

  reg->POEGG |= k_ra8_poeg_status_ssf;
  return k_ra8_ok;
}

/**
 * @brief Read the latched POEGG status bits.
 *
 * @details
 * Returns POEGG masked to the documented status set (PIDF, IOCF, OVRF,
 * SSF, ST -- HUM Ch 21.2.1, p 872) so callers can decide whether the
 * fault was a pin event, OVR, OSC stop or software trigger.
 *
 * @param[in]  group    POEG group 0..3.
 * @param[out] out_mask Receives the status snapshot.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Snapshot written into ``*out_mask``.
 * @retval k_ra8_err_null_ptr       ``out_mask`` was NULL.
 * @retval k_ra8_err_invalid_arg    ``group`` >= 4.
 *
 * @pre ``out_mask`` is non-NULL.
 * @pre ``ra8_poeg_init`` was previously called for ``group``.
 *
 * @post ``*out_mask`` reflects POEGG status bits at call time.
 * @post Hardware state is unchanged.
 *
 * @note Read-only; safe under simple races.
 * @since 0.1.0
 */
ra8_err_t ra8_poeg_get_status(uint8_t group, uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_poeg_regs_t* reg = ra8_poeg(group);
  RA8_CHECK_NULL_PTR(reg, s_tag, "group out of range");

  *out_mask = reg->POEGG & k_ra8_poeg_status_all;
  return k_ra8_ok;
}

/**
 * @brief Acknowledge selected POEGG status bits via write-0-to-clear.
 *
 * @details
 * Reads POEGG, then writes back the value with the requested status
 * bits cleared. Per HUM Ch 21.2.1 (p 872) the latched status flags
 * follow write-0-to-clear semantics.
 *
 * @param[in] group POEG group 0..3.
 * @param[in] mask  Bits to clear (any non-status bits are ignored).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Selected bits acknowledged.
 * @retval k_ra8_err_invalid_arg    ``group`` >= 4.
 *
 * @pre ``ra8_poeg_init`` was previously called for ``group``.
 * @pre Caller has either masked IRQs or holds a writer lock.
 *
 * @post POEGG status bits in ``mask`` read as 0.
 * @post POEGG control bits are unchanged.
 *
 * @note Not thread-safe (read-modify-write on POEGG).
 * @since 0.1.0
 */
ra8_err_t ra8_poeg_clear_status(uint8_t group, uint32_t mask)
{
  volatile r_poeg_regs_t* reg = ra8_poeg(group);
  RA8_CHECK_NULL_PTR(reg, s_tag, "group out of range");

  const uint32_t current = reg->POEGG;
  reg->POEGG             = current & ~(mask & k_ra8_poeg_status_all);
  return k_ra8_ok;
}

/**
 * @brief Register the per-group ISR callback (NULL detaches).
 *
 * @details
 * Stores ``(fn, ctx)`` in the per-group slot consulted by
 * ``ra8_poeg_dispatch``. Passing ``fn == NULL`` is the documented
 * detach signal. No POEGG bits are touched.
 *
 * @param[in] group POEG group 0..3.
 * @param[in] fn    Callback function or NULL to detach.
 * @param[in] ctx   Opaque value forwarded verbatim to ``fn``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Slot updated.
 * @retval k_ra8_err_invalid_arg    ``group`` >= 4.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Lifetime of ``ctx`` outlives the next ``ra8_poeg_dispatch`` call.
 *
 * @post Per-group callback slot reflects ``(fn, ctx)``.
 * @post Sibling group slots are unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_poeg_attach_handler(uint8_t group, ra8_poeg_event_fn_t fn, void* ctx)
{
  if (group >= (uint8_t)k_ra8_poeg_group_count) {
    return k_ra8_err_invalid_arg;
  }
  s_poeg_state[group].fn  = fn;
  s_poeg_state[group].ctx = ctx;
  return k_ra8_ok;
}

/**
 * @brief Drop the MSTP reference for one POEG group (low-power gate).
 *
 * @details
 * Clears the matching MSTPCRD bit (HUM Ch 11.2.9, p 448) so the POEG
 * group stops consuming clock during sleep / standby. Group state is
 * preserved by the rest of the SoC (POEGG keeps its value).
 *
 * @param[in] group POEG group 0..3.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 MSTP reference dropped.
 * @retval k_ra8_err_invalid_arg    ``group`` >= 4.
 * @retval k_ra8_err_invalid_state  ``ra8_mstp_disable`` underflow.
 *
 * @pre Group is in a quiescent state (no pending faults to ack).
 * @pre Caller manages MSTP reference balance per-group.
 *
 * @post MSTPCRD bit for the group reads as 1 (stopped).
 * @post Sibling group MSTP state is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_poeg_enter_stop(uint8_t group)
{
  if (group >= (uint8_t)k_ra8_poeg_group_count) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_mstp_disable(s_poeg_mstp_table[group]);
}

/**
 * @brief Re-enable the MSTP gate for one POEG group after stop.
 *
 * @details
 * Re-sets the MSTPCRD bit (HUM Ch 11.2.9, p 448) so the POEG group
 * resumes clocking. POEGG was preserved across the stop interval and
 * is honoured on resume.
 *
 * @param[in] group POEG group 0..3.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 MSTP reference taken; group running.
 * @retval k_ra8_err_invalid_arg    ``group`` >= 4.
 * @retval k_ra8_err_invalid_state  ``ra8_mstp_enable`` over-took limit.
 *
 * @pre ``ra8_poeg_enter_stop`` was previously called for ``group``.
 * @pre Caller manages MSTP reference balance per-group.
 *
 * @post MSTPCRD bit for the group reads as 0 (running).
 * @post POEGG configuration is honoured at the new clock.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_poeg_exit_stop(uint8_t group)
{
  if (group >= (uint8_t)k_ra8_poeg_group_count) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_mstp_enable(s_poeg_mstp_table[group]);
}

/**
 * @brief Snapshot POEGG status and fan out the per-group callback.
 *
 * @details
 * Reads POEGG (HUM Ch 21.2.1, p 872), masks to the documented status
 * bits, and forwards the snapshot to the registered handler. Out-of-
 * range ``group`` and unattached slots are silently dropped so a
 * spurious IRQ is harmless.
 *
 * @param[in] group POEG group 0..3.
 *
 * @pre Called from ISR context or a host-test driver.
 * @pre ``group`` < 4 (out-of-range silently dropped).
 *
 * @post Stored callback (if any) has been invoked exactly once.
 * @post Status latch is left for the caller to clear.
 *
 * @note Not thread-safe; pair with NVIC masking.
 * @since 0.1.0
 */
RA8_ISR_SAFE
void ra8_poeg_dispatch(uint8_t group)
{
  if (group >= (uint8_t)k_ra8_poeg_group_count) {
    return;
  }
  volatile r_poeg_regs_t* reg  = ra8_poeg(group);
  uint32_t                mask = 0U;
  if (reg != nullptr) {
    mask = reg->POEGG & k_ra8_poeg_status_all;
  }
  const ra8_poeg_event_fn_t fn  = s_poeg_state[group].fn;
  void* const               ctx = s_poeg_state[group].ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}
