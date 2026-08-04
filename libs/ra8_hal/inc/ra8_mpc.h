/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_mpc.h
 * @brief Multi-function Pin Controller (MPC) facade
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Ring 3 / HAL substrate. Wraps the RA8D2 ``PmnPFS`` register array
 * (HUM Ch 20 "I/O Ports", p 837) behind a small set of
 * operation-named helpers:
 *
 * - ``ra8_mpc_route_peripheral`` -- map a pin to a PSEL function.
 * - ``ra8_mpc_set_gpio`` -- reset a pin to plain GPIO.
 * - ``ra8_mpc_set_analog`` -- put a pin in analog-input mode.
 * - ``ra8_mpc_set_irq`` -- mark a pin as an external IRQ input.
 * - ``ra8_mpc_set_pull`` -- toggle pull-up.
 * - ``ra8_mpc_set_open_drain`` -- toggle N-channel open-drain output.
 *
 * Drivers never touch ``PmnPFS`` directly. The substrate handles
 * the PWPR unlock / lock sequence, the "write zero first" reset,
 * and the pin-validator registration so two drivers cannot claim
 * the same pin without being told about it.
 *
 * ## Threading
 *
 * Single-threaded init context only.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_port_constants.h"

/* =============================================================================
 * Types
 * =============================================================================
 */

/**
 * @enum ra8_mpc_psel_t
 * @brief Peripheral function select values for the PMR/PSEL path.
 *
 * @details
 * Values come from HUM Ch 20 table "Pin function select" (section
 * 20.2.x), cross-verified against FSP
 * ``ra/fsp/src/bsp/mcu/ra8d2/bsp_override.h`` (RA8D2-specific
 * ``ioport_peripheral_t`` enum). The 5-bit PSEL field accepts values
 * 0x00..0x1F; only the codes the firmware currently routes are
 * listed -- extend as new drivers come online.
 *
 * @note Values are the *raw* 5-bit PSEL, not pre-shifted into bits
 *       28..24. ``ra8_mpc_route_peripheral`` shifts left by
 *       ``k_ra8_pfs_bit_psel0`` before writing.
 */
typedef enum : uint8_t {
  k_ra8_mpc_psel_gpio        = 0x00U, /**< 00000: plain GPIO / DEBUG.               */
  k_ra8_mpc_psel_agt         = 0x01U, /**< 00001: AGT/AGTW timer I/O.               */
  k_ra8_mpc_psel_gpt0        = 0x02U, /**< 00010: GPT0 (low channels).              */
  k_ra8_mpc_psel_gpt1        = 0x03U, /**< 00011: GPT1 alt mux.                     */
  k_ra8_mpc_psel_sci0        = 0x04U, /**< 00100: SCI 0/2/4/6/8.                    */
  k_ra8_mpc_psel_sci1        = 0x05U, /**< 00101: SCI 1/3/5/7/9.                    */
  k_ra8_mpc_psel_spi         = 0x06U, /**< 00110: SPI.                              */
  k_ra8_mpc_psel_iic         = 0x07U, /**< 00111: IIC.                              */
  k_ra8_mpc_psel_key         = 0x08U, /**< 01000: KEY input.                        */
  k_ra8_mpc_psel_clkout_rtc  = 0x09U, /**< 01001: CLKOUT / comparator / RTC.        */
  k_ra8_mpc_psel_cac_adc     = 0x0AU, /**< 01010: CAC / ADC trigger.                */
  k_ra8_mpc_psel_bus         = 0x0BU, /**< 01011: external bus interface.           */
  k_ra8_mpc_psel_acmphs      = 0x0CU, /**< 01100: ACMPHS / CTSU.                    */
  k_ra8_mpc_psel_lcdc        = 0x0DU, /**< 01101: segment LCDC / SCI DEn.           */
  k_ra8_mpc_psel_ceu         = 0x0FU, /**< 01111: camera engine unit (CEU).         */
  k_ra8_mpc_psel_canfd       = 0x10U, /**< 10000: CAN / CANFD.                      */
  k_ra8_mpc_psel_qspi        = 0x11U, /**< 10001: QSPI controller.                  */
  k_ra8_mpc_psel_ssi         = 0x12U, /**< 10010: SSI audio.                        */
  k_ra8_mpc_psel_usbfs       = 0x13U, /**< 10011: USB Full-Speed.                   */
  k_ra8_mpc_psel_usbhs       = 0x14U, /**< 10100: USB High-Speed / GPT2.            */
  k_ra8_mpc_psel_sdhi        = 0x15U, /**< 10101: SDHI / MMC / GPT3.                */
  k_ra8_mpc_psel_ether_mii   = 0x16U, /**< 10110: Ethernet MII / GPT4 / UARTA.      */
  k_ra8_mpc_psel_ether_rmii  = 0x17U, /**< 10111: Ethernet RMII.                    */
  k_ra8_mpc_psel_pdc         = 0x18U, /**< 11000: PDC / AGT1 / Ether RGMII (alias). */
  k_ra8_mpc_psel_ether_rgmii = 0x18U, /**< 11000: Ethernet RGMII (preferred name).  */
  k_ra8_mpc_psel_glcdc       = 0x19U, /**< 11001: graphics LCDC / CAC.              */
  k_ra8_mpc_psel_trace       = 0x1AU, /**< 11010: ETM debug trace.                  */
  k_ra8_mpc_psel_pdm         = 0x1BU, /**< 11011: PDM / GPT5.                       */
  k_ra8_mpc_psel_ospi        = 0x1CU, /**< 11100: Octa-SPI controller.              */
  k_ra8_mpc_psel_cec         = 0x1DU, /**< 11101: CEC / PGAOUT0.                    */
  k_ra8_mpc_psel_ulpt        = 0x1EU, /**< 11110: ULPT / PGAOUT1.                   */
  k_ra8_mpc_psel_mipi        = 0x1FU, /**< 11111: MIPI DSI.                         */
} ra8_mpc_psel_t;

/**
 * @enum ra8_mpc_dir_t
 * @brief Pin direction for ``ra8_mpc_set_gpio``.
 */
typedef enum : uint8_t {
  k_ra8_mpc_dir_input  = 0U, /**< Input direction (PDR = 0).  */
  k_ra8_mpc_dir_output = 1U, /**< Output direction (PDR = 1). */
} ra8_mpc_dir_t;

/**
 * @enum ra8_mpc_pull_t
 * @brief Pull-up enable for ``ra8_mpc_set_pull``.
 */
typedef enum : uint8_t {
  k_ra8_mpc_pull_off = 0U, /**< RA8 mpc pull off. */
  k_ra8_mpc_pull_up  = 1U, /**< RA8 mpc pull up.  */
} ra8_mpc_pull_t;

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
 * @param[in] port Port index 0..k_ra8_port_max.
 * @param[in] pin Pin index 0..k_ra8_pin_max.
 * @param[in] psel Peripheral function from ``ra8_mpc_psel_t``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Pin routed.
 * @retval k_ra8_err_gpio_invalid_port port index out of range.
 * @retval k_ra8_err_gpio_invalid_pin pin index out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post The target pin is in peripheral mode with the requested
 * PSEL value programmed.
 * @post PWPR is re-locked regardless of success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_mpc_route_peripheral(ra8_port_t port, ra8_pin_t pin, ra8_mpc_psel_t psel);

/**
 * @brief Reset a pin to plain GPIO with a chosen direction.
 *
 * @param[in] port Port index 0..k_ra8_port_max.
 * @param[in] pin Pin index 0..k_ra8_pin_max.
 * @param[in] dir Input or output.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Pin reset to GPIO.
 * @retval k_ra8_err_gpio_invalid_port Port out of range.
 * @retval k_ra8_err_gpio_invalid_pin Pin out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post PMR = 0, PDR = ``dir``, all peripheral select bits cleared.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mpc_set_gpio(ra8_port_t port, ra8_pin_t pin, ra8_mpc_dir_t dir);

/**
 * @brief Put a pin in analog-input mode (ASEL = 1).
 *
 * @param[in] port Port index.
 * @param[in] pin Pin index.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Analog input configured.
 * @retval k_ra8_err_gpio_invalid_port Port out of range.
 * @retval k_ra8_err_gpio_invalid_pin Pin out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post PmnPFS: ASEL = 1, PMR = 0, PDR = 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mpc_set_analog(ra8_port_t port, ra8_pin_t pin);

/**
 * @brief Mark a pin as an external IRQ input (ISEL = 1).
 *
 * @details
 * Does NOT program the ICU IELSR entry -- that's ``ra8_isr``'s job.
 * This helper just flips the pin's PFS so the edge detector is
 * active.
 *
 * @param[in] port Port index.
 * @param[in] pin Pin index.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Pin ISEL set.
 * @retval k_ra8_err_gpio_invalid_port Port out of range.
 * @retval k_ra8_err_gpio_invalid_pin Pin out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post PmnPFS: ISEL = 1, PMR = 0, PDR = 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mpc_set_irq(ra8_port_t port, ra8_pin_t pin);

/**
 * @brief Enable or disable the internal pull-up on a pin.
 *
 * @param[in] port Port index.
 * @param[in] pin Pin index.
 * @param[in] pull Off or on.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Pull updated.
 * @retval k_ra8_err_gpio_invalid_port Port out of range.
 * @retval k_ra8_err_gpio_invalid_pin Pin out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post PCR reflects the requested state; all other fields
 * untouched.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mpc_set_pull(ra8_port_t port, ra8_pin_t pin, ra8_mpc_pull_t pull);

/**
 * @brief Enable or disable N-channel open-drain on a pin.
 *
 * @param[in] port Port index.
 * @param[in] pin Pin index.
 * @param[in] enable ``true`` for open-drain, ``false`` for CMOS.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok NCODR updated.
 * @retval k_ra8_err_gpio_invalid_port Port out of range.
 * @retval k_ra8_err_gpio_invalid_pin Pin out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post NCODR reflects the requested state.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mpc_set_open_drain(ra8_port_t port, ra8_pin_t pin, bool enable);

/**
 * @brief Read the raw 32-bit PmnPFS register for diagnostic use.
 *
 * @param[in] port Port index.
 * @param[in] pin Pin index.
 * @param[out] out_val On success, the current PmnPFS value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Value returned.
 * @retval k_ra8_err_null_ptr ``out_val`` was NULL.
 * @retval k_ra8_err_gpio_invalid_port Port out of range.
 * @retval k_ra8_err_gpio_invalid_pin Pin out of range.
 *
 * @pre ``out_val`` is non-NULL.
 * @post No hardware state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mpc_read_pfs(ra8_port_t port, ra8_pin_t pin, uint32_t* out_val);

#ifdef __cplusplus
}
#endif
