/**
 * @file ra8_agt.h
 * @brief Asynchronous General-Purpose Timer (AGT) driver header
 *
 * @details Declares bounded start, stop, counter, and reload operations for the RA8 asynchronous general-purpose timer channels.
 * @ingroup grp_hal_timers
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Start an AGT channel in free-running mode.
 *
 * @param[in] channel AGT channel (0..9).
 * @param[in] reload  16-bit reload value (counter starts here and
 *                    counts down toward zero).
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_agt_start_free_run(uint8_t channel, uint16_t reload);

/**
 * @brief Stop an AGT channel.
 *
 * @param[in] channel AGT channel (0..9).
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_agt_stop(uint8_t channel);

/**
 * @typedef ra8_agt_event_fn_t
 * @brief AGT event callback.
 */
typedef void (*ra8_agt_event_fn_t)(void* ctx, uint8_t channel);

/**
 * @brief Tear down one AGT channel.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_agt_deinit(uint8_t channel);

/**
 * @brief Change the AGT reload value at runtime.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_agt_set_reload(uint8_t channel, uint16_t reload);

/**
 * @brief Read the AGTCR status register.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_agt_get_status(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Attach an AGT event callback (shared across channels).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_agt_attach_handler(ra8_agt_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an AGT event -- fire callback.
 *
 * @details
 * Called from the AGT compare-match / underflow ISR (HUM Ch 24
 * "Asynchronous General Purpose Timer", p 1167) to fan out the
 * registered handler. Silently returns when ``channel`` is out of
 * range or no handler has been attached.
 *
 * @param[in] channel AGT channel (0..9) whose ISR fired.
 *
 * @pre Called from ISR context or a host-test driver.
 * @pre ``channel`` < 10.
 *
 * @post Stored callback (if any) has been invoked exactly once.
 * @post No AGT register state is mutated by this call.
 *
 * @note Not thread-safe; pair with IRQ masking.
 * @since 0.1.0
 */
void ra8_agt_dispatch(uint8_t channel);

/**
 * @brief Put one channel into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_agt_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_agt_exit_stop(uint8_t channel);

/**
 * @enum ra8_agt_pulse_mode_t
 * @brief AGT pulse-output run mode.
 *
 * @details
 * Maps to HUM Ch 24.3.4 "Pulse Output Mode" p 1177. ``continuous``
 * leaves the channel free-running so the AGTOn pin keeps toggling at
 * each underflow. ``one_shot`` lets the driver halt the counter on
 * the first underflow so the host emits exactly one pulse edge before
 * the firmware decides whether to re-arm.
 */
typedef enum : uint8_t {
  k_ra8_agt_pulse_mode_continuous = 0U, /**< Free-run, toggle every underflow. */
  k_ra8_agt_pulse_mode_one_shot   = 1U, /**< Underflow -> stop, single pulse.  */
} ra8_agt_pulse_mode_t;

/**
 * @enum ra8_agt_output_polarity_t
 * @brief Initial level of the AGTOn pulse output.
 *
 * @details
 * Drives the AGTIOC.TEDGSEL bit (HUM Ch 24.2.7 "AGTIOC : AGT I/O
 * Control Register" p 1170, Table 24.3 "AGTIOn pin I/O edge and
 * polarity switching" p 1171). ``active_high`` is the FSP-style
 * "normal output" -- output starts low, toggles high on the first
 * underflow. ``active_low`` is the "inverted output" -- output starts
 * high.
 */
typedef enum : uint8_t {
  k_ra8_agt_output_polarity_active_high = 0U, /**< TEDGSEL = 1, start-low pin.  */
  k_ra8_agt_output_polarity_active_low  = 1U, /**< TEDGSEL = 0, start-high pin. */
} ra8_agt_output_polarity_t;

/**
 * @enum ra8_agt_pulse_compare_t
 * @brief Compare-match output channel selection for pulse mode.
 *
 * @details
 * The AGT block exposes three output pins -- AGTOn (toggle on
 * underflow) plus AGTOAn / AGTOBn (toggle on compare match A or B).
 * This enum picks which compare-match pair the driver should program;
 * ``none`` leaves AGTCMSR cleared and only the AGTOn underflow pulse
 * runs. HUM Ch 24.2.9 "AGTCMSR : AGT Compare Match Function Select
 * Register" p 1172.
 */
typedef enum : uint8_t {
  k_ra8_agt_pulse_compare_none = 0U, /**< No compare-match output.      */
  k_ra8_agt_pulse_compare_a    = 1U, /**< AGTCMA + AGTOAn toggle armed. */
  k_ra8_agt_pulse_compare_b    = 2U, /**< AGTCMB + AGTOBn toggle armed. */
} ra8_agt_pulse_compare_t;

/**
 * @struct ra8_agt_pulse_cfg_t
 * @brief Configuration for `ra8_agt_start_pulse_output`.
 *
 * @details
 * Captures the four orthogonal settings the AGT pulse-output mode
 * needs from a caller. ``period`` is the 16-bit AGT reload (HUM
 * Ch 24.2.1 "AGT : AGT Counter" p 1165); the pin toggles every
 * (period + 1) count-source cycles. ``duty`` programmes the
 * AGTCMA / AGTCMB compare-match register that drives the chosen
 * AGTOAn / AGTOBn pin (HUM Ch 24.2.2 p 1166 / 24.2.3 p 1166).
 * ``mode`` decides whether the channel stops after the first
 * underflow. ``polarity`` selects the start-level of the AGTOn pin
 * via AGTIOC.TEDGSEL. ``compare`` selects which compare-match
 * channel (if any) should be wired to AGTCMSR.
 */
typedef struct {
  uint16_t                  period;   /**< 16-bit reload (AGT register).     */
  uint16_t                  duty;     /**< Compare-match A/B value.          */
  ra8_agt_pulse_mode_t      mode;     /**< continuous or one-shot.           */
  ra8_agt_output_polarity_t polarity; /**< Start-level of AGTOn.             */
  ra8_agt_pulse_compare_t   compare;  /**< Which compare-match pin is armed. */
} ra8_agt_pulse_cfg_t;

/**
 * @brief Start an AGT channel in pulse-output / output-compare mode.
 *
 * @details
 * Configures one AGT channel for HUM Ch 24.3.4 "Pulse Output Mode"
 * (p 1177): TMOD = 001b in AGTMR1, AGTIOC.TOE = 1 (HUM Ch 24.2.7
 * p 1170), AGT counter loaded with ``cfg->period`` (HUM Ch 24.2.1
 * p 1165), AGTCMA/AGTCMB loaded with ``cfg->duty`` when ``cfg->compare``
 * selects compare channel A or B (HUM Ch 24.2.2 / 24.2.3 p 1166).
 * AGTCMSR enables the matching TCMEA/TOEA/TOPOLA (or B-side) bits
 * (HUM Ch 24.2.9 p 1172). Finally AGTCR.TSTART is set to 1
 * (HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167).
 *
 * @param[in] channel AGT channel (0..9).
 * @param[in] cfg     Configuration block; must not be NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Channel armed, AGTOn pin toggling.
 * @retval k_ra8_err_null_ptr    ``cfg`` was NULL or ``channel`` >= 10.
 * @retval k_ra8_err_invalid_arg ``cfg->compare`` enum out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Pin function-select has already routed AGTOn / AGTOAn / AGTOBn
 *      to the chosen package pin via ``ra8_pfs_route_peripheral``.
 *
 * @post AGTCR.TSTART reads 1 (counter is running).
 * @post AGTMR1.TMOD reads 001b (pulse-output mode).
 *
 * @note Not thread-safe; pair with IRQ masking.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_agt_start_pulse_output(uint8_t channel, const ra8_agt_pulse_cfg_t* cfg);

typedef enum : uint8_t {
  k_ra8_agt_cascade_clk_pclkb      = 0U, /**< TCK = 000b. PCLKB direct. */
  k_ra8_agt_cascade_clk_pclkb_div8 = 1U, /**< TCK = 001b. PCLKB / 8.    */
  k_ra8_agt_cascade_clk_pclkb_div2 = 2U, /**< TCK = 011b. PCLKB / 2.    */
} ra8_agt_cascade_clk_t;

/**
 * @struct ra8_agt_cascade_cfg_t
 * @brief Configuration for `ra8_agt_start_cascade`.
 *
 * @details
 * Cascade mode wires AGT0's underflow into AGT1's count source so the
 * pair behaves as a 32-bit virtual down-counter. The low 16 bits live
 * in AGT0 and the high 16 bits live in AGT1; AGT1 underflows once
 * every ``(reload32 / 0x10000 + 1)`` AGT0 underflows. HUM Ch 24.1
 * "Overview" Table 24.1 p 1164 ("Underflow event signal from AGT0"
 * is a valid TCK[2:0] = 101b on AGT1 only -- HUM Ch 24.2.5 p 1168).
 *
 * ``reload32`` is split into ``reload32 & 0xFFFF`` (AGT0) and
 * ``(reload32 >> 16) & 0xFFFF`` (AGT1). ``prescaler`` programmes the
 * AGTMR1.TCK / AGTMR2.CKS pair for the low half; AGT1 is fixed at
 * "count from AGT0 underflow" (TCK = 101b).
 */
typedef struct {
  uint32_t              reload32;     /**< 32-bit virtual reload value.       */
  ra8_agt_cascade_clk_t clock;        /**< Count source for the AGT0 half.    */
  ra8_agt_event_fn_t    on_underflow; /**< Optional: fires on AGT1 underflow. */
  void*                 ctx;          /**< Opaque ctx forwarded to callback.  */
} ra8_agt_cascade_cfg_t;

/**
 * @brief Cascade AGT0 and AGT1 into a 32-bit virtual down-counter.
 *
 * @details
 * Sets up AGT0 in plain timer mode (HUM Ch 24.3.3 "Timer Mode"
 * p 1176) clocked from ``cfg->clock``, then sets up AGT1 in timer
 * mode with TCK[2:0] = 101b "Underflow event signal from AGT0"
 * (HUM Ch 24.2.5 "AGTMR1 : AGT Mode Register 1" p 1168). Both
 * channels are loaded from ``cfg->reload32`` -- low half into
 * AGT0.AGT, high half into AGT1.AGT (HUM Ch 24.2.1 "AGT : AGT
 * Counter" p 1165). If ``cfg->on_underflow`` is non-NULL the global
 * AGT dispatch slot is wired to it so the AGT1 underflow ISR fans
 * out via ``ra8_agt_dispatch``.
 *
 * Start order matters: AGT1's TSTART must be set before AGT0's so
 * AGT1 is ready to sample the AGT0 underflow on the very first cycle.
 * (HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167 -- count
 * starts in sync with the count source.)
 *
 * @param[in] cfg Cascade configuration block; must not be NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              AGT0+AGT1 armed as a 32-bit cascade.
 * @retval k_ra8_err_null_ptr    ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg ``cfg->clock`` enum out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller has not separately armed AGT0 or AGT1 (they will be
 *      reset by this call).
 *
 * @post AGT0.AGTCR.TSTART = 1; AGT1.AGTCR.TSTART = 1.
 * @post Global handler slot points at ``cfg->on_underflow`` (when
 *       non-NULL) so AGT1 underflow events fan out.
 *
 * @note Not thread-safe; pair with IRQ masking.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_agt_start_cascade(const ra8_agt_cascade_cfg_t* cfg);

#ifdef __cplusplus
}
#endif
