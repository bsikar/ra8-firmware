/**
 * @file fw_if_clock.h
 * @brief Architecture-neutral clock-intent port: rates and gating keyed on the
 * module that needs them.
 * @ingroup grp_fw_clock
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * This is the clock port `docs/PORTS.md` puts first in the build order, and it
 * is deliberately not a portable register API. The RA8, RX and RISC-V clock
 * trees do not share a shape: PCLKA is not a concept a portable driver can be
 * asked to know, and the 231 first-party files that today call
 * `ra8_cgc_get_clock_hz` all pass it a `k_ra8_clock_id_*` they had to look up
 * from a chip manual.
 *
 * What those callers actually want is one of three things:
 *
 *   1. the rate of whatever clock feeds the block they drive, so they can
 *      compute a divisor, a baud prescale, or a sampling period;
 *   2. an answer to whether that rate satisfies a floor their block needs;
 *   3. that block's clock turned on or off.
 *
 * None of those three needs the caller to name a clock domain, so this port
 * does not let it. A request names a *module* -- a neutral peripheral kind and
 * an instance index -- and a binding supplied by the chip and the board
 * resolves it against a clock profile they own between them. The chip knows
 * which domain feeds SCI3; the board knows what that domain was programmed to.
 * A portable driver knows neither and now needs neither.
 *
 * This port reads and gates. It does not reprogram the tree: dividers, PLL
 * multipliers and source selection stay with the board's own bring-up, because
 * a portable driver raising the bus clock to suit itself would silently change
 * the timing of every other block hanging off it. ::fw_clock_require therefore
 * reports whether a floor is met; it never reaches for a dial to meet it.
 *
 * Per the taxonomy in `docs/PORTS.md`, this uses (a) the caller-allocated
 * facade plus (b) a narrow ops struct the binding fills. It does not use (c):
 * there is no Ring-3 driver that needs a bridged subset today, and an
 * `_as_ops()` with no consumer is a function that rots.
 *
 * Handles are caller-owned. This interface performs no allocation and contains
 * no chip, operating-system or device header.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum fw_clock_module_kind_t
 * @brief Neutral peripheral kinds a clock request can name.
 *
 * @details
 * One enumerator per *kind* of block, never per chip block name: a binding maps
 * `k_fw_clock_module_uart` onto SCI on this chip, SCIF on another, and a UART
 * peripheral on a third. The list covers the blocks first-party code clocks
 * today; adding a kind is a contract change, which is the point -- it forces
 * the question of which domain feeds it on every chip, rather than letting a
 * driver smuggle in a register name.
 */
typedef enum : uint8_t {
  k_fw_clock_module_none    = 0U,   /**< Not a module; rejected.      */
  k_fw_clock_module_core    = 1U,   /**< The CPU the caller runs on.  */
  k_fw_clock_module_uart    = 2U,   /**< Asynchronous serial.         */
  k_fw_clock_module_spi     = 3U,   /**< Synchronous serial, master.  */
  k_fw_clock_module_i2c     = 4U,   /**< Two-wire serial.             */
  k_fw_clock_module_can     = 5U,   /**< CAN or CAN FD.               */
  k_fw_clock_module_timer   = 6U,   /**< General-purpose timer.       */
  k_fw_clock_module_pwm     = 7U,   /**< Timer in output-compare use. */
  k_fw_clock_module_adc     = 8U,   /**< Analog capture.              */
  k_fw_clock_module_dac     = 9U,   /**< Analog output.               */
  k_fw_clock_module_dma     = 10U,  /**< Bus-master transfer engine.  */
  k_fw_clock_module_display = 11U,  /**< Display controller.          */
  k_fw_clock_module_camera  = 12U,  /**< Image capture unit.          */
  k_fw_clock_module_usb     = 13U,  /**< USB device or host.          */
  k_fw_clock_module_ethernet = 14U, /**< MAC, not the PHY.            */
  k_fw_clock_module_sdhost  = 15U,  /**< SD or eMMC host.             */
  k_fw_clock_module_crypto  = 16U,  /**< Hardware crypto engine.      */
  k_fw_clock_module_rtc     = 17U,  /**< Real-time clock.             */
  k_fw_clock_module_watchdog = 18U, /**< Watchdog counter.            */
  k_fw_clock_module_memory  = 19U,  /**< Flash, MRAM or external bus. */
} fw_clock_module_kind_t;

/** @brief One past the last valid ::fw_clock_module_kind_t enumerator. */
#define K_FW_CLOCK_MODULE_KIND_COUNT 20U

/**
 * @struct fw_clock_module_t
 * @brief Which block a request is about.
 *
 * @details
 * `index` is the instance number within the kind as the *board* numbers them,
 * zero-based and dense. It is not a chip channel number: a board that wires
 * only SCI3 and SCI9 presents UART 0 and UART 1, and its binding maps them.
 * That indirection is what lets an application move between boards without
 * renumbering its ports.
 */
typedef struct fw_clock_module_s {
  fw_clock_module_kind_t kind;  /**< Which kind of block.        */
  uint8_t                index; /**< Board instance, zero-based. */
} fw_clock_module_t;

/**
 * @struct fw_clock_iface_t
 * @brief The narrow ops struct a chip-and-board binding fills.
 *
 * @details
 * Three entries, because the port has three intents. Each takes the binding's
 * own context as its first argument, so one binding can serve more than one
 * instance without a file-scope variable. A binding that cannot gate a
 * particular module returns ::k_ra8_err_not_supported from `set_gate` rather
 * than leaving the pointer NULL: a NULL op is a malformed binding, not a
 * declined capability, and ::fw_clock_bind rejects it.
 */
typedef struct fw_clock_iface_s {
  /** @brief Rate in Hz of the clock feeding @p module. */
  ra8_err_t (*rate_for)(void *ctx, fw_clock_module_t module, uint32_t *out_hz);
  /** @brief Turn @p module's clock on or off. */
  ra8_err_t (*set_gate)(void *ctx, fw_clock_module_t module, bool on);
  /** @brief Whether @p module exists on this board at all. */
  ra8_err_t (*has_module)(void *ctx, fw_clock_module_t module, bool *out_present);
} fw_clock_iface_t;

/**
 * @struct fw_clock_t
 * @brief Caller-owned binding handle.
 *
 * @details
 * Populated only by ::fw_clock_bind and treated as opaque afterwards. The
 * `bound` flag is what every entry point checks: a zeroed handle is not bound,
 * so a caller that forgets to bind gets ::k_ra8_err_not_initialized rather
 * than a jump through an uninitialised function pointer.
 */
typedef struct fw_clock_s {
  const fw_clock_iface_t *iface; /**< Binding ops, never NULL once bound. */
  void                   *ctx;   /**< Binding context, may be NULL.       */
  bool                    bound; /**< Set by ::fw_clock_bind only.        */
} fw_clock_t;

/**
 * @brief Bind a handle to a chip-and-board clock binding.
 *
 * @param[out] clk   Caller-owned storage, contents ignored on entry.
 * @param[in]  iface Ops struct; borrowed, must outlive @p clk.
 * @param[in]  ctx   Binding context, handed back to every op. May be NULL.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_arg when @p clk or @p iface is
 *         NULL or @p iface leaves an op unset.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_clock_bind(fw_clock_t *clk, const fw_clock_iface_t *iface, void *ctx);

/**
 * @brief Rate of the clock feeding one module.
 *
 * @param[in]  clk    Bound handle.
 * @param[in]  module Which block.
 * @param[out] out_hz On success, the rate in Hz; zero on any failure.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL output or a module
 *         kind outside the enumeration, ::k_ra8_err_not_initialized for an
 *         unbound handle, ::k_ra8_err_not_found when the board has no such
 *         instance, or ::k_ra8_err_invalid_state when the binding reports
 *         success with a zero rate.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_clock_rate_for(const fw_clock_t *clk, fw_clock_module_t module, uint32_t *out_hz);

/**
 * @brief Whether a module's clock meets a floor the caller needs.
 *
 * @details
 * This is intent (2), and it is a check rather than a request: a floor that is
 * not met comes back as ::k_ra8_err_not_supported with the actual rate still
 * written to @p out_hz, so a driver can report what it found instead of
 * guessing. Reprogramming the tree to satisfy the floor is the board's call,
 * never this port's.
 *
 * @param[in]  clk    Bound handle.
 * @param[in]  module Which block.
 * @param[in]  min_hz Floor in Hz; zero is rejected as meaningless.
 * @param[out] out_hz Always written when the rate could be read at all.
 * @return ::k_ra8_ok when the rate is at or above @p min_hz,
 *         ::k_ra8_err_not_supported when it is below, or whatever
 *         ::fw_clock_rate_for reported when the rate could not be read.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_clock_require(const fw_clock_t *clk, fw_clock_module_t module,
                                         uint32_t min_hz, uint32_t *out_hz);

/**
 * @brief Turn one module's clock on.
 *
 * @param[in] clk    Bound handle.
 * @param[in] module Which block.
 * @return ::k_ra8_ok, ::k_ra8_err_not_supported when the binding does not gate
 *         this module, or the same argument and state errors as
 *         ::fw_clock_rate_for.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_clock_enable(const fw_clock_t *clk, fw_clock_module_t module);

/**
 * @brief Turn one module's clock off.
 * @param[in] clk    Bound handle.
 * @param[in] module Which block.
 * @return As ::fw_clock_enable.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_clock_disable(const fw_clock_t *clk, fw_clock_module_t module);

/**
 * @brief Whether the board carries a given module instance.
 *
 * @param[in]  clk         Bound handle.
 * @param[in]  module      Which block.
 * @param[out] out_present On success, whether the instance exists; false on
 *                         any failure.
 * @return ::k_ra8_ok, or the same argument and state errors as
 *         ::fw_clock_rate_for.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_clock_has_module(const fw_clock_t *clk, fw_clock_module_t module,
                                            bool *out_present);

#ifdef __cplusplus
}
#endif
