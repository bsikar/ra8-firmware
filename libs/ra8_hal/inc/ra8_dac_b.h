/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_dac_b.h
 * @brief Full-featured 12-bit DAC_B driver
 * @ingroup grp_hal_analog
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * full build-out of the RA8D2 DAC_B peripheral.
 * Extends the write stub with: descriptor-based init,
 * deinit, runtime reference reconfigure, output-enable controls,
 * interrupt-mode attach / dispatch, power transition.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_dac_b_vref_t
 * @brief VREFH operating-mode selection (DACR2.OFSSEL bit 8).
 *
 * @details
 * Mirrors FSP `dac_b_vrefh_t`: only two values are supported by the
 * RA8D2 DAC_B IP. NORMAL is used when VREFH >= 2.7 V; LOW is used
 * when VREFH < 2.7 V. The driver writes this value left-shifted to
 * DACR2 bit 8 (OFSSEL).
 */
typedef enum : uint8_t {
  k_ra8_dac_b_vref_normal = 0U, /**< OFSSEL=0: VREFH >= 2.7V (default). */
  k_ra8_dac_b_vref_low    = 1U, /**< OFSSEL=1: VREFH < 2.7V.            */
} ra8_dac_b_vref_t;

/**
 * @enum ra8_dac_b_data_format_t
 * @brief Data placement selection (DACR1.DPSEL bit 16).
 *
 * @details
 * Mirrors the FSP `dac_data_format_t` semantics for DAC_B: 0 selects
 * right-justified 12-bit data in the 16-bit DADR; 1 selects
 * left-justified 12-bit data.
 */
typedef enum : uint8_t {
  k_ra8_dac_b_format_right = 0U, /**< DPSEL=0: right-justified (default). */
  k_ra8_dac_b_format_left  = 1U, /**< DPSEL=1: left-justified.            */
} ra8_dac_b_data_format_t;

/**
 * @struct ra8_dac_b_cfg_t
 * @brief Configuration descriptor for ``ra8_dac_b_init_configured``.
 *
 * @details
 * Mirrors FSP `dac_b_extended_cfg_t` plus the per-channel enable
 * flags. cppcheck cannot see tests/ so it flags every field as
 * unused; each member is read in ``ra8_dac_b_init_configured`` in
 * ``libs/ra8_hal/src/ra8_dac_b.c``.
 */
typedef struct {
  ra8_dac_b_vref_t        vref;                    /**< VREFH range (DACR2.OFSSEL).             */
  ra8_dac_b_data_format_t data_format;             /**< Data placement (DACR1.DPSEL).           */
  bool                    internal_output_enabled; /**< Drive internal route (clears DAOUTDIS). */
  bool                    enable_channel0;         /**< Initial DACEN0 state.                   */
  bool                    enable_channel1;         /**< Initial DACEN1 state.                   */
} ra8_dac_b_cfg_t;

/**
 * @typedef ra8_dac_b_update_fn_t
 * @brief DAC update (conversion done) event callback.
 * @param[in] ctx Caller context.
 * @param[in] channel Channel that finished updating.
 */
typedef void (*ra8_dac_b_update_fn_t)(void* ctx, uint8_t channel);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Legacy init -- zero every DAC_B register + enable MSTP.
 * @return `k_ra8_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dac_b_init(void);

/**
 * @brief Initialise DAC_B with a full config descriptor.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra8_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 * @post On success DACR matches ``cfg`` and the DAC_B module
 * is powered on via MSTP.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dac_b_init_configured(const ra8_dac_b_cfg_t* cfg);

/**
 * @brief Tear down the DAC_B peripheral.
 * @return ``ra8_err_t`` error code.
 * @post DACR == 0, MSTP released.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dac_b_deinit(void);

/* =============================================================================
 * Legacy polling API
 * =============================================================================
 */

/**
 * @brief Programme a DAC channel with a 12-bit code.
 * @param[in] channel 0 or 1.
 * @param[in] value Raw 12-bit code (clamped to 0..4095).
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dac_b_write(uint8_t channel, uint16_t value);

/* =============================================================================
 * Runtime reconfigure
 * =============================================================================
 */

/**
 * @brief Change the DAC voltage reference at runtime.
 * @param[in] vref New reference source.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dac_b_set_vref(ra8_dac_b_vref_t vref);

/**
 * @brief Toggle output-enable for one channel at runtime.
 * @param[in] channel 0 or 1.
 * @param[in] enable True -> drive the pin; false -> Hi-Z.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dac_b_set_output_enable(uint8_t channel, bool enable);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read the DACR output-enable / DAE mask.
 * @param[out] out_mask DACR value masked to the control bits.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dac_b_get_status(uint8_t* out_mask);

/**
 * @brief Clear DAE + DAOE bits (disable DAC outputs).
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dac_b_clear_status(void);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a DAC-update callback.
 * @param[in] fn Callback fired on ISR dispatch.
 * @param[in] ctx Context forwarded to the callback.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dac_b_attach_handler(ra8_dac_b_update_fn_t fn, void* ctx);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put DAC_B into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dac_b_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dac_b_exit_stop(void);

/* =============================================================================
 * ISR dispatch
 * =============================================================================
 */

/**
 * @brief Dispatch a DAC-update event -- fire the registered callback.
 * @param[in] channel Channel that completed an update.
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
void ra8_dac_b_dispatch_update(uint8_t channel);

#ifdef __cplusplus
}
#endif
