/**
 * @file ra8_agt.c
 * @brief Low Power Asynchronous General Purpose Timer driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 AGT block (10 channels total; only
 * AGT0 / AGT1 have dedicated MSTPD bits, the rest share the
 * sub-clock path). Programmes an AGT channel as a free-running
 * 16-bit down-counter clocked from PCLKB. Used as a coarse tick
 * source on boards where SysTick is not desirable. Every register
 * access carries a HUM Ch 24 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_agt.h"

#include <stdint.h>

#include "ra8_agt_regs.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

static const char* s_tag = "AGT";

/**
 * @enum ra8_agt_mstp_limit_t
 * @brief Number of AGT channels that have dedicated MSTPD bits.
 *
 * @details
 * HUM Ch 11.2.9 p 448 only lists MSTPD4 / MSTPD5 for AGT1 / AGT0.
 * The chip lays out 10 AGT channels total but only the first two
 * are ref-counted through ra8_mstp; additional channels inherit
 * their clock from the shared LOCO / sub-clock path.
 */
typedef enum : uint8_t {
  k_ra8_agt_mstp_id_count = 2U, /**< RA8 AGT mstp ID count. */
} ra8_agt_mstp_limit_t;

/**
 * @var s_agt_mstp_table
 * @brief Channel-index -> MSTP id lookup for AGT0 / AGT1.
 */
static const ra8_mstp_t s_agt_mstp_table[k_ra8_agt_mstp_id_count] = {
  k_ra8_mstp_agt0,
  k_ra8_mstp_agt1,
};

/**
 * @var s_agt_mstp_held
 * @brief Per-channel "this driver holds the MSTP reference" latch (AGT0/AGT1).
 *
 * @details
 * AGT0/AGT1 must hold exactly ONE ::ra8_mstp_enable reference for the life of
 * the channel, not one per (re)start. A free-run periodic demo re-arms every
 * underflow via ``ra8_agt_stop`` + ``ra8_agt_start_free_run``; without this latch
 * each restart took a fresh MSTP reference, leaking the ``uint8_t`` refcount in
 * ra8_mstp.c until it saturated at 255 and the 256th start returned
 * ``k_ra8_err_invalid_state`` (issue #68 -- "AGT free-run demos fault after
 * exactly 255 periods"). The latch makes the MSTP acquire idempotent: a start
 * only enables when the channel is not already held, and only deinit /
 * enter-stop release it.
 */
static bool s_agt_mstp_held[k_ra8_agt_mstp_id_count];

/**
 * @brief Acquire the per-channel AGT MSTP reference exactly once (issue #68).
 *
 * @details
 * Idempotent: enables the AGT0/AGT1 module stop reference only when the channel
 * does not already hold it, so a periodic re-arm (stop + start every underflow)
 * never leaks a fresh ::ra8_mstp_enable reference. Channels 2..9 have no
 * dedicated MSTP bit and are a no-op. Early returns keep every decision a
 * single relational test (no compound boolean).
 *
 * @param[in] channel AGT channel index (0..9).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok              Reference held (already, or newly acquired), or
 *                              channel has no dedicated MSTP bit.
 * @retval k_ra8_err_hw_timeout  ``ra8_mstp_enable`` read-back failed (target).
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``channel`` is a valid AGT channel.
 * @post On ::k_ra8_ok for channels 0/1, ``s_agt_mstp_held[channel]`` is true.
 * @post No module-stop write is issued when the reference is already held.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t agt_mstp_acquire(uint8_t channel)
{
  if (channel >= k_ra8_agt_mstp_id_count) {
    return k_ra8_ok;
  }
  if (s_agt_mstp_held[channel]) {
    return k_ra8_ok;
  }
  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D" p 448 */
  const ra8_err_t err = ra8_mstp_enable(s_agt_mstp_table[channel]);
  if (err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE -- target-only read-back path */
    return err;          /* GCOVR_EXCL_LINE                                  */
  }
  s_agt_mstp_held[channel] = true;
  return k_ra8_ok;
}

/**
 * @brief Release the per-channel AGT MSTP reference if held (issue #68).
 *
 * @details
 * Mirror of ::agt_mstp_acquire: drops the AGT0/AGT1 module-stop reference only
 * when the channel currently holds it, so a stop after a never-acquired start
 * (or a double release) cannot underflow the ra8_mstp refcount. Channels 2..9
 * are a no-op. Early returns keep every decision single-condition.
 *
 * @param[in] channel AGT channel index (0..9).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok                 Reference released, or nothing was held.
 * @retval k_ra8_err_invalid_state  ``ra8_mstp_disable`` underflow (defensive).
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``channel`` is a valid AGT channel.
 * @post On ::k_ra8_ok for channels 0/1, ``s_agt_mstp_held[channel]`` is false.
 * @post No module-stop write is issued when no reference is held.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t agt_mstp_release(uint8_t channel)
{
  if (channel >= k_ra8_agt_mstp_id_count) {
    return k_ra8_ok;
  }
  if (!s_agt_mstp_held[channel]) {
    return k_ra8_ok;
  }
  s_agt_mstp_held[channel] = false;
  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D" p 448 */
  return ra8_mstp_disable(s_agt_mstp_table[channel]);
}

[[nodiscard]] ra8_err_t ra8_agt_start_free_run(uint8_t channel, uint16_t reload)
{
  volatile r_agt_regs_t* reg = ra8_agt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  /* Acquire the per-channel MSTP reference once (issue #68). */
  const ra8_err_t mst_err = agt_mstp_acquire(channel);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "agt_start: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  /* HUM Ch 24.2.5 "AGTMR1 : AGT Mode Register 1" p 1168 -- timer mode,
   * PCLKB source. */
  reg->AGTMR1 = 0U;
  /* HUM Ch 24.2.6 "AGTMR2 : AGT Mode Register 2" p 1169 */
  reg->AGTMR2 = 0U;
  /* HUM Ch 24.2.1 "AGT : AGT Counter" p 1165 */
  reg->AGT = reload;
  /* HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167 */
  /* TSTART = 1. */
  reg->AGTCR = 0x01U;

  ra8_log_info_val(s_tag, "start channel", (uint32_t)channel);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_agt_stop(uint8_t channel)
{
  volatile r_agt_regs_t* reg = ra8_agt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  return k_ra8_ok;
}

/* =============================================================================
 * full build-out
 * =============================================================================
 */

static ra8_agt_event_fn_t s_agt_fn;
static void*              s_agt_ctx;

/**
 * @brief Tear down one AGT channel.
 *
 * @details
 * Clears AGTCR (HUM Ch 24.2.4 "AGTCR : AGT Control Register",
 * p 1167) so the counter stops, then drops the matching MSTP
 * reference for AGT0 / AGT1. Channels 2..9 share the LOCO /
 * sub-clock path, so no MSTP work is required for them.
 *
 * @param[in] channel AGT channel index (0..9).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Channel stopped, MSTP released.
 * @retval k_ra8_err_invalid_arg    ``channel`` >= 10 (no register window).
 * @retval k_ra8_err_invalid_state  ``ra8_mstp_disable`` underflow.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_agt_start_free_run`` (or sibling start) was previously
 *      called for ``channel``.
 *
 * @post AGTCR for the channel reads as 0 (counter stopped).
 * @post MSTP reference for AGT0/AGT1 has been decremented when the
 *      channel index falls inside the dedicated MSTP window.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_agt_deinit(uint8_t channel)
{
  volatile r_agt_regs_t* reg = ra8_agt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  return agt_mstp_release(channel);
}

/**
 * @brief Change the AGT reload value at runtime.
 *
 * @details
 * Writes ``reload`` into the AGT counter register (HUM Ch 24.2.1
 * "AGT : AGT Counter", p 1165). Valid only on a stopped channel
 * or while the counter has just been refreshed.
 *
 * @param[in] channel AGT channel index (0..9).
 * @param[in] reload  16-bit reload value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Counter rewritten.
 * @retval k_ra8_err_invalid_arg ``channel`` >= 10.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_agt_start_free_run`` was previously called for ``channel``
 *      or the channel has been initialized by a sibling driver.
 *
 * @post Counter holds ``reload``.
 * @post Channel state (running / stopped) is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_agt_set_reload(uint8_t channel, uint16_t reload)
{
  volatile r_agt_regs_t* reg = ra8_agt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.1 "AGT : AGT Counter" p 1165 */
  reg->AGT = reload;
  return k_ra8_ok;
}

/**
 * @brief Read the AGTCR status register.
 *
 * @details
 * Returns the raw 8-bit AGTCR value (HUM Ch 24.2.4 "AGTCR : AGT
 * Control Register", p 1167) so callers can inspect the TSTART /
 * TCSTF / TUNDF / TEDGF flags without leaking the register layout.
 *
 * @param[in]  channel  AGT channel index (0..9).
 * @param[out] out_mask Receives the AGTCR snapshot.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Status read into ``*out_mask``.
 * @retval k_ra8_err_null_ptr    ``out_mask`` was NULL.
 * @retval k_ra8_err_invalid_arg ``channel`` >= 10.
 *
 * @pre ``out_mask`` non-NULL.
 * @pre Caller has previously powered or started the channel.
 *
 * @post ``*out_mask`` reflects live AGTCR contents.
 * @post No AGT register state is mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_agt_get_status(uint8_t channel, uint8_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_agt_regs_t* reg = ra8_agt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167 */
  *out_mask = reg->AGTCR;
  return k_ra8_ok;
}

/**
 * @brief Attach an AGT event callback (shared across channels).
 *
 * @details
 * Stores ``fn`` / ``ctx`` in module-static storage. The same handler
 * fires for every AGT channel that is dispatched through
 * ``ra8_agt_dispatch``; the dispatcher passes the channel index back
 * to the handler so multi-channel callers can route on it.
 *
 * @param[in] fn  Callback (NULL clears the slot).
 * @param[in] ctx Opaque pointer passed back to ``fn``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Slot updated.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller is happy to share the single global slot with other
 *      AGT users.
 *
 * @post Subsequent ``ra8_agt_dispatch`` calls invoke ``fn(ctx, ch)``.
 * @post Previous callback (if any) is overwritten.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_agt_attach_handler(ra8_agt_event_fn_t fn, void* ctx)
{
  s_agt_fn  = fn;
  s_agt_ctx = ctx;
  return k_ra8_ok;
}

/**
 * @brief Dispatch an AGT event to the registered shared callback.
 *
 * @details
 * Called from the AGT compare-match / underflow ISR (HUM Ch 24
 * "Asynchronous General Purpose Timer", p 1167). Silently no-ops
 * when ``channel`` is out of range or no handler is attached.
 *
 * @param[in] channel AGT channel (0..9) whose ISR fired.
 *
 * @pre Called from ISR context or a host-test driver.
 * @pre ``channel`` < 10.
 *
 * @post Stored callback (if any) has been invoked exactly once.
 * @post No AGT register state is mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_ISR_SAFE
void ra8_agt_dispatch(uint8_t channel)
{
  if (ra8_agt(channel) == nullptr) {
    return;
  }
  const ra8_agt_event_fn_t fn  = s_agt_fn;
  void* const              ctx = s_agt_ctx;
  if (fn != nullptr) {
    fn(ctx, channel);
  }
}

/**
 * @brief Put one AGT channel into MSTP-gated stop.
 *
 * @details
 * Clears AGTCR (HUM Ch 24.2.4 "AGTCR : AGT Control Register",
 * p 1167) so the counter halts, then for AGT0 / AGT1 drops the
 * dedicated MSTPD bit (HUM Ch 11.2.9 "MSTPCRD", p 448) so the
 * peripheral clock can gate.
 *
 * @param[in] channel AGT channel index (0..9).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Channel halted, MSTP gated.
 * @retval k_ra8_err_invalid_arg    ``channel`` >= 10.
 * @retval k_ra8_err_invalid_state  ``ra8_mstp_disable`` underflow on the
 *                                 shared MSTP id.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Channel was previously started via ``ra8_agt_start_free_run``.
 *
 * @post AGTCR for the channel reads as 0.
 * @post For AGT0/AGT1 the MSTPCRD bit has been ref-decremented.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_agt_enter_stop(uint8_t channel)
{
  volatile r_agt_regs_t* reg = ra8_agt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  return agt_mstp_release(channel);
}

/**
 * @brief Exit MSTP-gated stop for one AGT channel.
 *
 * @details
 * Re-asserts the MSTP reference for AGT0 / AGT1 (HUM Ch 11.2.9
 * "MSTPCRD", p 448) so the channel sees its peripheral clock again.
 * Channels 2..9 share the LOCO / sub-clock path and become a no-op
 * here. The caller is expected to reissue a start request next.
 *
 * @param[in] channel AGT channel index (0..9).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              MSTP re-enabled (or no-op for shared).
 * @retval k_ra8_err_invalid_arg ``channel`` >= 10.
 * @retval k_ra8_err_hw_timeout  ``ra8_mstp_enable`` read-back failure.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Channel was previously placed in stop via ``ra8_agt_enter_stop``.
 *
 * @post For AGT0/AGT1 the matching MSTPCRD bit has been ref-incremented.
 * @post Channel is ready to be restarted.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_agt_exit_stop(uint8_t channel)
{
  if (ra8_agt(channel) == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  return agt_mstp_acquire(channel);
}

/* =============================================================================
 * Pulse-output mode -- HUM Ch 24.3.4 p 1177
 * =============================================================================
 */

/**
 * @brief Translate the pulse-mode polarity enum into an AGTIOC mask.
 *
 * @details
 * HUM Ch 24.2.7 "AGTIOC : AGT I/O Control Register" p 1170 +
 * Table 24.3 p 1171 ("Pulse output mode": TEDGSEL=0 -> start high,
 * TEDGSEL=1 -> start low). Always returns TOE = 1 so the AGTOn pin
 * is enabled when pulse mode is armed.
 *
 * @param[in] polarity One of `k_ra8_agt_output_polarity_active_high` or
 *                     `k_ra8_agt_output_polarity_active_low`.
 *
 * @return uint8_t bit-mask suitable for direct write to AGTIOC.
 * @retval 0x04 active-low (TOE only, TEDGSEL cleared).
 * @retval 0x05 active-high (TOE | TEDGSEL).
 *
 * @pre ``polarity`` is one of the two enumerators.
 * @pre Caller will use the result only as an AGTIOC value.
 *
 * @post Returned byte has TOE set (bit 2).
 * @post No global state is touched.
 *
 * @note Not thread-safe; pure ALU helper.
 * @since 0.1.0
 */
static uint8_t agt_pulse_agtioc_value(ra8_agt_output_polarity_t polarity)
{
  uint8_t v = (uint8_t)k_ra8_agt_agtioc_toe_msk;
  if (polarity == k_ra8_agt_output_polarity_active_high) {
    v |= (uint8_t)k_ra8_agt_agtioc_tedgsel_msk;
  }
  return v;
}

/**
 * @brief Build the AGTCMSR mask for a chosen compare-match output.
 *
 * @details
 * HUM Ch 24.2.9 "AGTCMSR" p 1172. Enables compare-match A or B
 * with the matching pin-output enable and "start-high / normal"
 * polarity. ``compare = none`` returns zero so the caller leaves
 * the register cleared.
 *
 * @param[in] compare One of `k_ra8_agt_pulse_compare_none`,
 *                    `k_ra8_agt_pulse_compare_a`, or
 *                    `k_ra8_agt_pulse_compare_b`.
 *
 * @return uint8_t bit-mask suitable for direct write to AGTCMSR.
 * @retval 0x00 none (no compare-match output armed).
 * @retval 0x03 compare A (TCMEA | TOEA).
 * @retval 0x30 compare B (TCMEB | TOEB).
 *
 * @pre ``compare`` is one of the three enumerators.
 * @pre Caller will use the result only as an AGTCMSR value.
 *
 * @post Returned byte has either none / A-side / B-side bits set.
 * @post No global state is touched.
 *
 * @note Not thread-safe; pure ALU helper.
 * @since 0.1.0
 */
static uint8_t agt_pulse_agtcmsr_value(ra8_agt_pulse_compare_t compare)
{
  if (compare == k_ra8_agt_pulse_compare_a) {
    return (uint8_t)(k_ra8_agt_agtcmsr_tcmea_msk | k_ra8_agt_agtcmsr_toea_msk);
  }
  if (compare == k_ra8_agt_pulse_compare_b) {
    return (uint8_t)(k_ra8_agt_agtcmsr_tcmeb_msk | k_ra8_agt_agtcmsr_toeb_msk);
  }
  return 0U;
}

/**
 * @brief Programme the AGTCMA / AGTCMB compare register for pulse mode.
 *
 * @details
 * HUM Ch 24.2.2 "AGTCMA : AGT Compare Match A Register" p 1166 and
 * HUM Ch 24.2.3 "AGTCMB : AGT Compare Match B Register" p 1166. The
 * unselected register is parked at 0xFFFF as per the spec note.
 *
 * @param[in,out] reg     AGT channel window (non-NULL).
 * @param[in]     compare Compare-match selector (see enum).
 * @param[in]     duty    16-bit duty value loaded into the selected
 *                        compare register; the unselected register is
 *                        loaded with 0xFFFF (parked).
 *
 * @pre ``reg`` points at a valid AGT channel window.
 * @pre ``compare`` is one of the three enumerators.
 *
 * @post AGTCMA holds ``duty`` when ``compare == A``, else 0xFFFF.
 * @post AGTCMB holds ``duty`` when ``compare == B``, else 0xFFFF.
 *
 * @note Not thread-safe; ISR-unsafe (touches MMIO).
 * @since 0.1.0
 */
static void agt_pulse_program_compare(volatile r_agt_regs_t*  reg,
                                      ra8_agt_pulse_compare_t compare,
                                      uint16_t                duty)
{
  const uint16_t parked = (uint16_t)k_ra8_agt_compare_parked_value;
  /* HUM Ch 24.2.2 "AGTCMA : AGT Compare Match A Register" p 1166 */
  reg->AGTCMA = (compare == k_ra8_agt_pulse_compare_a) ? duty : parked;
  /* HUM Ch 24.2.3 "AGTCMB : AGT Compare Match B Register" p 1166 */
  reg->AGTCMB = (compare == k_ra8_agt_pulse_compare_b) ? duty : parked;
}

/**
 * @brief Validate the enum members of a pulse-output config block.
 *
 * @details
 * The two pulse-mode enums (`compare` and `polarity`) are passed by
 * the caller and so must be sanity-checked before being mapped onto
 * register-mask values. ``mode`` is informational and is checked at
 * the polling layer in the demo, not here.
 *
 * @param[in] cfg Configuration block (caller has already null-checked).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Both enum fields hold a documented value.
 * @retval k_ra8_err_invalid_arg ``compare`` or ``polarity`` out of range.
 *
 * @pre ``cfg`` is non-NULL.
 * @pre Caller will inspect the return value.
 *
 * @post No global or MMIO state is touched.
 * @post Returns OK only when both enum fields hold a documented value.
 *
 * @note Not thread-safe; pure validator.
 * @since 0.1.0
 */
static ra8_err_t agt_pulse_validate_cfg(const ra8_agt_pulse_cfg_t* cfg)
{
  if ((cfg->compare != k_ra8_agt_pulse_compare_none) &&
      (cfg->compare != k_ra8_agt_pulse_compare_a) && (cfg->compare != k_ra8_agt_pulse_compare_b)) {
    return k_ra8_err_invalid_arg;
  }
  if ((cfg->polarity != k_ra8_agt_output_polarity_active_high) &&
      (cfg->polarity != k_ra8_agt_output_polarity_active_low)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Write the AGT mode / I/O / compare-match registers for pulse
 *        output.
 *
 * @details
 * Programmes everything except AGTCR.TSTART (the caller flips that
 * last). Each register write is preceded by a HUM citation comment.
 *
 * @param[in,out] reg AGT channel window (non-NULL).
 * @param[in]     cfg Validated pulse-output configuration.
 *
 * @pre ``reg`` points at a valid AGT channel window.
 * @pre ``cfg`` has already been validated by `agt_pulse_validate_cfg`.
 *
 * @post AGT is loaded with the period, compare-match regs hold duty
 *       or 0xFFFF.
 * @post Counter is still stopped (AGTCR.TSTART = 0).
 *
 * @note Not thread-safe; ISR-unsafe (touches MMIO).
 * @since 0.1.0
 */
static void agt_pulse_program_registers(volatile r_agt_regs_t* reg, const ra8_agt_pulse_cfg_t* cfg)
{
  /* Stop first. */
  /* HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  /* TMOD=001b (pulse output mode), TCK = PCLKB. */
  /* HUM Ch 24.2.5 "AGTMR1 : AGT Mode Register 1" p 1168 */
  reg->AGTMR1 = (uint8_t)k_ra8_agt_agtmr1_tmod_pulse_output;
  /* HUM Ch 24.2.6 "AGTMR2 : AGT Mode Register 2" p 1169 */
  reg->AGTMR2 = 0U;
  /* TOE = 1, polarity from the config. */
  /* HUM Ch 24.2.7 "AGTIOC : AGT I/O Control Register" p 1170 */
  reg->AGTIOC = agt_pulse_agtioc_value(cfg->polarity);
  /* HUM Ch 24.2.9 "AGTCMSR : AGT Compare Match Function Select" p 1172 */
  reg->AGTCMSR = agt_pulse_agtcmsr_value(cfg->compare);
  agt_pulse_program_compare(reg, cfg->compare, cfg->duty);
  /* HUM Ch 24.2.1 "AGT : AGT Counter" p 1165 */
  reg->AGT = cfg->period;
}

/**
 * @brief Arm one AGT channel in pulse-output / output-compare mode.
 *
 * @details
 * Implementation of the public API declared in `ra8_agt.h`. Validates
 * the config, ref-counts the MSTP bit for AGT0 / AGT1, programmes
 * the mode + I/O + compare-match registers, then sets AGTCR.TSTART
 * to 1. HUM Ch 24.3.4 "Pulse Output Mode" p 1177 is the spec
 * reference; per-register citations sit on each write below.
 *
 * @param[in] channel AGT channel (0..9).
 * @param[in] cfg     Configuration block; must not be NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Channel armed, AGTOn pin toggling.
 * @retval k_ra8_err_null_ptr    ``cfg`` NULL or ``channel`` >= 10.
 * @retval k_ra8_err_invalid_arg ``cfg->compare`` / ``cfg->polarity``
 *                              enum out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Pin function-select has already routed AGTOn etc.
 *
 * @post AGTCR.TSTART reads 1 (counter running).
 * @post AGTMR1.TMOD reads 001b (pulse-output mode).
 *
 * @note Not thread-safe; pair with IRQ masking.
 * @since 0.1.0
 */
ra8_err_t ra8_agt_start_pulse_output(uint8_t channel, const ra8_agt_pulse_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  volatile r_agt_regs_t* reg = ra8_agt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  const ra8_err_t verr = agt_pulse_validate_cfg(cfg);
  RA8_RETURN_ON_ERROR(verr, s_tag, "agt_pulse: cfg validation");

  /* Acquire the per-channel MSTP reference once (issue #68). */
  const ra8_err_t mst_err = agt_mstp_acquire(channel);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "agt_pulse: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  agt_pulse_program_registers(reg, cfg);

  /* TSTART = 1. */
  /* HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = (uint8_t)k_ra8_agt_agtcr_tstart_msk;

  /* One-shot mode is implemented at the polling layer: the caller
   * inspects AGTCR.TUNDF and calls ra8_agt_stop() once it sees the
   * first underflow. The hardware itself only knows continuous
   * pulse output (HUM Ch 24.3.4 p 1177). */
  (void)cfg->mode;

  ra8_log_info_val(s_tag, "pulse start channel", (uint32_t)channel);
  return k_ra8_ok;
}

/* =============================================================================
 * Cascade mode -- HUM Ch 24.1 Table 24.1 p 1164 + Ch 24.2.5 p 1168 note 6
 * =============================================================================
 */

/**
 * @brief Translate the cascade clock enum into an AGTMR1.TCK encoding.
 *
 * @details
 * Maps `ra8_agt_cascade_clk_t` onto the AGTMR1.TCK[2:0] bit field
 * (HUM Ch 24.2.5 p 1168). Only the three PCLKB-derived options are
 * supported -- AGT1 always uses TCK=101b (AGT0 underflow) so its
 * source does not come from this helper.
 *
 * @param[in]  clock   Cascade clock-source enum value.
 * @param[out] out_tck Receives the bit-shifted TCK encoding.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              ``*out_tck`` holds the encoded TCK value.
 * @retval k_ra8_err_invalid_arg ``clock`` was out of range.
 *
 * @pre ``out_tck`` non-NULL.
 * @pre Caller will consume ``*out_tck`` only on a k_ra8_ok return.
 *
 * @post ``*out_tck`` is unchanged when the function returns non-OK.
 * @post No global state is touched.
 *
 * @note Not thread-safe; pure mapping helper.
 * @since 0.1.0
 */
static ra8_err_t agt_cascade_clock_to_tck(ra8_agt_cascade_clk_t clock, uint8_t* out_tck)
{
  switch (clock) {
    case k_ra8_agt_cascade_clk_pclkb:
      *out_tck = (uint8_t)k_ra8_agt_agtmr1_tck_pclkb;
      return k_ra8_ok;
    case k_ra8_agt_cascade_clk_pclkb_div8:
      *out_tck = (uint8_t)k_ra8_agt_agtmr1_tck_pclkb_div8;
      return k_ra8_ok;
    case k_ra8_agt_cascade_clk_pclkb_div2:
      *out_tck = (uint8_t)k_ra8_agt_agtmr1_tck_pclkb_div2;
      return k_ra8_ok;
    default:
      return k_ra8_err_invalid_arg;
  }
}

/**
 * @brief Reset and arm one cascade half in plain timer mode.
 *
 * @details
 * Writes AGTCR=0 (stop), AGTMR1=tmr1 (timer mode + TCK encoding),
 * AGTMR2=0 (no CKS divide), AGT=reload, then leaves TSTART low so
 * the caller can start both halves in the correct order.
 *
 * @param[in,out] reg    AGT channel window (non-NULL).
 * @param[in]     tmr1   Packed AGTMR1 byte (TMOD timer-mode + TCK).
 * @param[in]     reload 16-bit counter reload value.
 *
 * @pre ``reg`` points at a valid AGT channel window.
 * @pre ``tmr1`` encodes TMOD = timer-mode plus a valid TCK value.
 *
 * @post AGT counter is loaded with ``reload``.
 * @post Counter is still stopped (AGTCR.TSTART = 0).
 *
 * @note Not thread-safe; ISR-unsafe (touches MMIO).
 * @since 0.1.0
 */
static void agt_cascade_arm_half(volatile r_agt_regs_t* reg, uint8_t tmr1, uint16_t reload)
{
  /* Stop the channel before touching the mode-control regs. */
  /* HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  /* HUM Ch 24.2.5 "AGTMR1 : AGT Mode Register 1" p 1168 */
  reg->AGTMR1 = tmr1;
  /* HUM Ch 24.2.6 "AGTMR2 : AGT Mode Register 2" p 1169 */
  reg->AGTMR2 = 0U;
  /* No external output in cascade timer mode. */
  /* HUM Ch 24.2.7 "AGTIOC : AGT I/O Control Register" p 1170 */
  reg->AGTIOC = 0U;
  /* No compare-match output. */
  /* HUM Ch 24.2.9 "AGTCMSR : AGT Compare Match Function Select" p 1172 */
  reg->AGTCMSR = 0U;
  /* HUM Ch 24.2.1 "AGT : AGT Counter" p 1165 */
  reg->AGT = reload;
}

/** @brief Shift / mask used to split the cascade 32-bit reload. */
typedef enum : uint32_t {
  k_ra8_agt_cascade_lo_mask  = 0xFFFFU, /**< RA8 AGT cascade lo mask.  */
  k_ra8_agt_cascade_hi_shift = 16U,     /**< RA8 AGT cascade hi shift. */
} ra8_agt_cascade_split_t;

/**
 * @brief Power on both halves of the cascade pair.
 *
 * @details
 * Reference-counts the MSTP bits for AGT0 and AGT1 in order so the
 * peripheral clock arrives before any register write touches them
 * (HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D" p 448).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             Both MSTP bits are ref-incremented.
 * @retval k_ra8_err_hw_timeout `ra8_mstp_enable` read-back failed.
 *
 * @pre IRQs masked or init context.
 * @pre `ra8_mstp_init` has been called previously.
 *
 * @post Both MSTP bits are now ref-incremented on success.
 * @post Caller releases via `ra8_agt_deinit` per channel.
 *
 * @note Not thread-safe; MSTP refcount table is shared with other
 *       drivers.
 * @since 0.1.0
 */
static ra8_err_t agt_cascade_mstp_enable_both(void)
{
  /* One MSTP reference per channel (issue #68): re-arming the cascade every
   * AGT1 underflow must not leak a fresh AGT0/AGT1 reference per period. */
  const ra8_err_t m0 = agt_mstp_acquire((uint8_t)k_ra8_agt_cascade_lo_channel);
  RA8_RETURN_ON_ERROR(m0, s_tag, "cascade: mstp AGT0"); /* GCOVR_EXCL_BR_LINE */
  const ra8_err_t m1 = agt_mstp_acquire((uint8_t)k_ra8_agt_cascade_hi_channel);
  RA8_RETURN_ON_ERROR(m1, s_tag, "cascade: mstp AGT1"); /* GCOVR_EXCL_BR_LINE */
  return k_ra8_ok;
}

/**
 * @brief Programme both AGT halves and start them in cascade order.
 *
 * @details
 * Splits ``reload32`` into the two 16-bit reload values, configures
 * AGT0 in plain timer mode and AGT1 with TCK=101b (AGT0 underflow),
 * then sets TSTART on AGT1 first and AGT0 second so AGT1 is ready to
 * sample on the first cycle (HUM Ch 24.2.5 p 1168 note 6).
 *
 * @param[in,out] lo       AGT0 channel window.
 * @param[in,out] hi       AGT1 channel window.
 * @param[in]     reload32 32-bit virtual reload value.
 * @param[in]     tck_lo   Pre-encoded AGTMR1 TCK byte for the AGT0 half.
 *
 * @pre Both MSTP bits are enabled.
 * @pre ``tck_lo`` encodes the AGTMR1 TCK field for the AGT0 half.
 *
 * @post Both AGTCR.TSTART = 1; cascade is live.
 * @post AGT1 begins counting on the first AGT0 underflow.
 *
 * @note Not thread-safe; ISR-unsafe (touches MMIO).
 * @since 0.1.0
 */
static void agt_cascade_program_and_start(volatile r_agt_regs_t* lo,
                                          volatile r_agt_regs_t* hi,
                                          uint32_t               reload32,
                                          uint8_t                tck_lo)
{
  const uint16_t reload_lo = (uint16_t)(reload32 & (uint32_t)k_ra8_agt_cascade_lo_mask);
  const uint16_t reload_hi = (uint16_t)((reload32 >> (uint32_t)k_ra8_agt_cascade_hi_shift) &
                                        (uint32_t)k_ra8_agt_cascade_lo_mask);

  /* HUM Ch 24.2.5 "AGTMR1 : AGT Mode Register 1" p 1168 */
  const uint8_t tmr1_lo = (uint8_t)((uint8_t)k_ra8_agt_agtmr1_tmod_timer | tck_lo);
  /* HUM Ch 24.2.5 "AGTMR1 : AGT Mode Register 1" p 1168 */
  const uint8_t tmr1_hi =
    (uint8_t)((uint8_t)k_ra8_agt_agtmr1_tmod_timer | (uint8_t)k_ra8_agt_agtmr1_tck_agt0_underflow);

  agt_cascade_arm_half(lo, tmr1_lo, reload_lo);
  agt_cascade_arm_half(hi, tmr1_hi, reload_hi);

  /* Start AGT1 first so it samples the AGT0 underflow on the very */
  /* first cycle. */
  /* HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167 */
  hi->AGTCR = (uint8_t)k_ra8_agt_agtcr_tstart_msk;
  /* HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167 */
  lo->AGTCR = (uint8_t)k_ra8_agt_agtcr_tstart_msk;
}

/**
 * @brief Install the optional cascade-completion callback.
 *
 * @details
 * Copies ``cfg->on_underflow`` and ``cfg->ctx`` into the global
 * AGT dispatch slot when ``cfg->on_underflow`` is non-NULL. NULL is
 * treated as "leave the previously-installed handler in place" so
 * cascade callers do not stomp on a sibling AGT-channel attach.
 *
 * @param[in] cfg Cascade configuration block (non-NULL).
 *
 * @pre ``cfg`` is non-NULL.
 * @pre IRQs masked or single-threaded init context.
 *
 * @post When ``cfg->on_underflow != nullptr`` the global slot is
 *       overwritten.
 * @post When ``cfg->on_underflow == nullptr`` the slot is unchanged.
 *
 * @note Not thread-safe; shared module-static slot.
 * @since 0.1.0
 */
static void agt_cascade_install_callback(const ra8_agt_cascade_cfg_t* cfg)
{
  if (cfg->on_underflow != nullptr) {
    s_agt_fn  = cfg->on_underflow;
    s_agt_ctx = cfg->ctx;
  }
}

/**
 * @brief Fetch and validate both AGT register windows for cascade.
 *
 * @details
 * Cascade only works on the AGT0 / AGT1 pair (HUM Ch 24.1 Table 24.1
 * p 1164). This helper resolves the two channel pointers and rejects
 * any host where the fake window is missing.
 *
 * @param[out] out_lo Receives the AGT0 channel window pointer.
 * @param[out] out_hi Receives the AGT1 channel window pointer.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Both pointers populated.
 * @retval k_ra8_err_null_ptr ``ra8_agt(0)`` or ``ra8_agt(1)`` returned NULL.
 *
 * @pre ``out_lo`` / ``out_hi`` non-NULL.
 * @pre Caller will inspect the return value.
 *
 * @post On OK return both ``*out_lo`` and ``*out_hi`` are non-NULL.
 * @post No MMIO is touched.
 *
 * @note Not thread-safe; pure resolver.
 * @since 0.1.0
 */
static ra8_err_t agt_cascade_resolve_halves(volatile r_agt_regs_t** out_lo,
                                            volatile r_agt_regs_t** out_hi)
{
  *out_lo = ra8_agt((uint8_t)k_ra8_agt_cascade_lo_channel);
  *out_hi = ra8_agt((uint8_t)k_ra8_agt_cascade_hi_channel);
  RA8_CHECK_NULL_PTR(*out_lo, s_tag, "cascade: AGT0 unavailable");
  RA8_CHECK_NULL_PTR(*out_hi, s_tag, "cascade: AGT1 unavailable");
  return k_ra8_ok;
}

/**
 * @brief Cascade AGT0 and AGT1 into a 32-bit virtual down-counter.
 *
 * @details
 * Implementation of the public API declared in `ra8_agt.h`. Validates
 * the clock enum, ref-counts the AGT0 / AGT1 MSTP bits, splits the
 * 32-bit reload into two 16-bit halves, programmes both channels in
 * timer mode (AGT1 with TCK=101b "AGT0 underflow", HUM Ch 24.2.5
 * p 1168 note 6), then starts AGT1 first so it samples the very
 * first AGT0 underflow.
 *
 * @param[in] cfg Cascade configuration block; must not be NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              AGT0 + AGT1 armed as a 32-bit cascade.
 * @retval k_ra8_err_null_ptr    ``cfg`` NULL or AGT0/AGT1 window missing.
 * @retval k_ra8_err_invalid_arg ``cfg->clock`` enum out of range.
 * @retval k_ra8_err_hw_timeout  MSTP enable read-back failed.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller has not separately armed AGT0 or AGT1.
 *
 * @post AGT0.AGTCR.TSTART = 1; AGT1.AGTCR.TSTART = 1.
 * @post Global handler slot points at ``cfg->on_underflow`` if
 *       non-NULL.
 *
 * @note Not thread-safe; pair with IRQ masking.
 * @since 0.1.0
 */
ra8_err_t ra8_agt_start_cascade(const ra8_agt_cascade_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  uint8_t         tck_lo = 0U;
  const ra8_err_t cerr   = agt_cascade_clock_to_tck(cfg->clock, &tck_lo);
  RA8_RETURN_ON_ERROR(cerr, s_tag, "cascade: bad clock enum");

  volatile r_agt_regs_t* lo   = nullptr;
  volatile r_agt_regs_t* hi   = nullptr;
  const ra8_err_t        herr = agt_cascade_resolve_halves(&lo, &hi);
  RA8_RETURN_ON_ERROR(herr, s_tag, "cascade: half resolve");

  const ra8_err_t merr = agt_cascade_mstp_enable_both();
  RA8_RETURN_ON_ERROR(merr, s_tag, "cascade: mstp enable");

  agt_cascade_program_and_start(lo, hi, cfg->reload32, tck_lo);
  agt_cascade_install_callback(cfg);
  ra8_log_info_val(s_tag, "cascade start", cfg->reload32);
  return k_ra8_ok;
}
