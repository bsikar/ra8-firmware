/**
 * @file ra_dac_b.h
 * @brief Full-featured 12-bit DAC_B driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 4.2 full build-out of the RA8D2 DAC_B peripheral.
 * Extends the Wave 0 write stub with: descriptor-based init,
 * deinit, runtime reference reconfigure, output-enable controls,
 * interrupt-mode attach / dispatch, power transition.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_dac_b_regs.h"
#include "ra_err.h"

/**
 * @enum ra_dac_b_vref_t
 * @brief Voltage reference source selection (DAVREFCR.REF bits).
 */
typedef enum : uint8_t {
  k_ra_dac_b_vref_avcc0    = 0U, /**< AVCC0 / AVSS0 reference.    */
  k_ra_dac_b_vref_internal = 1U, /**< Internal 1.5V / 2.0V ref.   */
  k_ra_dac_b_vref_external = 2U, /**< External VREFH / VREFL pin. */
} ra_dac_b_vref_t;

/**
 * @struct ra_dac_b_cfg_t
 * @brief Configuration descriptor for ``ra_dac_b_init_configured``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_dac_b_init_configured`` in
 * ``libs/ra_hal/src/ra_dac_b.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_dac_b_vref_t vref;            /**< Reference voltage source. */
  bool            enable_channel0; /**< Initial DAOE0 state.      */
  bool            enable_channel1; /**< Initial DAOE1 state.      */
  bool            sync_with_adc;   /**< DAADST ADC-sync enable.   */
} ra_dac_b_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_dac_b_update_fn_t
 * @brief DAC update (conversion done) event callback.
 * @param[in] ctx     Caller context.
 * @param[in] channel Channel that finished updating.
 */
typedef void (*ra_dac_b_update_fn_t)(void* ctx, uint8_t channel);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Legacy init -- zero every DAC_B register + enable MSTP.
 * @return `k_ra_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac_b_init(void);

/**
 * @brief Initialise DAC_B with a full config descriptor.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` has been called.
 * @post On success DACR matches ``cfg`` and the DAC_B module
 *       is powered on via MSTP.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_dac_b_init_configured(const ra_dac_b_cfg_t* cfg);

/**
 * @brief Tear down the DAC_B peripheral.
 * @return ``ra_err_t`` error code.
 * @post DACR == 0, MSTP released.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_dac_b_deinit(void);

/* =============================================================================
 * Legacy polling API
 * =============================================================================
 */

/**
 * @brief Programme a DAC channel with a 12-bit code.
 * @param[in] channel 0 or 1.
 * @param[in] value   Raw 12-bit code (clamped to 0..4095).
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac_b_write(uint8_t channel, uint16_t value);

/* =============================================================================
 * Runtime reconfigure
 * =============================================================================
 */

/**
 * @brief Change the DAC voltage reference at runtime.
 * @param[in] vref New reference source.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_dac_b_set_vref(ra_dac_b_vref_t vref);

/**
 * @brief Toggle output-enable for one channel at runtime.
 * @param[in] channel 0 or 1.
 * @param[in] enable  True -> drive the pin; false -> Hi-Z.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_dac_b_set_output_enable(uint8_t channel, bool enable);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read the DACR output-enable / DAE mask.
 * @param[out] out_mask DACR value masked to the control bits.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_dac_b_get_status(uint8_t* out_mask);

/**
 * @brief Clear DAE + DAOE bits (disable DAC outputs).
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_dac_b_clear_status(void);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a DAC-update callback.
 * @param[in] fn  Callback fired on ISR dispatch.
 * @param[in] ctx Context forwarded to the callback.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_dac_b_attach_handler(ra_dac_b_update_fn_t fn, void* ctx);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put DAC_B into MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_dac_b_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_dac_b_exit_stop(void);

/* =============================================================================
 * ISR dispatch
 * =============================================================================
 */

/**
 * @brief Dispatch a DAC-update event -- fire the registered callback.
 * @param[in] channel Channel that completed an update.
 * @since 0.2.0
 */
void ra_dac_b_dispatch_update(uint8_t channel);

#ifdef __cplusplus
}
#endif
