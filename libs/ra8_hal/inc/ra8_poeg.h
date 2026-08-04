/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_poeg.h
 * @brief Port Output Enable for GPT (POEG) driver
 * @ingroup grp_hal_timers
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * new driver for the RA8D2 POEG block, which forces GPT
 * output pins into high-impedance on:
 * - software trigger (``SSF`` bit),
 * - external input on the corresponding POEG pin,
 * - output-level short detection (same signal on both output
 * pins of a complementary PWM pair),
 * - detection of GPT output level mismatch (IOCE).
 *
 * Each of the four POEG groups (POEG0..POEG3) has a single
 * ``POEGG`` register that carries the enable bits, status flags,
 * and (when an interrupt is wired) fires a single event routed
 * through ``ra8_isr_register``.
 *
 * API surface:
 *
 * - ``ra8_poeg_init(group, cfg)`` -- descriptor init + MSTP
 * - ``ra8_poeg_deinit(group)`` -- clear POEGG, release MSTP
 * - ``ra8_poeg_trigger_stop`` -- software trigger via SSF
 * - ``ra8_poeg_get_status / clear`` -- PIDF / IOCF / SSF / ST
 * - ``ra8_poeg_attach_handler`` -- install IRQ callback
 * - ``ra8_poeg_enter_stop / exit_stop`` -- power transition
 * - ``ra8_poeg_dispatch`` -- ISR entry point
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/* =============================================================================
 * Types
 * =============================================================================
 */

/**
 * @enum ra8_poeg_status_mask_t
 * @brief POEGG status / enable bit masks (HUM Ch 21.2.1).
 */
typedef enum : uint32_t {
  k_ra8_poeg_status_none = 0x00000000UL, /**< RA8 poeg status none.    */
  k_ra8_poeg_status_pidf = 0x00000001UL, /**< POEGG.PIDF Port Input.   */
  k_ra8_poeg_status_iocf = 0x00000002UL, /**< POEGG.IOCF Output Short. */
  k_ra8_poeg_status_ovrf = 0x00000004UL, /**< POEGG.OSTPF Osc Stop.    */
  k_ra8_poeg_status_ssf  = 0x00000008UL, /**< POEGG.SSF Software.      */
  k_ra8_poeg_status_st   = 0x00010000UL, /**< POEGG.ST state.          */
} ra8_poeg_status_mask_t;

/**
 * @enum ra8_poeg_enable_mask_t
 * @brief POEGG enable bit fields.
 */
typedef enum : uint32_t {
  k_ra8_poeg_en_pide  = 0x00000100UL, /**< POEGG.PIDE input detect. */
  k_ra8_poeg_en_iocen = 0x00000200UL, /**< POEGG.IOCE output short. */
  k_ra8_poeg_en_osten = 0x00000400UL, /**< POEGG.OSTPE osc stop.    */
  k_ra8_poeg_en_inv   = 0x00001000UL, /**< POEGG.INV input invert.  */
} ra8_poeg_enable_mask_t;

/**
 * @struct ra8_poeg_cfg_t
 * @brief Configuration descriptor for ``ra8_poeg_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_poeg_init`` in
 * ``libs/ra8_hal/src/ra8_poeg.c``.
 */
typedef struct {
  bool enable_pin;      /**< True -> enable external PIDE input. */
  bool enable_ioc;      /**< True -> enable output short detect. */
  bool enable_osc_stop; /**< True -> enable osc stop detect.     */
  bool invert_input;    /**< True -> invert PIDE input polarity. */
} ra8_poeg_cfg_t;

/**
 * @typedef ra8_poeg_event_fn_t
 * @brief POEG IRQ callback.
 * @param[in] ctx Caller context.
 * @param[in] status_mask OR of ``k_ra8_poeg_status_*`` bits latched.
 */
typedef void (*ra8_poeg_event_fn_t)(void* ctx, uint32_t status_mask);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise a POEG group with a configuration descriptor.
 * @param[in] group POEG group 0..3.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra8_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 * @post POEGG bits match ``cfg`` and the MSTP reference is held.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_poeg_init(uint8_t group, const ra8_poeg_cfg_t* cfg);

/**
 * @brief Tear down a POEG group.
 * @param[in] group POEG group 0..3.
 * @return ``ra8_err_t`` error code.
 * @post POEGG == 0, MSTP released.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_poeg_deinit(uint8_t group);

/* =============================================================================
 * Runtime controls
 * =============================================================================
 */

/**
 * @brief Trigger an emergency stop via POEGG.SSF (software).
 * @param[in] group POEG group 0..3.
 * @return ``ra8_err_t`` error code.
 *
 * @post POEGG.ST is set and GPT outputs for this group are in
 * high-impedance until ``ra8_poeg_clear_status`` is called
 * with ``k_ra8_poeg_status_ssf``.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_poeg_trigger_stop(uint8_t group);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read POEGG status flag bits.
 * @param[in] group POEG group.
 * @param[out] out_mask OR of ``k_ra8_poeg_status_*``.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_poeg_get_status(uint8_t group, uint32_t* out_mask);

/**
 * @brief Clear one or more POEGG status flag bits.
 * @param[in] group POEG group.
 * @param[in] mask Mask of flags to clear.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_poeg_clear_status(uint8_t group, uint32_t mask);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a POEG event callback.
 * @param[in] group POEG group.
 * @param[in] fn Callback fired on ISR dispatch.
 * @param[in] ctx Context passed to callback.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_poeg_attach_handler(uint8_t group, ra8_poeg_event_fn_t fn, void* ctx);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put a group into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_poeg_enter_stop(uint8_t group);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_poeg_exit_stop(uint8_t group);

/* =============================================================================
 * ISR dispatch
 * =============================================================================
 */

/**
 * @brief Dispatch POEG event -- snapshot status + fire callback.
 *
 * @details
 * Called from the POEG group-N ISR (HUM Ch 28 "Port Output Enable
 * for GPT (POEG)", p 1335) to snapshot ``POEGGn.PIDF | OIDF | IOCF``
 * and invoke the registered handler. Out-of-range ``group`` and
 * unattached slots are silently ignored so a spurious IRQ is harmless.
 *
 * @param[in] group POEG group 0..3.
 *
 * @pre Called from ISR context or a host-test driver.
 * @pre ``group`` < 4.
 *
 * @post Stored callback (if any) has been invoked exactly once.
 * @post POEGGn status latch is left for the caller to clear.
 *
 * @note Not thread-safe; pair with NVIC masking.
 * @since 0.1.0
 */
void ra8_poeg_dispatch(uint8_t group);

#ifdef __cplusplus
}
#endif
