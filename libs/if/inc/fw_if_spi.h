/**
 * @file fw_if_spi.h
 * @brief Architecture-neutral SPI port: chip-select configuration, transmit
 * and full-duplex exchange behind one narrow vtable.
 * @ingroup grp_fw_spi
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * Portable code needs an SPI flash, a display panel and an SD card, and every
 * chip spells its serial peripheral block differently. This port carries the
 * three intents such a driver actually has -- configure a chip-select line,
 * transmit bytes, exchange bytes full-duplex -- and leaves the mechanism to a
 * binding supplied by the chip and board.
 *
 * SPI devices are named in *board* numbering, dense and zero-based: a board
 * presents its wired peripherals as device 0, device 1, ... however the
 * silicon and the mux allocate chip selects, and its binding maps them. That
 * is the same rule the clock port set for module instances and the UART port
 * set for serial lines, and how many devices exist is a board fact, so the
 * membrane does not range-check the number; the binding owns it and answers
 * ::k_ra8_err_not_found for one it does not carry.
 *
 * The line description is deliberately small: clock rate and clock phase
 * mode. Chip-select polarity, data frame size, DMA wiring and software or
 * hardware CS management are chip and board policy and belong in a
 * chip-specific binding header, which the composition root may name because
 * the composition root already knows which chip it picked.
 *
 * Transmit is all-or-error: ::fw_spi_write returns ::k_ra8_ok only when the
 * whole buffer is gone, so a caller never tracks a partial send. Exchange is
 * synchronous and symmetric: ::fw_spi_exchange shifts the transmit bytes out
 * while shifting receive bytes in, one length for both, and reports ::k_ra8_ok
 * only when every byte round-tripped. Neither call returns early with a
 * partial transfer; a driver that wants chunked transfer composes the calls.
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
 * @enum fw_spi_mode_t
 * @brief Clock polarity and phase, numbered as the datasheets number them.
 */
typedef enum : uint8_t {
  k_fw_spi_mode_none = 0U, /**< Not a mode; rejected.          */
  k_fw_spi_mode_0    = 1U, /**< CPOL 0, CPHA 0.                */
  k_fw_spi_mode_1    = 2U, /**< CPOL 0, CPHA 1.                */
  k_fw_spi_mode_2    = 3U, /**< CPOL 1, CPHA 0.                */
  k_fw_spi_mode_3    = 4U, /**< CPOL 1, CPHA 1.                */
} fw_spi_mode_t;

/** @brief One past the last valid ::fw_spi_mode_t enumerator. */
#define K_FW_SPI_MODE_COUNT 5U

/**
 * @struct fw_spi_config_s
 * @brief Everything the port says about a chip-select line.
 */
typedef struct fw_spi_config_s {
  uint32_t      clock_hz; /**< Peripheral clock ceiling in Hz; zero refused. */
  fw_spi_mode_t mode;     /**< Clock polarity and phase.                     */
} fw_spi_config_t;

/**
 * @struct fw_spi_iface_s
 * @brief The narrow ops struct a chip-and-board binding fills.
 *
 * @details
 * Three entries, because the port has three intents. Each takes the binding's
 * own context as its first argument, so one binding can serve more than one
 * instance without a file-scope variable. A NULL op is a malformed binding,
 * not a declined capability, and ::fw_spi_bind rejects it.
 */
typedef struct fw_spi_iface_s {
  /** @brief Put @p dev on @p cfg's clock rate and mode. */
  ra8_err_t (*configure)(void *ctx, uint8_t dev, fw_spi_config_t cfg);
  /** @brief Transmit @p len bytes. Returns ok only when all of them went. */
  ra8_err_t (*write)(void *ctx, uint8_t dev, const uint8_t *bytes, size_t len);
  /** @brief Shift @p len bytes out of @p tx and into @p rx in one exchange. */
  ra8_err_t (*exchange)(void *ctx, uint8_t dev, const uint8_t *tx, uint8_t *rx,
                        size_t len);
} fw_spi_iface_t;

/**
 * @struct fw_spi_s
 * @brief Caller-owned binding handle.
 *
 * @details
 * Populated only by ::fw_spi_bind and treated as opaque afterwards. The
 * `bound` flag is what every entry point checks: a zeroed handle is not bound,
 * so a caller that forgets to bind gets ::k_ra8_err_not_initialized rather
 * than a jump through an uninitialised function pointer.
 */
typedef struct fw_spi_s {
  const fw_spi_iface_t *iface; /**< Binding ops, never NULL once bound. */
  void                   *ctx;  /**< Binding context, may be NULL.       */
  bool                    bound; /**< Set by ::fw_spi_bind only.         */
} fw_spi_t;

/**
 * @brief Bind a handle to a chip-and-board SPI binding.
 *
 * @param[out] dev   Caller-owned storage, contents ignored on entry.
 * @param[in]  iface Ops struct; borrowed, must outlive @p dev.
 * @param[in]  ctx   Binding context, handed back to every op. May be NULL.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_arg when @p dev or @p iface is
 *         NULL or @p iface leaves an op unset.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_spi_bind(fw_spi_t *dev, const fw_spi_iface_t *iface, void *ctx);

/**
 * @brief Put one device on a clock rate and mode.
 *
 * @param[in] dev        Bound handle.
 * @param[in] dev_index  Device in board numbering.
 * @param[in] cfg        Line description; a zero clock or an out-of-range
 *                       mode is rejected before the binding is reached.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for an unbound handle or a
 *         malformed cfg, ::k_ra8_err_not_found when the board has no such
 *         device, or whatever the binding reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_spi_configure(const fw_spi_t *dev, uint8_t dev_index, fw_spi_config_t cfg);

/**
 * @brief Transmit bytes to one device.
 *
 * @details
 * All-or-error: on ::k_ra8_ok the whole buffer is gone, and on any error the
 * binding tells what it could not do, so a caller never tracks a partial
 * send. A zero-length write is satisfied without waking the peripheral.
 *
 * @param[in] dev       Bound handle.
 * @param[in] dev_index Device in board numbering.
 * @param[in] bytes     Buffer to send; may be NULL only when @p len is zero.
 * @param[in] len       Bytes to send.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL buffer with a
 *         nonzero length or an unbound handle, or whatever the binding
 *         reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_spi_write(const fw_spi_t *dev, uint8_t dev_index, const uint8_t *bytes,
             size_t len);

/**
 * @brief Exchange bytes with one device, full duplex.
 *
 * @details
 * The two buffers are the same length and move in the same call: byte @c n
 * out of @p tx rides the same clock as byte @c n into @p rx. On
 * ::k_ra8_ok every byte round-tripped; on any error the caller's buffers are
 * whatever the binding left them, and the caller retries the whole exchange.
 * Receive-only and transmit-only variants are @p tx or @p rx work the
 * binding does with its own filler; the portable call is symmetric on
 * purpose.
 *
 * @param[in]  dev       Bound handle.
 * @param[in]  dev_index Device in board numbering.
 * @param[in]  tx        Bytes to send.
 * @param[out] rx        Buffer for the bytes the device returns.
 * @param[in]  len       Bytes to exchange, one length for both buffers.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL buffer with a
 *         nonzero length or an unbound handle, or whatever the binding
 *         reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_spi_exchange(const fw_spi_t *dev, uint8_t dev_index, const uint8_t *tx,
                uint8_t *rx, size_t len);

#ifdef __cplusplus
}
#endif
