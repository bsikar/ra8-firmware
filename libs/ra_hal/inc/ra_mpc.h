/**
 * @file ra_mpc.h
 * @brief Multi-function Pin Controller (MPC) facade
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * substrate. Wraps the RA8D2 ``PmnPFS`` register array
 * (HUM Ch 20 "I/O Ports", p 837) behind a small set of
 * operation-named helpers:
 *
 * - ``ra_mpc_route_peripheral`` -- map a pin to a PSEL function.
 * - ``ra_mpc_set_gpio`` -- reset a pin to plain GPIO.
 * - ``ra_mpc_set_analog`` -- put a pin in analog-input mode.
 * - ``ra_mpc_set_irq`` -- mark a pin as an external IRQ input.
 * - ``ra_mpc_set_pull`` -- toggle pull-up.
 * - ``ra_mpc_set_open_drain`` -- toggle N-channel open-drain output.
 *
 * Drivers never touch ``PmnPFS`` directly. The substrate handles
 * the PWPR unlock / lock sequence, the "write zero first" reset,
 * and the pin-validator registration so two drivers cannot claim
 * the same pin without being told about it.
 *
 * ## Why wrap PFS in yet another layer?
 *
 * The existing ``ra8d2_pfs_regs.h`` header is low-level: a driver
 * must know the full 32-bit bit layout, remember to unlock PWPR,
 * and coordinate direction / pull / PSEL fields. ``ra_mpc_*``
 * exposes one call per high-level intent and takes care of the
 * bits.
 *
 * will route the whole facade through an NSC veneer so
 * Non-Secure drivers can keep claiming pins but only via the
 * single auditable entry point.
 *
 * ## Threading
 *
 * Single-threaded init context only.
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
#include "ra_port_constants.h"

/* =============================================================================
 * Types
 * =============================================================================
 */

/**
 * @enum ra_mpc_psel_t
 * @brief Peripheral function select values for the PMR/PSEL path.
 *
 * @details
 * Values come from HUM Ch 20 table "Pin function select" (section
 * 20.2.x). Only the ones the firmware currently uses are listed;
 * the enum is extended as new drivers come online.
 */
typedef enum : uint8_t {
  k_ra_mpc_psel_gpio     = 0x00U, /**< 00000: plain GPIO (PMR = 0). */
  k_ra_mpc_psel_agt      = 0x01U, /**< 00001: AGT timer I/O. */
  k_ra_mpc_psel_gpt0     = 0x02U, /**< 00010: GPT 0..3. */
  k_ra_mpc_psel_gpt1     = 0x03U, /**< 00011: GPT 0..3 alt. */
  k_ra_mpc_psel_sci0     = 0x04U, /**< 00100: SCI 0..9. */
  k_ra_mpc_psel_sci1     = 0x05U, /**< 00101: SCI 0..9 alt. */
  k_ra_mpc_psel_spi      = 0x06U, /**< 00110: SPI. */
  k_ra_mpc_psel_iic      = 0x07U, /**< 00111: IIC. */
  k_ra_mpc_psel_canfd    = 0x10U, /**< 10000: CANFD. */
  k_ra_mpc_psel_usbfs    = 0x13U, /**< 10011: USBFS. */
  k_ra_mpc_psel_usbhs    = 0x14U, /**< 10100: USBHS. */
  k_ra_mpc_psel_qspi     = 0x15U, /**< 10101: OSPI/QSPI. */
  k_ra_mpc_psel_sdhi     = 0x12U, /**< 10010: SDHI. */
  k_ra_mpc_psel_ethernet = 0x17U, /**< 10111: Ethernet (EDMAC/ESWM). */
  k_ra_mpc_psel_glcdc    = 0x15U, /**< 10101 (alt slot -- GLCDC). */
} ra_mpc_psel_t;

/**
 * @enum ra_mpc_dir_t
 * @brief Pin direction for ``ra_mpc_set_gpio``.
 */
typedef enum : uint8_t {
  k_ra_mpc_dir_input  = 0U, /**< Input direction (PDR = 0). */
  k_ra_mpc_dir_output = 1U, /**< Output direction (PDR = 1). */
} ra_mpc_dir_t;

/**
 * @enum ra_mpc_pull_t
 * @brief Pull-up enable for ``ra_mpc_set_pull``.
 */
typedef enum : uint8_t {
  k_ra_mpc_pull_off = 0U,
  k_ra_mpc_pull_up  = 1U,
} ra_mpc_pull_t;

/* =============================================================================
 * Operations
 * =============================================================================
 */

/**
 * @brief Route a pin to a peripheral function.
 *
 * @details
 * Unlocks PWPR, writes the pin's PmnPFS register with PMR = 1 and
 * PSEL = ``psel``, then re-locks PWPR. Any stale direction /
 * pull / open-drain bits are cleared in the same write so the
 * caller sees a clean slate.
 *
 * @param[in] port Port index 0..k_ra_port_max.
 * @param[in] pin Pin index 0..k_ra_pin_max.
 * @param[in] psel Peripheral function from ``ra_mpc_psel_t``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Pin routed.
 * @retval k_ra_err_gpio_invalid_port port index out of range.
 * @retval k_ra_err_gpio_invalid_pin pin index out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post The target pin is in peripheral mode with the requested
 * PSEL value programmed.
 * @post PWPR is re-locked regardless of success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mpc_route_peripheral(ra_port_t port, ra_pin_t pin, ra_mpc_psel_t psel);

/**
 * @brief Reset a pin to plain GPIO with a chosen direction.
 *
 * @param[in] port Port index 0..k_ra_port_max.
 * @param[in] pin Pin index 0..k_ra_pin_max.
 * @param[in] dir Input or output.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Pin reset to GPIO.
 * @retval k_ra_err_gpio_invalid_port Port out of range.
 * @retval k_ra_err_gpio_invalid_pin Pin out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post PMR = 0, PDR = ``dir``, all peripheral select bits cleared.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mpc_set_gpio(ra_port_t port, ra_pin_t pin, ra_mpc_dir_t dir);

/**
 * @brief Put a pin in analog-input mode (ASEL = 1).
 *
 * @param[in] port Port index.
 * @param[in] pin Pin index.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Analog input configured.
 * @retval k_ra_err_gpio_invalid_port Port out of range.
 * @retval k_ra_err_gpio_invalid_pin Pin out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post PmnPFS: ASEL = 1, PMR = 0, PDR = 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mpc_set_analog(ra_port_t port, ra_pin_t pin);

/**
 * @brief Mark a pin as an external IRQ input (ISEL = 1).
 *
 * @details
 * Does NOT program the ICU IELSR entry -- that's ``ra_isr``'s job.
 * This helper just flips the pin's PFS so the edge detector is
 * active.
 *
 * @param[in] port Port index.
 * @param[in] pin Pin index.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Pin ISEL set.
 * @retval k_ra_err_gpio_invalid_port Port out of range.
 * @retval k_ra_err_gpio_invalid_pin Pin out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post PmnPFS: ISEL = 1, PMR = 0, PDR = 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mpc_set_irq(ra_port_t port, ra_pin_t pin);

/**
 * @brief Enable or disable the internal pull-up on a pin.
 *
 * @param[in] port Port index.
 * @param[in] pin Pin index.
 * @param[in] pull Off or on.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Pull updated.
 * @retval k_ra_err_gpio_invalid_port Port out of range.
 * @retval k_ra_err_gpio_invalid_pin Pin out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post PCR reflects the requested state; all other fields
 * untouched.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mpc_set_pull(ra_port_t port, ra_pin_t pin, ra_mpc_pull_t pull);

/**
 * @brief Enable or disable N-channel open-drain on a pin.
 *
 * @param[in] port Port index.
 * @param[in] pin Pin index.
 * @param[in] enable ``true`` for open-drain, ``false`` for CMOS.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok NCODR updated.
 * @retval k_ra_err_gpio_invalid_port Port out of range.
 * @retval k_ra_err_gpio_invalid_pin Pin out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post NCODR reflects the requested state.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mpc_set_open_drain(ra_port_t port, ra_pin_t pin, bool enable);

/**
 * @brief Read the raw 32-bit PmnPFS register for diagnostic use.
 *
 * @param[in] port Port index.
 * @param[in] pin Pin index.
 * @param[out] out_val On success, the current PmnPFS value.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Value returned.
 * @retval k_ra_err_null_ptr ``out_val`` was NULL.
 * @retval k_ra_err_gpio_invalid_port Port out of range.
 * @retval k_ra_err_gpio_invalid_pin Pin out of range.
 *
 * @pre ``out_val`` is non-NULL.
 * @post No hardware state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mpc_read_pfs(ra_port_t port, ra_pin_t pin, uint32_t* out_val);

#ifdef __cplusplus
}
#endif
