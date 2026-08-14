/**
 * @file ra8_board_ek_ra8d2_pdm.h
 * @brief EK-RA8D2 onboard SPH0690 PDM microphone policy.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details Kept separate from the connector-only header so consumers that
 * need board pin constants do not inherit the PDM HAL dependency.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_pdm.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Selects one of the two SPH0690 microphones sharing PDM channel 2. */
typedef enum : uint8_t {
  k_ra8_board_pdm_mic1 = 0U, /**< Rising-edge MIC1 (SELECT tied low).   */
  k_ra8_board_pdm_mic2,      /**< Falling-edge MIC2 (SELECT tied high). */
  k_ra8_board_pdm_mic_count, /**< Number of on-board PDM microphones.   */
} ra8_board_pdm_mic_t;

/** @brief Board-owned PDM mechanism configuration for one microphone. */
typedef struct {
  ra8_pdm_channel_cfg_t pdm;            /**< SPH0690 filter coefficients. */
  uint32_t              sample_rate_hz; /**< Decimated PCM sample rate.   */
  uint8_t               channel;        /**< PDM-IF channel selector.     */
  uint8_t               valid_bits;     /**< Significant PCM bits.        */
} ra8_board_pdm_mic_config_t;

/**
 * @brief Route P812/P502 to PDMCLK2/PDMDAT2.
 * @return Error code.
 * @retval k_ra8_ok Both microphone pins were routed.
 * @retval k_ra8_err_gpio_conflict A pin is already claimed.
 * @retval other Propagated pin-routing error.
 * @pre PDMIFCLK is available from MOCO and `ra8_mstp_init` has run.
 * @post The shared microphone clock/data pins are assigned to PDM-IF.
 * @note Not thread-safe; call once during board initialization.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_pdm_mic_route(void);

/**
 * @brief Return the validated SPH0690 filter policy for one board microphone.
 * @param[in] microphone MIC1 or MIC2 selector.
 * @param[out] out_config Receives channel, rate, width, and HAL coefficients.
 * @return Error code.
 * @retval k_ra8_ok Configuration returned.
 * @retval k_ra8_err_null_ptr `out_config` was `nullptr`.
 * @retval k_ra8_err_invalid_arg `microphone` is out of range.
 * @pre `out_config` points to writable storage.
 * @post On success `out_config` describes 16 kHz, 20-bit mono PCM.
 * @note Thread-safe; returns immutable board policy by value.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_pdm_mic_get_config(ra8_board_pdm_mic_t         microphone,
                                                     ra8_board_pdm_mic_config_t* out_config);

#ifdef __cplusplus
}
#endif
