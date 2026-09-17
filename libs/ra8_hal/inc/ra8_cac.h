/**
 * @file ra8_cac.h
 * @brief Clock Accuracy Check driver
 *
 * @details Declares configuration and status access for hardware clock-accuracy measurement against caller-supplied cycle limits.
 * @ingroup grp_hal_system
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
 * @brief Programme CAC with upper + lower bounds on measured cycles.
 *
 * @param[in] upper Upper limit count.
 * @param[in] lower Lower limit count.
 * @return `k_ra8_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cac_init(uint16_t upper, uint16_t lower);

/**
 * @brief Kick off a CAC measurement and wait for completion.
 *
 * @param[out] out_count Counter value captured on completion.
 * @return `k_ra8_ok` if the measurement completed inside the
 *         window, `k_ra8_err_hw_timeout` if CAC did not finish.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cac_measure(uint16_t* out_count);

/**
 * @enum ra8_cac_status_mask_t
 * @brief CASTR status-flag bit masks (HUM Ch 10.2.5 "CASTR" p 424).
 *
 * @details
 * CASTR layout (cross-verified against FSP `R_CAC_Type.CASTR_b` and CMSIS
 * `R_CAC_CASTR_*_Pos` in `R7KA8D2KF_core0.h`):
 *  - bit 0 = FERRF (Frequency Error Flag)
 *  - bit 1 = MENDF (Measurement End Flag)
 *  - bit 2 = OVFF  (Counter Overflow Flag)
 *
 * To clear a flag, write 1 to the matching `*CL` bit in CAICR shifted up
 * by `k_ra8_cac_castr_to_caicr_shift` (= 4): FERRFCL=bit4, MENDFCL=bit5,
 * OVFFCL=bit6 (HUM Ch 10.2.4 "CAICR" p 423).
 */
typedef enum : uint8_t {
  k_ra8_cac_status_none  = 0x00U, /**< RA8 cac status none.            */
  k_ra8_cac_status_ferrf = 0x01U, /**< Frequency error  (CASTR bit 0). */
  k_ra8_cac_status_mendf = 0x02U, /**< Measurement end  (CASTR bit 1). */
  k_ra8_cac_status_ovff  = 0x04U, /**< Counter overflow (CASTR bit 2). */
} ra8_cac_status_mask_t;

/**
 * @typedef ra8_cac_event_fn_t
 * @brief CAC event callback.
 */
typedef void (*ra8_cac_event_fn_t)(void* ctx, uint8_t status_mask);

/**
 * @brief Tear down the CAC (disable + MSTP release).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cac_deinit(void);

/**
 * @brief Read the CASTR flag bits.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cac_get_status(uint8_t* out_mask);

/**
 * @brief Clear the CASTR flag bits via CAICR.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cac_clear_status(uint8_t mask);

/**
 * @brief Attach a callback for CAC events.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cac_attach_handler(ra8_cac_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch CAC events -- snapshot + fire callback.
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
void ra8_cac_dispatch(void);

/**
 * @brief Put CAC into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cac_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cac_exit_stop(void);

#ifdef __cplusplus
}
#endif
