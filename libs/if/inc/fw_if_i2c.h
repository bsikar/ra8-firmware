/**
 * @file fw_if_i2c.h
 * @brief Architecture-neutral I2C port: bus speed configuration, addressed
 * transmit and addressed receive behind one narrow vtable.
 * @ingroup grp_fw_i2c
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * Portable code needs an RTC, a sensor and an EEPROM, and every chip spells
 * its I2C block differently. This port carries the three intents such a
 * driver actually has -- configure a bus speed, send bytes to a target,
 * receive bytes from a target -- and leaves the mechanism to a binding
 * supplied by the chip and board.
 *
 * Buses are named in *board* numbering, dense and zero-based: a board
 * presents its wired buses as bus 0, bus 1, ... however the silicon
 * allocates them, and its binding maps them. That is the same rule the clock
 * port set for module instances and the UART and SPI ports set for their
 * lines, and how many buses exist is a board fact, so the membrane does not
 * range-check the number; the binding owns it and answers
 * ::k_ra8_err_not_found for one it does not carry.
 *
 * Target addresses are the 7-bit device address with no R/W bit in it, which
 * is how the datasheets print them; the membrane rejects anything wider.
 * Multi-byte addressing, repeated starts and clock stretching are mechanism,
 * and a combined write-then-read transaction waits for a first-party caller
 * the same way the critical section does: this port does not declare it on
 * speculation.
 *
 * The speed description is deliberately small: one bus speed. Bus idle
 * timeouts, analog filtering, drive strength and pull-up policy are chip and
 * board policy and belong in a chip-specific binding header, which the
 * composition root may name because the composition root already knows which
 * chip it picked.
 *
 * Transmit and receive are all-or-error per call: ::fw_i2c_write returns
 * ::k_ra8_ok only when the whole buffer was clocked out, and ::fw_i2c_read
 * fills the whole buffer. A zero-length call is a no-op that wakes nothing.
 *
 * Per the taxonomy in `docs/PORTS.md`, this uses (a) the caller-allocated
 * facade plus (b) a narrow ops struct the binding fills. Handles are
 * caller-owned. This interface performs no allocation and contains no chip,
 * operating-system or device header.
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
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum fw_i2c_speed_t
 * @brief Bus speed, named as the spec names it.
 */
typedef enum : uint8_t {
  k_fw_i2c_speed_none      = 0U, /**< Not a speed; rejected.        */
  k_fw_i2c_speed_std       = 1U, /**< 100 kbit/s standard mode.     */
  k_fw_i2c_speed_fast      = 2U, /**< 400 kbit/s fast mode.         */
  k_fw_i2c_speed_fast_plus = 3U, /**< 1 Mbit/s fast-mode plus.      */
} fw_i2c_speed_t;

/** @brief One past the last valid ::fw_i2c_speed_t enumerator. */
#define K_FW_I2C_SPEED_COUNT 4U

/**
 * @struct fw_i2c_config_s
 * @brief Everything the port says about a bus.
 */
typedef struct fw_i2c_config_s {
  fw_i2c_speed_t speed; /**< Bus speed. Zero is the unconfigured shape. */
} fw_i2c_config_t;

/**
 * @struct fw_i2c_iface_s
 * @brief The narrow ops struct a chip-and-board binding fills.
 *
 * @details
 * Three entries, because the port has three intents. Each takes the binding's
 * own context as its first argument, so one binding can serve more than one
 * instance without a file-scope variable. A NULL op is a malformed binding,
 * not a declined capability, and ::fw_i2c_bind rejects it.
 */
typedef struct fw_i2c_iface_s {
  /** @brief Put @p bus on @p cfg's speed. */
  ra8_err_t (*configure)(void *ctx, uint8_t bus, fw_i2c_config_t cfg);
  /** @brief Send @p len bytes to the 7-bit target on @p bus. */
  ra8_err_t (*write)(void *ctx, uint8_t bus, uint8_t target,
                     const uint8_t *bytes, size_t len);
  /** @brief Receive @p len bytes from the 7-bit target on @p bus. */
  ra8_err_t (*read)(void *ctx, uint8_t bus, uint8_t target, uint8_t *out,
                    size_t len);
} fw_i2c_iface_t;

/**
 * @struct fw_i2c_s
 * @brief Caller-owned binding handle.
 *
 * @details
 * Populated only by ::fw_i2c_bind and treated as opaque afterwards. The
 * `bound` flag is what every entry point checks: a zeroed handle is not bound,
 * so a caller that forgets to bind gets ::k_ra8_err_not_initialized rather
 * than a jump through an uninitialised function pointer.
 */
typedef struct fw_i2c_s {
  const fw_i2c_iface_t *iface; /**< Binding ops, never NULL once bound. */
  void                   *ctx;  /**< Binding context, may be NULL.       */
  bool                    bound; /**< Set by ::fw_i2c_bind only.         */
} fw_i2c_t;

/**
 * @brief Bind a handle to a chip-and-board I2C binding.
 *
 * @param[out] bus   Caller-owned storage, contents ignored on entry.
 * @param[in]  iface Ops struct; borrowed, must outlive @p bus.
 * @param[in]  ctx   Binding context, handed back to every op. May be NULL.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_arg when @p bus or @p iface is
 *         NULL or @p iface leaves an op unset.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_i2c_bind(fw_i2c_t *bus, const fw_i2c_iface_t *iface, void *ctx);

/**
 * @brief Put one bus on a speed.
 *
 * @param[in] bus_handle Bound handle.
 * @param[in] bus        Bus in board numbering.
 * @param[in] cfg        Speed description; an out-of-range speed is rejected
 *                       before the binding is reached.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for an unbound handle or a
 *         malformed cfg, ::k_ra8_err_not_found when the board has no such
 *         bus, or whatever the binding reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_i2c_configure(const fw_i2c_t *bus_handle, uint8_t bus, fw_i2c_config_t cfg);

/**
 * @brief Send bytes to one target on a bus.
 *
 * @details
 * All-or-error: on ::k_ra8_ok the whole buffer was clocked out and
 * acknowledged. On any error the binding tells what it could not do, so a
 * caller never tracks a partial send. A zero-length write is satisfied
 * without waking the bus.
 *
 * @param[in] bus_handle Bound handle.
 * @param[in] bus        Bus in board numbering.
 * @param[in] target     7-bit target address, no R/W bit.
 * @param[in] bytes      Buffer to send; may be NULL only when @p len is zero.
 * @param[in] len        Bytes to send.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for an address wider than 7
 *         bits, a NULL buffer with a nonzero length, or an unbound handle,
 *         or whatever the binding reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_i2c_write(const fw_i2c_t *bus_handle, uint8_t bus, uint8_t target,
             const uint8_t *bytes, size_t len);

/**
 * @brief Receive bytes from one target on a bus.
 *
 * @details
 * All-or-error: on ::k_ra8_ok the whole buffer was filled. On any error the
 * caller's buffer is whatever the binding left it, and the caller retries
 * the whole read.
 *
 * @param[in]  bus_handle Bound handle.
 * @param[in]  bus        Bus in board numbering.
 * @param[in]  target     7-bit target address, no R/W bit.
 * @param[out] out        Buffer to fill; may be NULL only when @p len is
 *                        zero.
 * @param[in]  len        Bytes to receive.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for an address wider than 7
 *         bits, a NULL buffer with a nonzero length, or an unbound handle,
 *         or whatever the binding reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_i2c_read(const fw_i2c_t *bus_handle, uint8_t bus, uint8_t target,
            uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif
