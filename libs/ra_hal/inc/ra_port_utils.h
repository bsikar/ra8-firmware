/**
 * @file ra_port_utils.h
 * @brief High-level GPIO helpers on top of the PORT + PFS register layer
 *
 * @details
 * Thin convenience API that takes `ra_port_pin_t` values and wraps the
 * PORT and PFS register writes with the correct PWPR unlock / lock
 * sequence. Drivers should prefer these helpers over hand-coding the
 * PFS dance at every callsite.
 *
 * ## Pattern
 *
 * @code{.c}
 * ra_err_t err = ra_gpio_output_init(k_ra_pin_led1, k_ra_level_low);
 * RA_ERROR_CHECK(err);
 * ra_gpio_write(k_ra_pin_led1, k_ra_level_high);
 * @endcode
 *
 * Every helper claims ownership of the pin through
 * `ra_pin_validator_claim()` under the tag `"GPIO"`. If a different
 * driver later tries to claim the same pin, it gets
 * `k_ra_err_gpio_conflict`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_icu_regs.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_port_constants.h"

/**
 * @brief Configure a pin as a digital output and drive it to an
 * initial level.
 *
 * @param[in] pin Packed port/pin identifier.
 * @param[in] init_level Initial output level (`k_ra_level_low` or
 * `k_ra_level_high`).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Pin configured and driven.
 * @retval k_ra_err_gpio_invalid_port Port out of range.
 * @retval k_ra_err_gpio_invalid_pin Pin out of range.
 * @retval k_ra_err_gpio_conflict Pin already owned.
 *
 * @pre `ra_infrastructure_init()` has run (pin validator ready).
 * @pre The IOPORT module clock is on (IOPORT is one of the "always on"
 * blocks, so this is satisfied automatically after reset).
 * @post On success, the pin is in GPIO-output mode driving `init_level`.
 * @post On success, the pin is owned by this driver (`"GPIO"` tag).
 *
 * @note Not thread-safe: reads / modifies / writes the PFS register
 * and touches PWPR. Protect with IRQ masking or run during
 * single-threaded init.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_gpio_output_init(ra_port_pin_t pin, ra_level_t init_level);

/**
 * @brief Configure a pin as a digital input.
 *
 * @param[in] pin Packed port/pin identifier.
 * @param[in] pull `k_ra_pull_none` / `k_ra_pull_up`.
 *
 * @return `ra_err_t` error code (same set as `ra_gpio_output_init()`).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_gpio_input_init(ra_port_pin_t pin, ra_pin_pull_t pull);

/**
 * @brief Drive a previously-configured output to the given level.
 *
 * @param[in] pin Packed port/pin identifier.
 * @param[in] level Target level.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Level driven.
 * @retval k_ra_err_gpio_invalid_port Port out of range.
 * @retval k_ra_err_gpio_invalid_pin Pin out of range.
 *
 * @note Uses the atomic POSR/PORR register so the write is race-free
 * against concurrent updates to other pins of the same port.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_gpio_write(ra_port_pin_t pin, ra_level_t level);

/**
 * @brief Toggle a previously-configured output.
 *
 * @param[in] pin Packed port/pin identifier.
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_gpio_toggle(ra_port_pin_t pin);

/**
 * @brief Read a previously-configured input.
 *
 * @param[in] pin Packed port/pin identifier.
 * @param[out] out_level Pointer to receive current level.
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_gpio_read(ra_port_pin_t pin, ra_level_t* out_level);

/* =============================================================================
 * External IRQ attachment
 * =============================================================================
 */

/**
 * @struct ra_gpio_irq_cfg_t
 * @brief Configuration descriptor for ``ra_gpio_attach_irq``.
 *
 * @details
 * Bundles the pin-level pull mode, the IRQCRi sense / filter
 * fields, and the NVIC priority into a single struct so the
 * caller can wire an external-IRQ input with one call instead
 * of the four-step ``ra_gpio_input_init`` +
 * ``ra_pfs_route_peripheral`` + ``ra_icu_configure_irq_pin`` +
 * ``ra_isr_register`` dance.
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_gpio_attach_irq`` in
 * ``libs/ra_hal/src/gpio.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_pin_pull_t    pull;       /**< Input pull setting. */
  ra_icu_irqmd_t   sense;      /**< Edge / level detection mode. */
  ra_icu_fclksel_t filter_div; /**< Digital filter clock divider. */
  bool             filter_en;  /**< True -> enable digital filter. */
  uint8_t          priority;   /**< NVIC priority 0..15. */
} ra_gpio_irq_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Wire a pin as an external IRQ input and install a handler.
 *
 * @details
 * One-call convenience that:
 * - configures the pin as GPIO input with the requested pull,
 * - programmes IRQCRi edge / filter fields,
 * - registers ``handler`` against the matching ICU IRQ event in
 * the ``ra_isr`` table (which also enables the NVIC
 * line and sets priority).
 *
 * @param[in] pin Packed port/pin identifier for the IRQ input.
 * @param[in] irq_num External IRQ pin number 0..15.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @param[in] handler Non-NULL handler invoked on IRQ firing.
 * @param[in] ctx Opaque context forwarded to the handler.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Pin, IRQCR, ISR all programmed.
 * @retval k_ra_err_null_ptr ``cfg`` or ``handler`` was NULL.
 * @retval k_ra_err_invalid_arg ``irq_num`` out of range.
 * @retval k_ra_err_gpio_invalid_* Pin port/pin out of range.
 * @retval k_ra_err_exists IRQ event already registered.
 *
 * @pre ``ra_infrastructure_init`` and ``ra_icu_init`` / ``ra_isr_init``
 * have run.
 * @post On success the pin is input-configured, IRQCR[irq_num] matches
 * ``cfg``, and the NVIC line is enabled.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_gpio_attach_irq(ra_port_pin_t            pin,
                                          uint8_t                  irq_num,
                                          const ra_gpio_irq_cfg_t* cfg,
                                          ra_isr_handler_t         handler,
                                          void*                    ctx);

/**
 * @brief Tear down a previously attached external IRQ.
 *
 * @details
 * Unregisters the ISR slot (disables the NVIC line, clears IELSR),
 * zeroes IRQCR[irq_num], and releases the pin claim so another
 * driver can reuse it.
 *
 * @param[in] pin Packed port/pin identifier previously attached.
 * @param[in] irq_num External IRQ number 0..15.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Pin and IRQ torn down.
 * @retval k_ra_err_invalid_arg ``irq_num`` out of range.
 * @retval k_ra_err_not_found No handler registered for this IRQ.
 *
 * @pre ``ra_gpio_attach_irq`` was called for the same ``irq_num``.
 * @post NVIC line disabled, IRQCR == 0, pin validator released.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_gpio_detach_irq(ra_port_pin_t pin, uint8_t irq_num);

#ifdef __cplusplus
}
#endif
