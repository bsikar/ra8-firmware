/**
 * @file ra_adc.h
 * @brief Full-featured ADC_B driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * full build-out of the RA8D2 ADC_B peripheral (14-bit
 * SAR). Extends the polling stub with: descriptor-based
 * init, deinit, runtime resolution reconfigure, status + clear,
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

#include "ra_err.h"

/**
 * @enum ra_adc_resolution_t
 * @brief ADCER.ADPRC resolution selector.
 */
typedef enum : uint8_t {
  k_ra_adc_res_12bit = 0U,
  k_ra_adc_res_10bit = 1U,
  k_ra_adc_res_14bit = 2U,
} ra_adc_resolution_t;

/**
 * @enum ra_adc_trigger_t
 * @brief ADCSR trigger mode.
 */
typedef enum : uint8_t {
  k_ra_adc_trig_software = 0U, /**< Software trigger (write ADST). */
  k_ra_adc_trig_external = 1U, /**< External trigger pin. */
  k_ra_adc_trig_elc      = 2U, /**< ELC event routed trigger. */
} ra_adc_trigger_t;

/**
 * @struct ra_adc_cfg_t
 * @brief Configuration descriptor for ``ra_adc_init_configured``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_adc_init_configured`` in
 * ``libs/ra_hal/src/adc.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_adc_resolution_t resolution;    /**< 12 / 10 / 14 bit. */
  ra_adc_trigger_t    trigger;       /**< Trigger source. */
  bool                right_aligned; /**< True -> right-align. */
  bool                scan_mode;     /**< True -> continuous scan. */
} ra_adc_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @enum ra_adc_status_mask_t
 * @brief ADCSR status mask.
 */
typedef enum : uint16_t {
  k_ra_adc_status_none = 0x0000U,
  k_ra_adc_status_busy = 0x8000U, /**< ADCSR.ADST (conversion in flight). */
  k_ra_adc_status_ie   = 0x1000U, /**< ADCSR.ADIE. */
} ra_adc_status_mask_t;

/**
 * @typedef ra_adc_complete_fn_t
 * @brief Conversion-complete callback.
 * @param[in] ctx Caller context.
 * @param[in] result Last conversion result (raw ADDRxx).
 */
typedef void (*ra_adc_complete_fn_t)(void* ctx, uint16_t result);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Legacy init -- 14-bit right-aligned, software trigger.
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_init(void);

/**
 * @brief Initialise ADC_B with a full config descriptor.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` has been called.
 * @post On success ADCSR and ADCER match ``cfg`` and the ADC_B
 * module is powered on via MSTP.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_adc_init_configured(const ra_adc_cfg_t* cfg);

/**
 * @brief Tear down the ADC_B peripheral.
 * @return ``ra_err_t`` error code.
 * @post ADCSR == 0, ADCER == 0, MSTP released.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_adc_deinit(void);

/* =============================================================================
 * Legacy polling API
 * =============================================================================
 */

/**
 * @brief Blocking single-channel sample.
 *
 * @param[in] channel Analog channel index (0..47 on RA8D2 ADC_B).
 * @param[out] out_raw Pointer to receive the 14-bit result right-aligned.
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_read_channel(uint8_t channel, uint16_t* out_raw);

/* =============================================================================
 * Runtime reconfigure
 * =============================================================================
 */

/**
 * @brief Change the ADC resolution at runtime.
 * @param[in] resolution New resolution selection.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_adc_set_resolution(ra_adc_resolution_t resolution);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read the ADC status bits (ADCSR.ADST / ADIE).
 * @param[out] out_mask OR of ``k_ra_adc_status_*``.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_adc_get_status(uint16_t* out_mask);

/**
 * @brief Clear the ADCSR.ADST busy bit (abort in-flight conversion).
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_adc_clear_status(void);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a conversion-complete callback.
 * @param[in] fn Non-NULL callback.
 * @param[in] ctx Context passed to the callback.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_adc_attach_handler(ra_adc_complete_fn_t fn, void* ctx);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put ADC_B into MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_adc_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_adc_exit_stop(void);

/* =============================================================================
 * ISR dispatch
 * =============================================================================
 */

/**
 * @brief Dispatch conversion-complete -- read result + fire callback.
 * @param[in] channel Channel the interrupt was reported on.
 * @since 0.2.0
 */
void ra_adc_dispatch_cnv_end(uint8_t channel);

#ifdef __cplusplus
}
#endif
