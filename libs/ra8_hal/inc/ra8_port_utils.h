/**
 * @file ra8_port_utils.h
 * @brief High-level GPIO helpers on top of the PORT + PFS register layer
 * @ingroup grp_hal_system
 *
 * @details
 * Thin convenience API that takes `ra8_port_pin_t` values and wraps the
 * PORT and PFS register writes with the correct PWPR unlock / lock
 * sequence. Drivers should prefer these helpers over hand-coding the
 * PFS dance at every callsite.
 *
 * ## Pattern
 *
 * @code{.c}
 * ra8_err_t err = ra8_gpio_output_init(k_ra8_pin_led1, k_ra8_level_low);
 * RA8_ERROR_CHECK(err);
 * ra8_gpio_write(k_ra8_pin_led1, k_ra8_level_high);
 * @endcode
 *
 * Every helper claims ownership of the pin through
 * `ra8_pin_validator_claim()` under the tag `"GPIO"`. If a different
 * driver later tries to claim the same pin, it gets
 * `k_ra8_err_gpio_conflict`.
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
#include "ra8_gpio_constants.h"
#include "ra8_icu_regs.h"
#include "ra8_isr.h"
#include "ra8_pfs_regs.h"
#include "ra8_port_constants.h"

/**
 * @brief Configure a pin as a digital output and drive it to an
 * initial level.
 *
 * @param[in] pin Packed port/pin identifier.
 * @param[in] init_level Initial output level (`k_ra8_level_low` or
 * `k_ra8_level_high`).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Pin configured and driven.
 * @retval k_ra8_err_gpio_invalid_port Port out of range.
 * @retval k_ra8_err_gpio_invalid_pin Pin out of range.
 * @retval k_ra8_err_gpio_conflict Pin already owned.
 * @retval k_ra8_err_hw_unmapped The port/pin pair has no PFS register on
 * this device.
 *
 * @pre `ra8_infrastructure_init()` has run (pin validator ready).
 * @pre The IOPORT module clock is on (IOPORT is one of the "always on"
 * blocks, so this is satisfied automatically after reset).
 * @post On success, the pin is in GPIO-output mode driving `init_level`.
 * @post On success, the pin is owned by this driver (`"GPIO"` tag).
 * @post On **any** failure, pin ownership is unchanged: a claim taken while
 * configuring is handed back before returning, so a failed call never
 * strands the pin. Callers may retry or fall through without leaking it.
 *
 * @note Not thread-safe: reads / modifies / writes the PFS register
 * and touches PWPR. Protect with IRQ masking or run during
 * single-threaded init.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpio_output_init(ra8_port_pin_t pin, ra8_level_t init_level);

/**
 * @brief Configure a pin as a digital input.
 *
 * @param[in] pin Packed port/pin identifier.
 * @param[in] pull `k_ra8_pull_none` / `k_ra8_pull_up`. The RA8D2 PFS has a
 * pull-up bit only, so `k_ra8_pull_down` is accepted but configures no pull.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Pin configured as a digital input.
 * @retval k_ra8_err_gpio_invalid_port Port out of range.
 * @retval k_ra8_err_gpio_invalid_pin Pin out of range.
 * @retval k_ra8_err_gpio_conflict Pin already owned.
 * @retval k_ra8_err_hw_unmapped The port/pin pair has no PFS register on
 * this device.
 *
 * @pre `ra8_infrastructure_init()` has run (pin validator ready).
 * @pre The IOPORT module clock is on (always-on after reset).
 * @post On success, the pin is a digital input owned by this driver.
 * @post On **any** failure, pin ownership is unchanged (see
 * `ra8_gpio_output_init()`).
 *
 * @note Not thread-safe; same PFS/PWPR caveat as `ra8_gpio_output_init()`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpio_input_init(ra8_port_pin_t pin, ra8_pin_pull_t pull);

/**
 * @brief Drive a previously-configured output to the given level.
 *
 * @param[in] pin Packed port/pin identifier.
 * @param[in] level Target level.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Level driven.
 * @retval k_ra8_err_gpio_invalid_port Port out of range.
 * @retval k_ra8_err_gpio_invalid_pin Pin out of range.
 *
 * @note Uses the atomic POSR/PORR register so the write is race-free
 * against concurrent updates to other pins of the same port.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpio_write(ra8_port_pin_t pin, ra8_level_t level);

/**
 * @brief Toggle a previously-configured output.
 *
 * @param[in] pin Packed port/pin identifier.
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpio_toggle(ra8_port_pin_t pin);

/**
 * @brief Read a previously-configured input.
 *
 * @param[in] pin Packed port/pin identifier.
 * @param[out] out_level Pointer to receive current level.
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpio_read(ra8_port_pin_t pin, ra8_level_t* out_level);

/**
 * @brief Release a GPIO pin claim.
 *
 * @param[in] pin Packed port/pin identifier previously claimed by a
 *                GPIO init call.
 * @return `ra8_err_t` error code from the pin validator.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpio_release(ra8_port_pin_t pin);

/**
 * @brief Route a pin to a non-IRQ peripheral function via PFS.PSEL.
 *
 * @details
 * Claims the pin through the validator, unlocks PWPR, writes
 * `PMR=1` plus the requested PSEL code into the pin's PFS register,
 * relocks PWPR, and logs the routing. Use for SCI / IIC / SPI /
 * GPT / xSPI / GLCDC / etc. -- any peripheral whose pin selection
 * is encoded in the PFS PSEL field. External IRQ inputs go through
 * `ra8_gpio_attach_irq` instead, which combines the PFS write with
 * the ICU + NVIC configuration in one call.
 *
 * @param[in] pin   Packed port/pin identifier (see `RA8_PIN(...)`).
 * @param[in] psel  PFS PSEL code (`ra8_psel_t` from
 *                  `ra8_gpio_constants.h`).
 * @param[in] owner Non-NULL static string used for the validator
 *                  ownership log.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                 Pin is now in peripheral mode.
 * @retval k_ra8_err_null_ptr       `owner` was nullptr.
 * @retval k_ra8_err_gpio_invalid_port  Port index out of range.
 * @retval k_ra8_err_gpio_invalid_pin   Pin index out of range.
 * @retval k_ra8_err_gpio_conflict      Pin is already claimed by
 *                                     another owner.
 * @retval k_ra8_err_hw_unmapped        Pin has no PFS mapping
 *                                     (host-test sim only).
 *
 * @pre IOPORT module is reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success the pin's PFS holds `PMR=1 | (psel << PSEL0)`.
 * @post On success the pin is claimed by `owner`.
 *
 * @note Not thread-safe; intended for boot.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_pfs_route_peripheral(ra8_port_pin_t pin, ra8_psel_t psel, const char* owner);

/**
 * @brief Set the output drive strength (PmnPFS.DSCR) of an already-routed pin.
 *
 * @details
 * Read-modify-write of the PmnPFS.DSCR[1:0] field for one pin under a
 * PWPR write-protect unlock. Only the DSCR field is touched -- the
 * pin's PSEL / PMR / direction are preserved -- so this is meant to
 * be called AFTER ::ra8_pfs_route_peripheral (or ::ra8_gpio_output_init)
 * has already configured the pin's function.
 *
 * High-speed peripheral outputs need more than the reset-default low
 * drive: HUM Ch 20.2.6 requires DSCR = 01b for RGMII/3.3 V transmit
 * pins (and 11b for RGMII/2.5 V). Leaving an RGMII transmit pin at
 * the default 00b leaves its edge timing out of spec -- bench-
 * confirmed to corrupt the majority of transmitted frames.
 *
 * @param[in] pin  Packed port/pin identifier (::RA8_PIN).
 * @param[in] dscr Target drive-capability code (::ra8_pfs_dscr_t).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    DSCR updated.
 * @retval k_ra8_err_gpio_invalid_port port index out of range.
 * @retval k_ra8_err_gpio_invalid_pin  pin index out of range.
 * @retval k_ra8_err_hw_unmapped       pin has no PFS register.
 *
 * @pre The pin has already been routed / configured by the caller.
 * @pre Caller is single-threaded init context.
 * @post On success the pin's PmnPFS.DSCR equals @p dscr.
 * @post PSEL / PMR / direction bits are unchanged.
 *
 * @note Not thread-safe; intended for boot.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pfs_set_drive_strength(ra8_port_pin_t pin, ra8_pfs_dscr_t dscr);

/* =============================================================================
 * External IRQ attachment
 * =============================================================================
 */

/**
 * @struct ra8_gpio_irq_cfg_t
 * @brief Configuration descriptor for ``ra8_gpio_attach_irq``.
 *
 * @details
 * Bundles the pin-level pull mode, the IRQCRi sense / filter
 * fields, and the NVIC priority into a single struct so the
 * caller can wire an external-IRQ input with one call instead
 * of the four-step ``ra8_gpio_input_init`` +
 * ``ra8_pfs_route_peripheral`` + ``ra8_icu_configure_irq_pin`` +
 * ``ra8_isr_register`` dance.
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_gpio_attach_irq`` in
 * ``libs/ra8_hal/src/gpio.c``.
 */
typedef struct {
  ra8_pin_pull_t    pull;       /**< Input pull setting.            */
  ra8_icu_irqmd_t   sense;      /**< Edge / level detection mode.   */
  ra8_icu_fclksel_t filter_div; /**< Digital filter clock divider.  */
  bool              filter_en;  /**< True -> enable digital filter. */
  uint8_t           priority;   /**< NVIC priority 0..15.           */
} ra8_gpio_irq_cfg_t;

/**
 * @brief Wire a pin as an external IRQ input and install a handler.
 *
 * @details
 * One-call convenience that:
 * - configures the pin as GPIO input with the requested pull,
 * - programmes IRQCRi edge / filter fields,
 * - registers ``handler`` against the matching ICU IRQ event in
 * the ``ra8_isr`` table (which also enables the NVIC
 * line and sets priority).
 *
 * @param[in] pin Packed port/pin identifier for the IRQ input.
 * @param[in] irq_num External IRQ pin number 0..15.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @param[in] handler Non-NULL handler invoked on IRQ firing.
 * @param[in] ctx Opaque context forwarded to the handler.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Pin, IRQCR, ISR all programmed.
 * @retval k_ra8_err_null_ptr ``cfg`` or ``handler`` was NULL.
 * @retval k_ra8_err_invalid_arg ``irq_num`` out of range.
 * @retval k_ra8_err_gpio_invalid_* Pin port/pin out of range.
 * @retval k_ra8_err_exists IRQ event already registered.
 *
 * @pre ``ra8_infrastructure_init`` and ``ra8_icu_init`` / ``ra8_isr_init``
 * have run.
 * @post On success the pin is input-configured, IRQCR[irq_num] matches
 * ``cfg``, and the NVIC line is enabled.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpio_attach_irq(ra8_port_pin_t            pin,
                                            uint8_t                   irq_num,
                                            const ra8_gpio_irq_cfg_t* cfg,
                                            ra8_isr_handler_t         handler,
                                            void*                     ctx);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Pin and IRQ torn down.
 * @retval k_ra8_err_invalid_arg ``irq_num`` out of range.
 * @retval k_ra8_err_not_found No handler registered for this IRQ.
 *
 * @pre ``ra8_gpio_attach_irq`` was called for the same ``irq_num``.
 * @post NVIC line disabled, IRQCR == 0, pin validator released.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gpio_detach_irq(ra8_port_pin_t pin, uint8_t irq_num);

#ifdef __cplusplus
}
#endif
