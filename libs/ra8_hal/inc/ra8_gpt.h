/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_gpt.h
 * @brief Full-featured General PWM Timer (GPT) driver
 * @ingroup grp_hal_timers
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * full build-out of the RA8D2 GPT peripheral. Extends
 * the free-run stub with: descriptor-based init, deinit,
 * runtime period / duty reconfigure, status flags, interrupt
 * attach / dispatch, power transition.
 *
 * API surface:
 *
 * - ``ra8_gpt_init(channel, cfg)`` -- full config + MSTP enable
 * - ``ra8_gpt_deinit(channel)`` -- stop + MSTP release
 * - ``ra8_gpt_start_free_run`` -- legacy shim
 * - ``ra8_gpt_stop`` -- legacy
 * - ``ra8_gpt_read`` -- legacy
 * - ``ra8_gpt_set_period`` -- runtime GTPR/GTPBR change
 * - ``ra8_gpt_set_duty`` -- runtime GTCCR[A/B] change
 * - ``ra8_gpt_get_status / clear_status`` -- GTST overflow/underflow
 * - ``ra8_gpt_attach_handler`` -- IRQ callback
 * - ``ra8_gpt_enter_stop / exit_stop`` -- power transition
 * - ``ra8_gpt_dispatch_ovf / und / ccra / ccrb`` -- ISR entry points
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_dma.h"
#include "ra8_err.h"

/* =============================================================================
 * Types
 * =============================================================================
 */

/**
 * @enum ra8_gpt_mode_t
 * @brief GPT counter-mode selection (GTCR.MD field).
 */
typedef enum : uint8_t {
  k_ra8_gpt_mode_saw_pwm       = 0U, /**< Saw-wave PWM (up-count).     */
  k_ra8_gpt_mode_saw_one_shot  = 1U, /**< Saw-wave one-shot.           */
  k_ra8_gpt_mode_triangle_pwm  = 4U, /**< Triangle-wave PWM symmetric. */
  k_ra8_gpt_mode_triangle_pwm2 = 5U, /**< Triangle-wave PWM 1.         */
  k_ra8_gpt_mode_triangle_pwm3 = 6U, /**< Triangle-wave PWM 2.         */
} ra8_gpt_mode_t;

/**
 * @enum ra8_gpt_prescaler_t
 * @brief GTCR.TPCS clock prescaler.
 */
typedef enum : uint8_t {
  k_ra8_gpt_ps_div_1    = 0U, /**< PCLKD / 1.    */
  k_ra8_gpt_ps_div_4    = 1U, /**< PCLKD / 4.    */
  k_ra8_gpt_ps_div_16   = 2U, /**< PCLKD / 16.   */
  k_ra8_gpt_ps_div_64   = 3U, /**< PCLKD / 64.   */
  k_ra8_gpt_ps_div_256  = 4U, /**< PCLKD / 256.  */
  k_ra8_gpt_ps_div_1024 = 5U, /**< PCLKD / 1024. */
} ra8_gpt_prescaler_t;

/**
 * @struct ra8_gpt_cfg_t
 * @brief Configuration descriptor for ``ra8_gpt_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_gpt_init`` in
 * ``libs/ra8_hal/src/ra8_gpt.c``.
 */
typedef struct {
  ra8_gpt_mode_t      mode;       /**< Counter mode.                */
  ra8_gpt_prescaler_t prescaler;  /**< Clock divider.               */
  uint32_t            period;     /**< GTPR period.                 */
  uint32_t            duty_a;     /**< GTCCRA compare/duty (PWM A). */
  uint32_t            duty_b;     /**< GTCCRB compare/duty (PWM B). */
  bool                auto_start; /**< True -> start after init.    */
} ra8_gpt_cfg_t;

/**
 * @enum ra8_gpt_status_mask_t
 * @brief Bit mask of GPT status flags (GTST register subset).
 *
 * @details
 * Bit positions match the GTST register in HUM Ch 22.2.16
 * "GTST : General PWM Timer Status Register", p 962..964:
 * TCFA = bit 0, TCFB = bit 1, TCFPO = bit 6, TCFPU = bit 7.
 */
typedef enum : uint32_t {
  k_ra8_gpt_status_none      = 0x00000000UL, /**< RA8 GPT status none. */
  k_ra8_gpt_status_ccra      = 0x00000001UL, /**< GTST.TCFA  (bit 0).  */
  k_ra8_gpt_status_ccrb      = 0x00000002UL, /**< GTST.TCFB  (bit 1).  */
  k_ra8_gpt_status_overflow  = 0x00000040UL, /**< GTST.TCFPO (bit 6).  */
  k_ra8_gpt_status_underflow = 0x00000080UL, /**< GTST.TCFPU (bit 7).  */
} ra8_gpt_status_mask_t;

/**
 * @enum ra8_gpt_ccr_sel_t
 * @brief Selector for ``ra8_gpt_set_duty``.
 */
typedef enum : uint8_t {
  k_ra8_gpt_ccr_a = 0U, /**< GTCCRA. */
  k_ra8_gpt_ccr_b = 1U, /**< GTCCRB. */
} ra8_gpt_ccr_sel_t;

/**
 * @typedef ra8_gpt_event_fn_t
 * @brief GPT IRQ event callback signature.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[in] status_mask OR of ``k_ra8_gpt_status_*`` bits.
 */
typedef void (*ra8_gpt_event_fn_t)(void* ctx, uint32_t status_mask);

/* =============================================================================
 * PWM pin configuration (Sweep 3 Task 2 -- mirrors FSP r_gpt PWM pin API)
 * =============================================================================
 */

/**
 * @enum ra8_gpt_pwm_pin_t
 * @brief Selector for the GTIOCnA / GTIOCnB PWM output pins.
 *
 * @details
 * Used by ``ra8_gpt_duty_cycle_set`` and ``ra8_gpt_pwm_pin_configure``.
 * Mirrors FSP ``gpt_io_pin_t::GPT_IO_PIN_GTIOCA`` /
 * ``GPT_IO_PIN_GTIOCB`` from
 * ``fsp/ra/fsp/src/r_gpt/r_gpt.c``.
 *
 * HUM Ch 22.2.13 "GTIOR : General PWM Timer I/O Control Register"
 * p 942..946 -- bit 8 is OAE (output enable A), bit 24 is OBE.
 */
typedef enum : uint8_t {
  k_ra8_gpt_pin_a = 0U, /**< GTIOCnA -- compare register A path. */
  k_ra8_gpt_pin_b = 1U, /**< GTIOCnB -- compare register B path. */
} ra8_gpt_pwm_pin_t;

/**
 * @enum ra8_gpt_pwm_polarity_t
 * @brief Output polarity for ``ra8_gpt_pwm_pin_configure``.
 *
 * @details
 * Active-high writes the GTIOR.GTIOA / GTIOR.GTIOB sub-field with
 * "low at compare match, high at cycle end" (0x9 per HUM Table 22.18,
 * p 944). Active-low writes the inverted pattern (0x6).
 */
typedef enum : uint8_t {
  k_ra8_gpt_pol_active_high = 0U, /**< Output is high while duty < count. */
  k_ra8_gpt_pol_active_low  = 1U, /**< Output is low while duty < count.  */
} ra8_gpt_pwm_polarity_t;

/**
 * @enum ra8_gpt_pwm_stop_level_t
 * @brief Pin level when the timer is stopped.
 *
 * @details Maps to GTIOR.OADFLT / GTIOR.OBDFLT (HUM Ch 22.2.13).
 */
typedef enum : uint8_t {
  k_ra8_gpt_stop_low  = 0U, /**< OnDFLT = 0 -> stop level low.  */
  k_ra8_gpt_stop_high = 1U, /**< OnDFLT = 1 -> stop level high. */
} ra8_gpt_pwm_stop_level_t;

/**
 * @enum ra8_gpt_pwm_disable_t
 * @brief Output-disable-on-POEG-fault behaviour (GTIOR.OADF / OBDF).
 *
 * @details HUM Ch 22.2.13 "GTIOR" Table 22.18, p 944..946.
 */
typedef enum : uint8_t {
  k_ra8_gpt_disable_none      = 0U, /**< POEG fault does not affect pin. */
  k_ra8_gpt_disable_high_z    = 1U, /**< POEG fault -> Hi-Z.             */
  k_ra8_gpt_disable_drive_low = 2U, /**< POEG fault -> drive low.        */
  k_ra8_gpt_disable_drive_hi  = 3U, /**< POEG fault -> drive high.       */
} ra8_gpt_pwm_disable_t;

/**
 * @struct ra8_gpt_pwm_pin_cfg_t
 * @brief Configuration descriptor for ``ra8_gpt_pwm_pin_configure``.
 *
 * @details
 * Holds the per-pin policy programmed into the GTIOR register
 * (HUM Ch 22.2.13). See ``ra8_gpt_pwm_pin_configure`` for the
 * field-by-field semantics. The structure is consumed by
 * ``ra8_gpt_pwm_pin_configure`` in
 * ``libs/ra8_hal/src/ra8_gpt.c``.
 */
typedef struct {
  bool                     output_enable;    /**< OAE / OBE -- enable pin.   */
  ra8_gpt_pwm_polarity_t   polarity;         /**< Active-high vs active-low. */
  ra8_gpt_pwm_stop_level_t stop_level;       /**< OADFLT / OBDFLT.           */
  ra8_gpt_pwm_disable_t    disable_on_fault; /**< OADF / OBDF (POEG).        */
} ra8_gpt_pwm_pin_cfg_t;

/* =============================================================================
 * Three-phase PWM (Sweep 3 Task 2 -- mirrors FSP r_gpt_three_phase)
 * =============================================================================
 */

/**
 * @enum ra8_gpt_three_phase_idx_t
 * @brief Index into ``ra8_gpt_three_phase_cfg_t::channels`` for U/V/W.
 */
typedef enum : uint8_t {
  k_ra8_gpt_three_phase_u     = 0U, /**< U-phase channel slot. */
  k_ra8_gpt_three_phase_v     = 1U, /**< V-phase channel slot. */
  k_ra8_gpt_three_phase_w     = 2U, /**< W-phase channel slot. */
  k_ra8_gpt_three_phase_count = 3U, /**< Number of phases.     */
} ra8_gpt_three_phase_idx_t;

/**
 * @struct ra8_gpt_three_phase_cfg_t
 * @brief Configuration descriptor for ``ra8_gpt_three_phase_open``.
 *
 * @details
 * Mirrors FSP ``three_phase_cfg_t`` from
 * ``fsp/ra/fsp/inc/api/r_three_phase_api.h``. The driver opens the
 * three GPT channels listed in ``channels`` (typically GPT0/GPT1/GPT2)
 * with a shared ``period_counts`` and identical ``base_cfg`` mode and
 * prescaler so they form a phase-synchronized motor inverter. The
 * three channels are armed via the same GTSTR write so they start
 * on the same PCLKD edge.
 *
 * Each member is read inside ``ra8_gpt_three_phase_open`` in
 * ``libs/ra8_hal/src/ra8_gpt.c``.
 */
typedef struct {
  uint8_t             channels[k_ra8_gpt_three_phase_count]; /**< U/V/W ch ids.   */
  ra8_gpt_mode_t      mode;                                  /**< Triangle/saw.   */
  ra8_gpt_prescaler_t prescaler;                             /**< Clock div.      */
  uint32_t            period_counts;                         /**< Shared GTPR.    */
  uint32_t            initial_duty_u;                        /**< Initial duty U. */
  uint32_t            initial_duty_v;                        /**< Initial duty V. */
  uint32_t            initial_duty_w;                        /**< Initial duty W. */
} ra8_gpt_three_phase_cfg_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise a GPT channel with a full config descriptor.
 *
 * @param[in] channel GPT channel (0..13).
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra8_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 * @post On success, GTCR + GTPR + GTCCR are programmed and
 * (if ``cfg->auto_start``) GTSTR is set.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_init(uint8_t channel, const ra8_gpt_cfg_t* cfg);

/**
 * @brief Tear down a channel (stop + MSTP release).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_deinit(uint8_t channel);

/* =============================================================================
 * Legacy polling API
 * =============================================================================
 */

/**
 * @brief Configure a GPT channel as a free-running up-counter.
 *
 * @param[in] channel GPT channel (0..13).
 * @param[in] period Period in timer ticks (write to GTPR).
 * @return `ra8_err_t` error code.
 *
 * @note The counter runs from PCLKD (or a configurable source) with
 * no prescaler. The caller is responsible for choosing a
 * sensible period value given the current PCLKD.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_start_free_run(uint8_t channel, uint32_t period);

/**
 * @brief Stop a GPT channel.
 *
 * @param[in] channel GPT channel (0..13).
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_stop(uint8_t channel);

/**
 * @brief Read the current counter value.
 *
 * @param[in] channel GPT channel (0..13).
 * @param[out] out Receive counter value.
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_read(uint8_t channel, uint32_t* out);

/* =============================================================================
 * Runtime reconfigure
 * =============================================================================
 */

/**
 * @brief Change the GPT period (GTPR / GTPBR) at runtime.
 * @param[in] channel GPT channel.
 * @param[in] period New GTPR value.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_set_period(uint8_t channel, uint32_t period);

/**
 * @brief Change one compare register (GTCCRA / GTCCRB) at runtime.
 * @param[in] channel GPT channel.
 * @param[in] which Which compare register.
 * @param[in] value New compare value.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_set_duty(uint8_t channel, ra8_gpt_ccr_sel_t which, uint32_t value);

/* =============================================================================
 * Sweep 3 Task 2 -- runtime PWM duty / period / counter / dead time
 * =============================================================================
 */

/**
 * @brief Update the GPT period via the shadow buffer (GTPBR).
 *
 * @details
 * Writes ``period_counts`` to GTPBR; on the next overflow the timer
 * loads GTPBR into GTPR. If the counter is currently stopped the
 * value is also written into GTPR immediately and the counter is
 * cleared so the next ``ra8_gpt_set_period`` style call has the same
 * starting point as a fresh ``ra8_gpt_init``. Mirrors FSP
 * ``R_GPT_PeriodSet`` from ``fsp/ra/fsp/src/r_gpt/r_gpt.c``.
 *
 * @param[in] channel       GPT channel 0..13.
 * @param[in] period_counts New period in counter ticks.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Period buffer programmed.
 * @retval k_ra8_err_null_ptr ``channel`` out of range.
 *
 * @pre Channel previously initialized via ``ra8_gpt_init``.
 * @pre IRQs masked or single-threaded init context.
 * @post GTPBR holds ``period_counts``.
 * @post On the next overflow GTPR is reloaded from GTPBR.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_gpt_set_period() Legacy single-shot variant (no buffer).
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 22.2.21 "GTPR : General PWM Timer Cycle Setting Register",
 * p 985..986.
 */
[[nodiscard]] ra8_err_t ra8_gpt_period_set(uint8_t channel, uint32_t period_counts);

/**
 * @brief Update one PWM compare value via the shadow buffer.
 *
 * @details
 * Writes ``compare_counts`` into GTCCRC (for ``k_ra8_gpt_pin_a``) or
 * GTCCRE (for ``k_ra8_gpt_pin_b``). Per FSP ``R_GPT_DutyCycleSet``
 * the buffer registers are swapped into GTCCRA/GTCCRB at the next
 * cycle end, so PWM transitions are glitch-free. The ``GTBER.CCRA``
 * / ``GTBER.CCRB`` single-buffer enable bits are also asserted so
 * the swap actually happens.
 *
 * @param[in] channel        GPT channel 0..13.
 * @param[in] pin            Which output pin -- A or B.
 * @param[in] compare_counts New compare value (must be <= period).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Compare buffer programmed.
 * @retval k_ra8_err_invalid_arg ``pin`` not in ``ra8_gpt_pwm_pin_t``.
 * @retval k_ra8_err_null_ptr    ``channel`` out of range.
 *
 * @pre Channel previously initialized via ``ra8_gpt_init``.
 * @pre Caller has ensured ``compare_counts`` <= GTPR.
 * @post GTCCRC / GTCCRE holds the new value.
 * @post GTBER buffer-enable bit for the selected pin is asserted.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 22.2.20 "GTCCRA..F : General PWM Timer Compare/Capture
 * Registers", p 982..984. Ch 22.2.17 "GTBER : General PWM Timer
 * Buffer Enable Register", p 965..968.
 */
[[nodiscard]] ra8_err_t
ra8_gpt_duty_cycle_set(uint8_t channel, ra8_gpt_pwm_pin_t pin, uint32_t compare_counts);

/**
 * @brief Force GTCNT to ``value`` (timer must be stopped).
 *
 * @details
 * Mirrors FSP ``R_GPT_CounterSet`` -- the underlying register is
 * not safe to write while the counter is running, so the call
 * rejects any channel whose GTCR.CST bit is set.
 *
 * @param[in] channel GPT channel 0..13.
 * @param[in] value   New GTCNT value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Counter updated.
 * @retval k_ra8_err_null_ptr     ``channel`` out of range.
 * @retval k_ra8_err_invalid_state Counter is currently running.
 *
 * @pre Timer counter is stopped (GTCR.CST == 0).
 * @pre IRQs masked or single-threaded init context.
 * @post GTCNT holds ``value``.
 * @post GTCR.CST is unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 22.2.19 "GTCNT : General PWM Timer Counter", p 980..981.
 */
[[nodiscard]] ra8_err_t ra8_gpt_counter_set(uint8_t channel, uint32_t value);

/**
 * @brief Configure GTIOR for one PWM output pin.
 *
 * @details
 * Programmes the polarity, output-enable, stop level, and
 * output-disable-on-POEG-fault behaviour for either GTIOCnA or
 * GTIOCnB. Mirrors the GTIOR-write portion of FSP
 * ``R_GPT_OutputEnable`` / ``R_GPT_OutputDisable`` and the PWM
 * setup logic in ``gpt_hardware_initialize``.
 *
 * @param[in] channel GPT channel 0..13.
 * @param[in] pin     Which output pin -- A or B.
 * @param[in] cfg     Non-NULL configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Pin configured.
 * @retval k_ra8_err_null_ptr    ``channel`` out of range or ``cfg`` NULL.
 * @retval k_ra8_err_invalid_arg ``pin`` not in ``ra8_gpt_pwm_pin_t``.
 *
 * @pre Channel previously initialized via ``ra8_gpt_init``.
 * @pre IRQs masked or single-threaded init context.
 * @post GTIOR fields for the selected pin reflect ``cfg``.
 * @post GTIOR fields for the *other* pin are preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 22.2.13 "GTIOR : General PWM Timer I/O Control Register",
 * p 942..946 -- bit 8 OAE, bit 24 OBE, bit 6 OADFLT, bit 22 OBDFLT,
 * bits [9..10] OADF, bits [25..26] OBDF, bits [0..4] GTIOA[4:0],
 * bits [16..20] GTIOB[4:0].
 */
[[nodiscard]] ra8_err_t
ra8_gpt_pwm_pin_configure(uint8_t channel, ra8_gpt_pwm_pin_t pin, const ra8_gpt_pwm_pin_cfg_t* cfg);

/**
 * @brief Programme the dead-time value pair for complementary PWM.
 *
 * @details
 * Writes GTDVU (rising-edge dead-time) and GTDVD (falling-edge
 * dead-time) and asserts GTDTCR.TDE (dead-time enable) iff at
 * least one of the two values is non-zero. Mirrors the dead-time
 * branch of FSP ``gpt_hardware_initialize``.
 *
 * @param[in] channel    GPT channel 0..13.
 * @param[in] rising_dt  Dead-time count for the rising edge (GTDVU).
 * @param[in] falling_dt Dead-time count for the falling edge (GTDVD).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Dead-time programmed.
 * @retval k_ra8_err_null_ptr ``channel`` out of range.
 *
 * @pre Channel previously initialized via ``ra8_gpt_init``.
 * @pre IRQs masked or single-threaded init context.
 * @post GTDVU == ``rising_dt`` and GTDVD == ``falling_dt``.
 * @post GTDTCR.TDE bit is set iff either input is non-zero.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 22.2.27 "GTDTCR : General PWM Timer Dead Time Control
 * Register", p 998..999. Ch 22.2.28 "GTDVU/GTDVD", p 1000..1001.
 */
[[nodiscard]] ra8_err_t
ra8_gpt_dead_time_set(uint8_t channel, uint32_t rising_dt, uint32_t falling_dt);

/* =============================================================================
 * Three-phase synchronized PWM (FSP r_gpt_three_phase mirror)
 * =============================================================================
 */

/**
 * @brief Open three GPT channels as a phase-synchronized U/V/W triple.
 *
 * @details
 * Calls ``ra8_gpt_init`` on each of the three channels in
 * ``cfg->channels`` with the same mode, prescaler, and period.
 * After all three are configured, the U-channel's GTSTR is written
 * with a bitmask covering all three channel positions so they start
 * on the same PCLKD edge. Mirrors FSP
 * ``R_GPT_THREE_PHASE_Open`` + ``R_GPT_THREE_PHASE_Start``.
 *
 * @param[in] cfg Non-NULL three-phase configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              All three channels opened and started.
 * @retval k_ra8_err_null_ptr    ``cfg`` NULL.
 * @retval k_ra8_err_invalid_arg Any channel id out of range or already open.
 *
 * @pre ``ra8_mstp_init`` has been called.
 * @pre IRQs masked or single-threaded init context.
 * @post All three GPT channels are configured and counting.
 * @post Driver-internal three-phase state is marked open.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 22.2.2 "GTSTR : General PWM Timer Software Start Register",
 * p 901 -- writing multiple CSTRTn bits in one access starts those
 * channels synchronously.
 */
[[nodiscard]] ra8_err_t ra8_gpt_three_phase_open(const ra8_gpt_three_phase_cfg_t* cfg);

/**
 * @brief Atomically update U/V/W compare values via the shadow buffer.
 *
 * @details
 * Writes ``u_duty`` / ``v_duty`` / ``w_duty`` into the GTCCRC and
 * GTCCRE shadow buffers of the three channels recorded by
 * ``ra8_gpt_three_phase_open``. The GTBER buffer-enable bit makes
 * the swap into GTCCRA/B happen at the next cycle end, so all
 * three phases update on the same overflow. Mirrors FSP
 * ``R_GPT_THREE_PHASE_DutyCycleSet``.
 *
 * @param[in] u_duty New U-phase compare value.
 * @param[in] v_duty New V-phase compare value.
 * @param[in] w_duty New W-phase compare value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Compare values queued for swap.
 * @retval k_ra8_err_invalid_state ``ra8_gpt_three_phase_open`` not called.
 * @retval k_ra8_err_invalid_arg  Any duty value > GTPR.
 *
 * @pre ``ra8_gpt_three_phase_open`` returned ``k_ra8_ok``.
 * @pre All three duty values <= the shared period.
 * @post GTCCRC and GTCCRE on each channel hold the new duty.
 * @post Three-phase state is still open.
 *
 * @note Thread safety: not thread-safe. Recommended to call from
 * a high-priority ISR per FSP r_gpt_three_phase guidance.
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 22.2.20 "GTCCRA..F", p 982..984. Ch 22.2.17 "GTBER", p 965.
 */
[[nodiscard]] ra8_err_t
ra8_gpt_three_phase_set_duty(uint32_t u_duty, uint32_t v_duty, uint32_t w_duty);

/**
 * @brief Stop and tear down the three U/V/W channels.
 *
 * @details
 * Stops the three channels via a single GTSTP write and calls
 * ``ra8_gpt_deinit`` on each. Mirrors FSP
 * ``R_GPT_THREE_PHASE_Stop`` + ``R_GPT_THREE_PHASE_Close``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               All three channels closed.
 * @retval k_ra8_err_invalid_state ``ra8_gpt_three_phase_open`` not called.
 *
 * @pre ``ra8_gpt_three_phase_open`` returned ``k_ra8_ok``.
 * @pre IRQs masked or single-threaded shutdown context.
 * @post All three channels are stopped and MSTP-released.
 * @post Three-phase state is marked closed.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_three_phase_close(void);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read GTST overflow / underflow / compare flags.
 * @param[in] channel GPT channel.
 * @param[out] out_mask OR of ``k_ra8_gpt_status_*``.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_get_status(uint8_t channel, uint32_t* out_mask);

/**
 * @brief Clear GTST flag bits (write-0 to clear).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_clear_status(uint8_t channel, uint32_t mask);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a GPT event callback for a channel.
 * @param[in] channel GPT channel.
 * @param[in] fn Callback fired on ISR dispatch.
 * @param[in] ctx Context passed to the callback.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_attach_handler(uint8_t channel, ra8_gpt_event_fn_t fn, void* ctx);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put a channel into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_exit_stop(uint8_t channel);

/* =============================================================================
 * DMA TX / RX
 * =============================================================================
 */

/**
 * @brief Stream a buffer of period values into GPT GTPR via DMA.
 *
 * @details
 * Programmes the ra8_dma substrate to copy ``count`` 32-bit period
 * values from ``periods[]`` into the channel's GTPR register. This
 * is the "sample streaming" DMA TX path from the roadmap
 * note -- the driver pumps a preloaded period sequence into the
 * timer without the CPU touching GTPR each tick.
 *
 * @param[in] channel GPT channel 0..13.
 * @param[in] periods Source array of 32-bit period values.
 * Must outlive the transfer.
 * @param[in] count Number of periods; non-zero.
 * @param[in] on_complete Completion callback. May be NULL.
 * @param[in] ctx Context passed to ``on_complete``.
 * @param[out] out_dma_channel Allocated DMAC channel on success.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer armed.
 * @retval k_ra8_err_null_ptr ``periods`` / ``out_dma_channel`` NULL.
 * @retval k_ra8_err_invalid_arg Channel or ``count`` invalid.
 * @retval k_ra8_err_no_mem All DMAC channels in use.
 * @retval k_ra8_err_hw_error ``ra8_dma_request`` failed.
 *
 * @pre Channel previously initialized via ``ra8_gpt_init``.
 * @pre ``ra8_dma_init`` has been called.
 * @post On success, DMAC channel is armed.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_write_dma(uint8_t               channel,
                                          const uint32_t*       periods,
                                          uint16_t              count,
                                          ra8_dma_complete_fn_t on_complete,
                                          void*                 ctx,
                                          uint8_t*              out_dma_channel);

/**
 * @brief Capture a stream of GTCNT values into a buffer via DMA.
 *
 * @details
 * Programmes the ra8_dma substrate to copy ``count`` 32-bit counter
 * snapshots from the channel's GTCNT register into ``out_counts[]``.
 * This is the "capture streaming" DMA RX path from the
 * roadmap note -- the driver samples GTCNT on each ELC trigger and
 * stores the value, so the host sees the full capture sequence
 * without ISR overhead.
 *
 * @param[in] channel GPT channel 0..13.
 * @param[out] out_counts Destination counter buffer. Must
 * outlive the transfer.
 * @param[in] count Number of samples; non-zero.
 * @param[in] on_complete Completion callback. May be NULL.
 * @param[in] ctx Context passed to ``on_complete``.
 * @param[out] out_dma_channel Allocated DMAC channel on success.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer armed.
 * @retval k_ra8_err_null_ptr ``out_counts`` / ``out_dma_channel`` NULL.
 * @retval k_ra8_err_invalid_arg Channel or ``count`` invalid.
 * @retval k_ra8_err_no_mem All DMAC channels in use.
 * @retval k_ra8_err_hw_error ``ra8_dma_request`` failed.
 *
 * @pre Channel previously initialized via ``ra8_gpt_init``.
 * @pre ``ra8_dma_init`` has been called.
 * @post On success, DMAC channel is armed.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpt_read_dma(uint8_t               channel,
                                         uint32_t*             out_counts,
                                         uint16_t              count,
                                         ra8_dma_complete_fn_t on_complete,
                                         void*                 ctx,
                                         uint8_t*              out_dma_channel);

/* =============================================================================
 * ISR dispatch
 * =============================================================================
 */

/**
 * @brief Dispatch GPT overflow (GTCIV) -- clear + fire callback.
 * @param[in] channel GPT channel.
 * @since 0.1.0
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 */
void ra8_gpt_dispatch_ovf(uint8_t channel);

/**
 * @brief Dispatch GPT underflow (GTCIU) -- clear + fire callback.
 * @param[in] channel GPT channel.
 * @since 0.1.0
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 */
void ra8_gpt_dispatch_und(uint8_t channel);

/**
 * @brief Dispatch GPT compare-match A (GTCIA) -- clear + fire callback.
 * @param[in] channel GPT channel.
 * @since 0.1.0
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 */
void ra8_gpt_dispatch_ccra(uint8_t channel);

/**
 * @brief Dispatch GPT compare-match B (GTCIB) -- clear + fire callback.
 * @param[in] channel GPT channel.
 * @since 0.1.0
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 */
void ra8_gpt_dispatch_ccrb(uint8_t channel);

/*
 * Input capture & external event counting live in ra8_gpt_capture.h, which
 * includes THIS header for its base types. The include is one-directional
 * (this header does NOT include it back) so there is no include cycle;
 * consumers that need the capture / event-count API include ra8_gpt_capture.h.
 */

#ifdef __cplusplus
}
#endif
