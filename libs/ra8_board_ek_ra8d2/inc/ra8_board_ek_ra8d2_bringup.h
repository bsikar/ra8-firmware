/**
 * @file ra8_board_ek_ra8d2_bringup.h
 * @brief EK-RA8D2 substrate prologue behind one audited call
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Every EK-RA8D2 application opens by hand-sequencing the same substrate
 * calls -- ``ra8_cgc_init()``, ``ra8_mstp_init()``,
 * ``ra8_cgc_get_clock_hz()``, ``ra8_time_init()``,
 * ``ra8_board_uart_console_init()``, ``ra8_board_led_init()``,
 * ``ra8_isr_globals_enable()`` -- each wrapped in its own
 * ``!= k_ra8_ok`` guard. Nothing in the tree stated the required order,
 * so each application copied it from whichever sibling it was written
 * beside, and the ordering became a property of that lineage rather
 * than of the board.
 *
 * ::ra8_board_bringup collapses that block to a config struct plus one
 * checked call, and makes the order a property of this library. It is
 * the DEFAULT path, not the only one: ``ra8_cgc_init()``,
 * ``ra8_mstp_init()``, ``ra8_time_init()``,
 * ``ra8_board_uart_console_init()`` and ``ra8_board_led_init()`` stay
 * public and unchanged, so an application wanting a non-default clock
 * tree, no console, or a deliberately different order keeps calling
 * them directly. Same relationship ``ra8_io_blockdev_*`` has to the raw
 * SDHI / XSPI drivers.
 *
 * @code
 * ra8_board_bringup_out_t       sub = {};
 * const ra8_board_bringup_cfg_t cfg = {
 *   .console_baud      = 115200U,
 *   .leds_mask         = (uint32_t)k_ra8_board_bringup_led1,
 *   .enable_interrupts = true,
 * };
 * if (ra8_board_bringup(&cfg, &sub) != k_ra8_ok) {
 *   app_panic_halt();
 * }
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_board_bringup_led_mask_t
 * @brief Selector bits for ::ra8_board_bringup_cfg_t::leds_mask.
 *
 * @details
 * One bit per ::ra8_board_led_id_t, in enumeration order, so
 * ``1U << (uint8_t)led`` is the bit for any LED id. Kept as a separate
 * mask type because ::ra8_board_led_id_t is an ordinal (0, 1, 2), not a
 * bitmask, and passing an ordinal where a mask is expected would light
 * the wrong pin.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_board_bringup_leds_none = 0x0U, /**< Bring no user LED up. */
  k_ra8_board_bringup_led1      = 0x1U, /**< LED1, BLUE,  P600.    */
  k_ra8_board_bringup_led2      = 0x2U, /**< LED2, GREEN, P303.    */
  k_ra8_board_bringup_led3      = 0x4U, /**< LED3, RED,   PA07.    */
  k_ra8_board_bringup_leds_all  = 0x7U, /**< All three user LEDs.  */
} ra8_board_bringup_led_mask_t;

/**
 * @struct ra8_board_bringup_cfg_t
 * @brief What an application wants from the substrate prologue.
 *
 * @details
 * Zero-initialise (``= {}``) and set only what the application needs:
 * the zero value of every field is the "do not bring this up" choice,
 * so a compute-only application that wants clocks and a timebase and
 * nothing else passes an all-zero struct.
 *
 * @invariant @c leds_mask carries no bit outside
 *            ::k_ra8_board_bringup_leds_all.
 * @since 0.1.0
 */
typedef struct {
  uint32_t console_baud;      /**< J-Link OB VCOM line rate; 0 brings no console up.  */
  uint32_t leds_mask;         /**< ::ra8_board_bringup_led_mask_t bits to initialise. */
  bool     enable_interrupts; /**< Run ``ra8_isr_globals_enable()`` as the last step. */
} ra8_board_bringup_cfg_t;

/**
 * @struct ra8_board_bringup_out_t
 * @brief Clock rates the prologue observed, for the caller's own setup.
 *
 * @details
 * Both rates are read back from the clock generator after the tree is
 * up, not restated from a compile-time constant, so they are the same
 * values the console's bit-rate divisor was computed against.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t cpuclk0_hz; /**< Live CPUCLK0 rate, the one ``ra8_time_init`` was given. */
  uint32_t pclka_hz;   /**< Live PCLKA rate, the one the console divisor used.      */
} ra8_board_bringup_out_t;

/**
 * @brief Run the EK-RA8D2 substrate prologue in one audited order.
 *
 * @details
 * The order is fixed here and is the one the pieces actually require:
 *
 *   1. ``ra8_board_clocks_init()`` -- PLL1 tree up (wraps ``ra8_cgc_init``).
 *   2. ``ra8_mstp_init()`` -- module-stop refcounts reset; the console
 *      documents this as a precondition.
 *   3. ``ra8_cgc_get_clock_hz()`` for CPUCLK0 and PCLKA, read back live.
 *   4. ``ra8_time_init(cpuclk0_hz)`` -- 1 kHz SysTick against that rate.
 *   5. ``ra8_board_uart_console_init(console_baud)`` when the baud is
 *      non-zero; its divisor needs the post-PLL PCLKA from step 1.
 *   6. ``ra8_board_led_init()`` for each bit of @c leds_mask, LED1 first.
 *   7. ``ra8_isr_globals_enable()`` when asked, always last, so no IRQ
 *      dispatches into half-initialised driver state.
 *
 * First failing step returns; nothing is unwound, exactly as the
 * hand-written block behaved, because the failure arm of that block was
 * always a halt.
 *
 * @param[in]  cfg What to bring up. Must be non-null.
 * @param[out] out Observed clock rates. Must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Every requested step succeeded.
 * @retval k_ra8_err_null_ptr    @p cfg or @p out was null.
 * @retval k_ra8_err_invalid_arg @p cfg->leds_mask carries a bit outside
 *                               ::k_ra8_board_bringup_leds_all.
 * @retval other                 Propagated unchanged from the first
 *                               failing step.
 *
 * @pre Single-threaded board boot context, before any driver init.
 * @post On success the clock tree, timebase, and every requested
 *       facility are up, and @p out carries the live rates.
 * @post On failure @p out is unchanged and the steps that had already
 *       succeeded stay up.
 *
 * @note Not thread-safe; call once during board bring-up.
 *
 * @see ra8_board_clocks_init      Step 1 on its own.
 * @see ra8_board_uart_console_init Step 5 on its own.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_bringup(const ra8_board_bringup_cfg_t* cfg,
                                          ra8_board_bringup_out_t*       out);

#ifdef __cplusplus
}
#endif
